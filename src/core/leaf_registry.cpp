#include "leaf_registry.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include <google/protobuf/util/message_differencer.h>

namespace gnmid::core {

void LeafRegistry::registerGroup(const std::string& name, const std::string& prefix, bool atomic,
                                 std::optional<LeafType> preferredType) {
    std::unique_lock lock(mutex_);
    registerGroupLocked(name, prefix, atomic, preferredType);
}

LeafId LeafRegistry::registerLeaf(const std::string& path, std::optional<LeafType> type,
                                  std::optional<gnmi::TypedValue> initialValue) {
    std::unique_lock lock(mutex_);
    return registerLeafLocked(path, type, std::move(initialValue));
}

void LeafRegistry::registerGroupLocked(const std::string& name, const std::string& prefix,
                                       bool atomic, std::optional<LeafType> preferredType) {
    CanonicalPath canonPrefix = canonicalize(prefix);

    if (groups_.count(name)) {
        throw std::invalid_argument("registerGroup: duplicate group name '" + name + "'");
    }

    // D5: no two group prefixes may nest (one being a path-ancestor-or-equal of the
    // other). The message states WHY, so a caller hitting it understands the rule
    // without digging into the design doc.
    if (const CanonicalPath* clash = overlappingPrefix(canonPrefix)) {
        throw std::invalid_argument(
            "registerGroup: prefix '" + canonPrefix.str() + "' overlaps existing prefix '" +
            clash->str() +
            "' — group prefixes must not nest (one a path-ancestor of the other): an atomic "
            "notification for the outer prefix declares the COMPLETE state of its subtree, so "
            "the inner group's leaves would be implicitly deleted client-side (gNMI §2.1.1), "
            "and auto-assign would be ambiguous. Sibling prefixes (e.g. /a/b and /a/bc) are "
            "fine; only ancestor/descendant overlap is rejected. (wouldConflict() lets you "
            "check ahead of time.)");
    }

    auto [it, inserted] = groups_.try_emplace(name, RegistryAccess{}, name, std::move(canonPrefix),
                                              atomic, preferredType);
    NotificationGroup& group = it->second;
    groupsByPrefix_.emplace(group.prefix(), &group);
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
    // Longest-first so the most specific group wins; D5 makes the choice unique.
    for (std::string_view ancestor : ancestorPrefixes(path)) {
        auto it = groupsByPrefix_.find(ancestor);
        if (it != groupsByPrefix_.end()) {
            return it->second;
        }
    }
    return nullptr;
}

const CanonicalPath* LeafRegistry::overlappingPrefix(const CanonicalPath& prefix) const {
    for (const auto& [existing, _] : groupsByPrefix_) {
        if (isUnderPrefix(prefix, existing) || isUnderPrefix(existing, prefix)) {
            return &existing;
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
    GroupView view{.name          = group.name(),
                   .prefix        = group.prefix().str(),
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

std::optional<GroupView> LeafRegistry::getGroup(const std::string& name) const {
    std::shared_lock lock(mutex_);

    auto it = groups_.find(name);
    if (it == groups_.end()) {
        return std::nullopt;
    }
    return viewOf(it->second);
}

std::vector<std::string> LeafRegistry::registeredPrefixes() const {
    std::shared_lock lock(mutex_);

    std::vector<std::string> prefixes;
    prefixes.reserve(groupsByPrefix_.size());
    for (const auto& [prefix, _] : groupsByPrefix_) {  // map is ordered, so already sorted
        prefixes.push_back(prefix.str());
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

    // Keys sharing canonPrefix as a STRING prefix form a contiguous block from
    // lower_bound; isUnderPrefix then keeps only true subtree members (excludes
    // /a/bc under /a/b — spec §2.4.2 node + children, not string prefix).
    LeafSnapshot snapshot;
    for (auto it = leaves_.lower_bound(canonPrefix); it != leaves_.end(); ++it) {
        const CanonicalPath& path = *it->first;
        if (!path.str().starts_with(canonPrefix.str())) {
            break;
        }
        if (isUnderPrefix(canonPrefix, path)) {
            snapshot.emplace(path.str(), snapshotOf(it->second));
        }
    }
    return snapshot;
}

SubscriptionView LeafRegistry::collectForSubscription(const std::string& query) const {
    const CanonicalPath canonQuery = canonicalize(query);

    std::shared_lock lock(mutex_);

    SubscriptionView view;
    std::unordered_set<const NotificationGroup*> seen;
    for (auto it = leaves_.lower_bound(canonQuery); it != leaves_.end(); ++it) {
        const CanonicalPath& path = *it->first;
        if (!path.str().starts_with(canonQuery.str())) {
            break;
        }
        if (!isUnderPrefix(canonQuery, path)) {
            continue;
        }
        const LeafEntry& leaf = it->second;
        view.leaves.emplace(path.str(), snapshotOf(leaf));
        if (const NotificationGroup* g = leaf.group(); g && seen.insert(g).second) {
            view.groups.push_back(viewOf(*g));
        }
    }
    return view;
}

std::vector<LeafId> LeafRegistry::attachSubtree(const SubtreeSpec& spec) {
    std::unique_lock lock(mutex_);

    // Groups before leaves, so the leaves auto-assign within the same branch (D3).
    for (const auto& g : spec.groups) {
        registerGroupLocked(g.name, g.prefix, g.atomic, g.preferredType);
    }
    std::vector<LeafId> ids;
    ids.reserve(spec.leaves.size());
    for (const auto& l : spec.leaves) {
        ids.push_back(registerLeafLocked(l.path, l.type, l.initialValue));
    }
    return ids;
}

void LeafRegistry::detachSubtree(const std::string& prefix) {
    const CanonicalPath canonPrefix = canonicalize(prefix);

    std::unique_lock lock(mutex_);

    // Remove every leaf under the branch, unlinking from its group first (D1).
    for (auto it = leaves_.lower_bound(canonPrefix); it != leaves_.end();) {
        const CanonicalPath& path = *it->first;
        if (!path.str().starts_with(canonPrefix.str())) {
            break;
        }
        if (isUnderPrefix(canonPrefix, path)) {
            LeafEntry& leaf = it->second;
            if (leaf.group_ != nullptr) {
                leaf.group_->unlinkLeaf(&leaf);
            }
            it = leaves_.erase(it);
        } else {
            ++it;
        }
    }

    // Remove every group whose prefix is under the branch (its members are already
    // gone). A group with a broader prefix outside the branch survives, minus the
    // leaves just unlinked.
    for (auto it = groupsByPrefix_.lower_bound(canonPrefix); it != groupsByPrefix_.end();) {
        const CanonicalPath& groupPrefix = it->first;
        if (!groupPrefix.str().starts_with(canonPrefix.str())) {
            break;
        }
        if (isUnderPrefix(canonPrefix, groupPrefix)) {
            const std::string name = it->second->name();
            it = groupsByPrefix_.erase(it);
            groups_.erase(name);
        } else {
            ++it;
        }
    }
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

void LeafRegistry::unregisterGroup(const std::string& name) {
    std::unique_lock lock(mutex_);

    auto it = groups_.find(name);
    if (it == groups_.end()) {
        return;
    }
    NotificationGroup& group = it->second;
    // Members survive as ungrouped, independent units (D9) — only the back-pointer
    // is cleared, the leaf data and value stay.
    for (LeafEntry* member : group.members()) {
        member->group_ = nullptr;
    }
    groupsByPrefix_.erase(group.prefix());
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
