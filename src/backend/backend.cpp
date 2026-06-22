#include "backend/backend.hpp"

#include <utility>

#include "canonical_path.hpp"

namespace gnmid {

namespace {

// Routing / list-fan-out matching now lives in core (canonical_path.hpp), shared
// with the push hub so setup-time and change-time routing cannot drift. Bring the
// names into this scope so the call sites below read unchanged.
using core::ownsPath;
using core::selects;

}  // namespace

std::string Backend::canon(const std::string& xpath) {
    return core::canonicalize(xpath).str();
}

void Backend::addProvider(std::unique_ptr<Provider> p) {
    ownedPrefixes_.push_back(core::canonicalize(p->domainPrefix()));
    p->start();                       // provider already registered its leaves in its ctor
    providers_.push_back(std::move(p));
}

void Backend::injectHardwareEvent(const std::string& unit, bool present) {
    // Fan out to every provider; the one that owns the unit acts, the rest no-op.
    // Out-of-band hardware events have no path, so there is nothing to route on —
    // a fan-out is both correct and trivially decoupled at this scale.
    for (auto& p : providers_)
        p->onHardwareEvent(unit, present);
}

void Backend::declareGroup(const std::string& prefix, bool atomic) {
    registry_.registerGroup(prefix, atomic);
}

core::LeafId Backend::declareLeaf(const std::string& xpath, core::LeafType type,
                                  std::optional<gnmi::TypedValue> value) {
    std::lock_guard lk(bindingsMu_);
    core::LeafId id = registry_.registerLeaf(xpath, type, std::move(value));
    const std::string c = canon(xpath);
    ids_[c] = id;
    if (type == core::LeafType::Config) writableConfig_.insert(c);
    return id;
}

std::map<std::string, core::LeafId>
Backend::attachSubtree(const core::SubtreeSpec& spec) {
    std::lock_guard lk(bindingsMu_);
    std::map<std::string, core::LeafId> ids = registry_.attachSubtree(spec);
    for (const auto& [path, id] : ids) {        // core returns canonical-path keys
        ids_[path] = id;
        if (auto snap = registry_.getLeaf(id);
            snap && snap->effectiveType == core::LeafType::Config)
            writableConfig_.insert(path);
    }
    return ids;
}

void Backend::detachSubtree(const std::string& prefix) {
    const std::string p = canon(prefix);
    std::lock_guard lk(bindingsMu_);
    registry_.detachSubtree(prefix);            // registry canonicalizes the prefix itself
    for (auto it = ids_.begin(); it != ids_.end(); )
        if (ownsPath(p, it->first)) it = ids_.erase(it); else ++it;
    for (auto it = writableConfig_.begin(); it != writableConfig_.end(); )
        if (ownsPath(p, *it)) it = writableConfig_.erase(it); else ++it;
}

bool Backend::routed(const std::string& xpath) const {
    const std::string path = canon(xpath);
    for (const auto& prefix : ownedPrefixes_)
        if (ownsPath(prefix.str(), path)) return true;
    return false;
}

bool Backend::writable(const std::string& xpath) const {
    std::lock_guard lk(bindingsMu_);
    return writableConfig_.count(canon(xpath)) > 0;
}

Backend::View Backend::snapshot(const std::string& xpath) const {
    View v;
    v.routed = routed(xpath);
    if (!v.routed) return v;

    core::SubscriptionView sub = registry_.collectForSubscription(xpath);
    for (auto& [path, snap] : sub.leaves)
        if (snap.value) v.leaves.emplace(path, snap);   // skip unset — never hits the wire
    v.groups = std::move(sub.groups);

    // Key-omitted list query (no predicate) that matched nothing exactly: fan out
    // to all entries of the same shape (old LeafStore::selects). The element-aligned
    // core won't cross a bare-list -> keyed-entry boundary, so do the expansion here
    // (boundary layer; device-modelling-conventions §5/§6).
    const std::string q = canon(xpath);
    if (v.leaves.empty() && q.find('[') == std::string::npos) {
        core::SubscriptionView all = registry_.collectForSubscription("");  // root
        for (auto& [path, snap] : all.leaves)
            if (snap.value && selects(q, path)) v.leaves.emplace(path, snap);
        v.groups = std::move(all.groups);
    }
    return v;
}

gnmi::SubscriptionMode Backend::preferredMode(const std::string& xpath) const {
    const core::LeafSnapshot leaves = registry_.collectLeaves(xpath);
    if (leaves.empty()) return gnmi::SAMPLE;
    for (const auto& [path, snap] : leaves)
        if (snap.effectiveType != core::LeafType::Operational) return gnmi::ON_CHANGE;
    return gnmi::SAMPLE;
}

void Backend::commit(const std::vector<SetOp>& ops) {
    std::lock_guard lk(bindingsMu_);  // ids_/writableConfig_ may race a runtime hot-plug
    // Structural pass first: deletes remove the leaf (data-plane absence); an
    // update to an absent-but-declared config path re-creates it (D3 re-assigns it
    // to its group). The writable schema persists across delete, so a re-Set is
    // allowed.
    for (const auto& op : ops) {
        const std::string c = canon(op.xpath);
        if (op.kind == SetOp::Kind::Delete) {
            registry_.unregisterLeaf(op.xpath);
            ids_.erase(c);
        } else if (ids_.find(c) == ids_.end()) {
            ids_[c] = registry_.registerLeaf(op.xpath, core::LeafType::Config, std::nullopt);
            writableConfig_.insert(c);
        }
    }

    // Value pass: one ValueWriter scope so a multi-leaf / atomic record lands
    // coherently (a reader sees all of it or none).
    core::ValueWriter w = registry_.writeValues();
    for (const auto& op : ops)
        if (op.kind == SetOp::Kind::Update)
            w.set(ids_.at(canon(op.xpath)), op.value, op.collectedNs);
}

}  // namespace gnmid
