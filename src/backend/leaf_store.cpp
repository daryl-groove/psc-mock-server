#include "leaf_store.hpp"

namespace {

// Drop every [key=value] predicate, leaving the bare element path. Lets a
// keyless query (/components/component/.../input-voltage) select keyed leaves of
// every instance.
std::string stripKeys(const std::string& s) {
    std::string out;
    bool inKey = false;
    for (char c : s) {
        if (c == '[') { inKey = true;  continue; }
        if (c == ']') { inKey = false; continue; }
        if (!inKey) out += c;
    }
    return out;
}

// True if a stored leaf path is selected by query (already quote-stripped).
// A keyless query also matches keyed leaves of the same shape.
bool selects(const std::string& query, const std::string& leaf) {
    if (isPathPrefix(query, leaf)) return true;
    if (query.find('[') == std::string::npos)
        return isPathPrefix(query, stripKeys(leaf));
    return false;
}

bool valueEquals(const gnmi::TypedValue& a, const gnmi::TypedValue& b) {
    // Same message type, same process: serialisation is deterministic, which is
    // all diff() needs to detect a value change.
    return a.SerializeAsString() == b.SerializeAsString();
}

} // namespace

void LeafStore::commit(const WriteBatch& batch) {
    // One lock for the whole batch: this is the write-side atomicity guarantee.
    // Ops apply in batch order, so a delete-then-set of the same leaf within one
    // transaction ends as the set (spec Set order delete -> replace -> update).
    std::unique_lock lock(mu_);
    for (const auto& op : batch.ops()) {
        const std::string key = stripPathQuotes(op.xpath);
        switch (op.kind) {
            case WriteOp::Kind::Set:
                leaves_[key] = Leaf{op.val, op.collectedNs};
                break;
            case WriteOp::Kind::Remove:
                leaves_.erase(key);
                break;
        }
    }
}

std::optional<LeafStore::Leaf> LeafStore::get(const std::string& xpath) const {
    const std::string key = stripPathQuotes(xpath);
    std::shared_lock lock(mu_);
    auto it = leaves_.find(key);
    if (it == leaves_.end()) return std::nullopt;
    return it->second;
}

bool LeafStore::collect(const std::string& queryXpath,
                        RepeatedPtrField<Update>* list) const {
    const std::string query = stripPathQuotes(queryXpath);
    std::shared_lock lock(mu_);
    const int before = list->size();
    for (const auto& [path, leaf] : leaves_) {
        if (!selects(query, path)) continue;
        auto* u = list->Add();
        xpath_to_gnmi_path(path, u->mutable_path());
        *u->mutable_val() = leaf.val;
    }
    return list->size() > before;
}

LeafStore::Snapshot LeafStore::snapshot(const std::string& queryXpath) const {
    const std::string query = stripPathQuotes(queryXpath);
    std::shared_lock lock(mu_);
    Snapshot out;
    for (const auto& [path, leaf] : leaves_)
        if (selects(query, path))
            out.emplace(path, leaf);
    return out;
}

LeafStore::Diff LeafStore::diff(const Snapshot& prev, const Snapshot& cur) {
    Diff d;
    for (const auto& [path, leaf] : cur) {
        auto it = prev.find(path);
        if (it == prev.end() || !valueEquals(it->second.val, leaf.val))
            d.updated.emplace_back(path, leaf);
    }
    for (const auto& [path, leaf] : prev)
        if (cur.find(path) == cur.end())
            d.removed.push_back(path);
    return d;
}
