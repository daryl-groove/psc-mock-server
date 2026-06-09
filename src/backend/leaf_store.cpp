#include "leaf_store.hpp"

namespace {

// gnmi_to_xpath emits quoted keys (e.g. [name="PSC-0"]); store keys and queries
// are normalised to the unquoted form so both spellings compare equal.
std::string stripQuotes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '"') out += c;
    return out;
}

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

// True if prefix matches full at a path-segment boundary, so /power does not
// match /power-supply.
bool isPrefixAtBoundary(const std::string& prefix, const std::string& full) {
    if (full.rfind(prefix, 0) != 0) return false;
    if (full.size() == prefix.size()) return true;
    const char next = full[prefix.size()];
    return next == '/' || next == '[';
}

// True if a stored leaf path is selected by query (already quote-stripped).
// A keyless query also matches keyed leaves of the same shape.
bool selects(const std::string& query, const std::string& leaf) {
    if (isPrefixAtBoundary(query, leaf)) return true;
    if (query.find('[') == std::string::npos)
        return isPrefixAtBoundary(query, stripKeys(leaf));
    return false;
}

bool valueEquals(const gnmi::TypedValue& a, const gnmi::TypedValue& b) {
    // Same message type, same process: serialisation is deterministic, which is
    // all diff() needs to detect a value change.
    return a.SerializeAsString() == b.SerializeAsString();
}

} // namespace

void LeafStore::set(const std::string& xpath, double value, int64_t collectedNs) {
    gnmi::TypedValue v;
    v.set_double_val(value);
    setValue(xpath, std::move(v), collectedNs);
}

void LeafStore::set(const std::string& xpath, const std::string& value,
                    int64_t collectedNs) {
    gnmi::TypedValue v;
    v.set_string_val(value);
    setValue(xpath, std::move(v), collectedNs);
}

void LeafStore::set(const std::string& xpath, bool value, int64_t collectedNs) {
    gnmi::TypedValue v;
    v.set_bool_val(value);
    setValue(xpath, std::move(v), collectedNs);
}

void LeafStore::set(const std::string& xpath, int64_t value, int64_t collectedNs) {
    gnmi::TypedValue v;
    v.set_int_val(value);
    setValue(xpath, std::move(v), collectedNs);
}

void LeafStore::set(const std::string& xpath, uint64_t value, int64_t collectedNs) {
    gnmi::TypedValue v;
    v.set_uint_val(value);
    setValue(xpath, std::move(v), collectedNs);
}

void LeafStore::setValue(const std::string& xpath, gnmi::TypedValue val,
                         int64_t collectedNs) {
    const std::string key = stripQuotes(xpath);
    std::unique_lock lock(mu_);
    leaves_[key] = Leaf{std::move(val), collectedNs};
}

void LeafStore::remove(const std::string& xpath) {
    const std::string key = stripQuotes(xpath);
    std::unique_lock lock(mu_);
    leaves_.erase(key);
}

std::optional<LeafStore::Leaf> LeafStore::get(const std::string& xpath) const {
    const std::string key = stripQuotes(xpath);
    std::shared_lock lock(mu_);
    auto it = leaves_.find(key);
    if (it == leaves_.end()) return std::nullopt;
    return it->second;
}

bool LeafStore::collect(const std::string& queryXpath,
                        RepeatedPtrField<Update>* list) const {
    const std::string query = stripQuotes(queryXpath);
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
    const std::string query = stripQuotes(queryXpath);
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
