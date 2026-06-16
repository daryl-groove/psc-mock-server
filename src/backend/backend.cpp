#include "backend/backend.hpp"

#include <utility>

#include "canonical_path.hpp"

namespace gnmid {

namespace {

// Namespace ownership: does `prefix` own `path`'s namespace? Looser than the
// core's element-aligned isUnderPrefix on purpose — a key-LESS namespace root
// (e.g. "/components/component") must own its keyed entries
// ("/components/component[name=PSC-0]/..."), so a '[' right after the match counts
// as a boundary too (matching the old isPathPrefix routing semantics). Both
// arguments are canonical strings.
bool ownsPath(const std::string& prefix, const std::string& path) {
    if (!path.starts_with(prefix)) return false;
    if (path.size() == prefix.size()) return true;
    const char next = path[prefix.size()];
    return next == '/' || next == '[';
}

// Drop every [key=value] predicate from a path, leaving the bare element names.
std::string stripKeys(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    bool inKey = false;
    for (char c : path) {
        if (c == '[')      inKey = true;
        else if (c == ']') inKey = false;
        else if (!inKey)   out.push_back(c);
    }
    return out;
}

// Does a query select a leaf? Exact element-aligned prefix; AND a query with NO
// key predicate also selects keyed leaves of the same shape (a key-omitted list
// query fans out to all entries — the old LeafStore::selects semantics, the list
// expansion the conventions place in the boundary layer, not the core).
bool selects(const std::string& query, const std::string& leaf) {
    if (ownsPath(query, leaf)) return true;
    if (query.find('[') == std::string::npos)
        return ownsPath(query, stripKeys(leaf));
    return false;
}

}  // namespace

std::string Backend::canon(const std::string& xpath) {
    return core::canonicalize(xpath).str();
}

void Backend::addProvider(std::unique_ptr<Provider> p) {
    ownedPrefixes_.push_back(core::canonicalize(p->domainPrefix()));
    p->start();                       // provider already registered its leaves in its ctor
    providers_.push_back(std::move(p));
}

void Backend::declareGroup(const std::string& name, const std::string& prefix, bool atomic) {
    registry_.registerGroup(name, prefix, atomic);
}

core::LeafId Backend::declareLeaf(const std::string& xpath, core::LeafType type,
                                  std::optional<gnmi::TypedValue> value) {
    core::LeafId id = registry_.registerLeaf(xpath, type, std::move(value));
    const std::string c = canon(xpath);
    ids_[c] = id;
    if (type == core::LeafType::Config) writableConfig_.insert(c);
    return id;
}

bool Backend::routed(const std::string& xpath) const {
    const std::string path = canon(xpath);
    for (const auto& prefix : ownedPrefixes_)
        if (ownsPath(prefix.str(), path)) return true;
    return false;
}

bool Backend::writable(const std::string& xpath) const {
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
