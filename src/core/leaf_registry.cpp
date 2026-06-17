#include "leaf_registry.hpp"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include <google/protobuf/util/message_differencer.h>

namespace gnmid::core {

namespace {

// Visits, in sorted order, every entry of `m` whose key is element-aligned under `prefix`
// (the §2.4.2 subtree boundary). Keys sharing `prefix` as a STRING prefix form one
// contiguous block from lower_bound; the starts_with check ends it, and isUnderPrefix
// filters out non-element-aligned siblings (e.g. /a/bc under /a/b — not a subtree member).
// `fn(it)` processes a matching entry and returns the iterator to continue from:
// std::next(it) for a read pass, or m.erase(it) for a removing pass. Because fn MUST
// return an iterator, a read pass cannot forget to advance (it would not compile).
// Works for both leaves_ and groups_ — they share the shared_ptr<const CanonicalPath> key.
template <class Map, class Fn>
void forEachUnderPrefix(Map& m, const CanonicalPath& prefix, Fn&& fn) {
    for (auto it = m.lower_bound(prefix); it != m.end();) {
        const CanonicalPath& key = *it->first;
        if (!key.str().starts_with(prefix.str())) {
            break;
        }
        if (isUnderPrefix(prefix, key)) {
            it = fn(it);
        } else {
            ++it;
        }
    }
}

}  // namespace

void LeafRegistry::registerGroup(const std::string& prefix, bool atomic,
                                 std::optional<LeafType> preferredType) {
    std::unique_lock lock(mutex_);
    registerGroupLocked(prefix, atomic, preferredType);
}

LeafId LeafRegistry::registerLeaf(const std::string& path, std::optional<LeafType> type,
                                  std::optional<gnmi::TypedValue> initialValue) {
    std::unique_lock lock(mutex_);
    return registerLeafLocked(path, type, std::move(initialValue));
}

void LeafRegistry::registerGroupLocked(const std::string& prefix, bool atomic,
                                       std::optional<LeafType> preferredType) {
    CanonicalPath canonPrefix = canonicalize(prefix);

    // D5: no two group prefixes may nest (one being a path-ancestor-or-equal of the
    // other) — the equal case also rejects a duplicate group (D4: the prefix is the group's
    // sole identity). The message states WHY, so a caller hitting it understands the rule
    // without digging into the design doc.
    if (const CanonicalPath* clash = overlappingPrefix(canonPrefix)) {
        throw std::invalid_argument(
            "registerGroup: prefix '" + canonPrefix.str() + "' overlaps existing prefix '" +
            clash->str() +
            "' — group prefixes must not nest (one a path-ancestor of the other): an atomic "
            "notification for the outer prefix declares the COMPLETE state of its subtree, so "
            "the inner group's leaves would be implicitly deleted client-side (gNMI §2.1.1). "
            "Only atomic nesting is spec-forced; we reject ALL nesting as a deliberate "
            "simplification (non-atomic nesting is unambiguous via the D3 longest-ancestor "
            "rule, but unsupported). Sibling prefixes (e.g. /a/b and /a/bc) are fine; only "
            "ancestor/descendant overlap is rejected. (wouldConflict() lets you check ahead "
            "of time.)");
    }

    // Materialise the prefix ONCE; the same handle becomes the map key and the group's
    // prefix_ (L=B), mirroring registerLeafLocked.
    auto prefixHandle = std::make_shared<const CanonicalPath>(std::move(canonPrefix));
    groups_.try_emplace(prefixHandle, RegistryAccess{}, prefixHandle, atomic, preferredType);
}

LeafId LeafRegistry::registerLeafLocked(const std::string& path, std::optional<LeafType> type,
                                        std::optional<gnmi::TypedValue> initialValue) {
    CanonicalPath canonPath = canonicalize(path);

    if (leaves_.find(canonPath) != leaves_.end()) {
        throw std::invalid_argument("registerLeaf: duplicate leaf path '" + canonPath.str() + "'");
    }

    // Materialise the canonical path ONCE; the same handle becomes the map key, the
    // entry's path_, and the returned LeafId (L=B).
    auto pathHandle = std::make_shared<const CanonicalPath>(std::move(canonPath));
    auto valueHandle = initialValue
                           ? std::make_shared<const gnmi::TypedValue>(std::move(*initialValue))
                           : std::shared_ptr<const gnmi::TypedValue>{};

    auto [it, inserted] =
        leaves_.try_emplace(pathHandle, RegistryAccess{}, pathHandle, type, std::move(valueHandle));
    LeafEntry& leaf = it->second;

    // D3 auto-assign: attach to the single group whose prefix is a path-ancestor of
    // this leaf. D5 guarantees at most one such group exists.
    if (NotificationGroup* owner = findOwningGroup(leaf.path())) {
        owner->linkLeaf(&leaf);
    }

    return LeafId(std::move(pathHandle));
}

NotificationGroup* LeafRegistry::findOwningGroup(const CanonicalPath& path) {
    // Longest-first so the most specific group wins; D5 makes the choice unique. Probes
    // groups_ by a string_view ancestor slice (DerefLess transparent) — no key materialised.
    for (std::string_view ancestor : ancestorPrefixes(path)) {
        auto it = groups_.find(ancestor);
        if (it != groups_.end()) {
            return &it->second;
        }
    }
    return nullptr;
}

const CanonicalPath* LeafRegistry::overlappingPrefix(const CanonicalPath& prefix) const {
    for (const auto& [existing, _] : groups_) {
        if (isUnderPrefix(prefix, *existing) || isUnderPrefix(*existing, prefix)) {
            return existing.get();
        }
    }
    return nullptr;
}

LeafValueSnapshot LeafRegistry::snapshotOf(const LeafEntry& leaf) const {
    return LeafValueSnapshot{.value         = leaf.value(),
                             .collectedNs   = leaf.collectedNs(),
                             .changeSeq     = leaf.changeSeq(),
                             .effectiveType = leaf.effectiveType()};
}

GroupView LeafRegistry::viewOf(const NotificationGroup& group) const {
    GroupView view{.prefix        = group.prefix().str(),
                   .atomic        = group.atomic(),
                   .preferredType = group.preferredType(),
                   .memberPaths   = {}};
    view.memberPaths.reserve(group.members().size());
    for (const LeafEntry* member : group.members()) {
        view.memberPaths.push_back(member->path().str());
    }
    std::sort(view.memberPaths.begin(), view.memberPaths.end());  // members() order is non-deterministic
    return view;
}

std::optional<LeafValueSnapshot> LeafRegistry::getLeaf(const std::string& path) const {
    const CanonicalPath canonPath = canonicalize(path);

    std::shared_lock lock(mutex_);

    auto it = leaves_.find(canonPath);
    if (it == leaves_.end()) {
        return std::nullopt;
    }
    return snapshotOf(it->second);
}

std::optional<LeafValueSnapshot> LeafRegistry::getLeaf(const LeafId& id) const {
    if (!id.valid()) {
        return std::nullopt;
    }

    std::shared_lock lock(mutex_);

    auto it = leaves_.find(id.path());
    if (it == leaves_.end()) {
        return std::nullopt;
    }
    return snapshotOf(it->second);
}

std::optional<GroupView> LeafRegistry::getGroup(const std::string& prefix) const {
    const CanonicalPath canonPrefix = canonicalize(prefix);

    std::shared_lock lock(mutex_);

    auto it = groups_.find(canonPrefix);
    if (it == groups_.end()) {
        return std::nullopt;
    }
    return viewOf(it->second);
}

std::vector<std::string> LeafRegistry::registeredPrefixes() const {
    std::shared_lock lock(mutex_);

    std::vector<std::string> prefixes;
    prefixes.reserve(groups_.size());
    for (const auto& [prefix, _] : groups_) {  // map is ordered, so already sorted
        prefixes.push_back(prefix->str());
    }
    return prefixes;
}

std::optional<std::string> LeafRegistry::wouldConflict(const std::string& prefix) const {
    const CanonicalPath canonPrefix = canonicalize(prefix);

    std::shared_lock lock(mutex_);

    if (const CanonicalPath* clash = overlappingPrefix(canonPrefix)) {
        return clash->str();
    }
    return std::nullopt;
}

LeafSnapshot LeafRegistry::collectLeaves(const std::string& prefix) const {
    const CanonicalPath canonPrefix = canonicalize(prefix);

    std::shared_lock lock(mutex_);

    // §2.4.2 subtree members under canonPrefix (see forEachUnderPrefix for the boundary).
    LeafSnapshot snapshot;
    forEachUnderPrefix(leaves_, canonPrefix, [&](auto it) {
        snapshot.emplace(it->first->str(), snapshotOf(it->second));
        return std::next(it);
    });
    return snapshot;
}

SubscriptionView LeafRegistry::collectForSubscription(const std::string& query) const {
    const CanonicalPath canonQuery = canonicalize(query);

    std::shared_lock lock(mutex_);

    SubscriptionView view;
    std::unordered_set<const NotificationGroup*> seen;
    forEachUnderPrefix(leaves_, canonQuery, [&](auto it) {
        const LeafEntry& leaf = it->second;
        view.leaves.emplace(it->first->str(), snapshotOf(leaf));
        if (const NotificationGroup* g = leaf.group(); g && seen.insert(g).second) {
            view.groups.push_back(viewOf(*g));
        }
        return std::next(it);
    });
    return view;
}

std::map<std::string, LeafId> LeafRegistry::attachSubtree(const SubtreeSpec& spec) {
    std::unique_lock lock(mutex_);

    // Groups before leaves, so the leaves auto-assign within the same branch (D3).
    for (const auto& g : spec.groups) {
        registerGroupLocked(g.prefix, g.atomic, g.preferredType);
    }
    std::map<std::string, LeafId> ids;
    for (const auto& l : spec.leaves) {
        LeafId id = registerLeafLocked(l.path, l.type, l.initialValue);
        ids.emplace(canonicalize(l.path).str(), id);  // addressable by canonical path, not spec order
    }
    return ids;
}

void LeafRegistry::detachSubtree(const std::string& prefix) {
    const CanonicalPath canonPrefix = canonicalize(prefix);

    std::unique_lock lock(mutex_);

    // Remove every leaf under the branch, unlinking from its group first (D1).
    forEachUnderPrefix(leaves_, canonPrefix, [&](auto it) {
        LeafEntry& leaf = it->second;
        if (leaf.group_ != nullptr) {
            leaf.group_->unlinkLeaf(&leaf);
        }
        return leaves_.erase(it);
    });

    // Remove every group whose prefix is under the branch (its members are already gone).
    // A group with a broader prefix outside the branch survives, minus the leaves just unlinked.
    forEachUnderPrefix(groups_, canonPrefix, [&](auto it) { return groups_.erase(it); });
}

void LeafRegistry::unregisterLeaf(const std::string& path) {
    const CanonicalPath canonPath = canonicalize(path);

    std::unique_lock lock(mutex_);

    auto it = leaves_.find(canonPath);
    if (it == leaves_.end()) {
        return;
    }
    LeafEntry& leaf = it->second;
    if (leaf.group_ != nullptr) {
        leaf.group_->unlinkLeaf(&leaf);  // unlink BEFORE erase (D1 erase order)
    }
    leaves_.erase(it);
}

void LeafRegistry::unregisterGroup(const std::string& prefix) {
    const CanonicalPath canonPrefix = canonicalize(prefix);

    std::unique_lock lock(mutex_);

    auto it = groups_.find(canonPrefix);
    if (it == groups_.end()) {
        return;
    }
    NotificationGroup& group = it->second;
    // Members survive as ungrouped, independent units (D9) — only the back-pointer
    // is cleared, the leaf data and value stay.
    for (LeafEntry* member : group.members()) {
        member->group_ = nullptr;
    }
    groups_.erase(it);
}

ValueWriter LeafRegistry::writeValues() {
    return ValueWriter(*this, std::unique_lock<std::shared_mutex>(mutex_));
}

bool LeafRegistry::setValueLocked(const LeafId& id, gnmi::TypedValue value, int64_t collectedNs) {
    if (!id.valid()) {
        return false;
    }
    auto it = leaves_.find(id.path());
    if (it == leaves_.end()) {
        return false;  // stale id — leaf detached
    }
    LeafEntry& leaf = it->second;

    // Value-gated (D14): a re-pushed identical value is a no-op, so static sensors
    // never spuriously fire ON_CHANGE. The registry, not the provider, decides what
    // counts as a change.
    if (leaf.value_ &&
        google::protobuf::util::MessageDifferencer::Equals(*leaf.value_, value)) {
        return true;
    }

    // Real change: install a NEW immutable version (never mutate in place, so earlier
    // snapshots stay valid — D17) and advance the global change token.
    leaf.value_       = std::make_shared<const gnmi::TypedValue>(std::move(value));
    leaf.collectedNs_ = collectedNs;
    leaf.changeSeq_   = ++globalSeq_;
    return true;
}

bool ValueWriter::set(const LeafId& id, gnmi::TypedValue value, int64_t collectedNs) {
    return reg_->setValueLocked(id, std::move(value), collectedNs);
}

}  // namespace gnmid::core
