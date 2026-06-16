#include "canonical_path.hpp"

#include <map>
#include <stdexcept>

namespace gnmid::core {

namespace {

// Re-encode a decoded predicate value into the canonical form: no surrounding
// quotes, backslash-escape the three characters that would otherwise confuse the
// escape-aware bracket scanners ('\', '[', ']'). '/' inside a predicate is left
// literal — depth tracking already protects it from being read as a separator.
std::string encodeValue(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '[' || c == ']') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// Parse one path element starting at `i` (advanced past the element), returning
// its canonical spelling: name followed by key-sorted [key=value] predicates with
// values decoded (quotes stripped, escapes resolved) and re-encoded canonically.
// Stops at the next depth-0 '/' or end of input.
std::string canonicalizeElement(std::string_view raw, std::size_t& i) {
    const std::size_t n = raw.size();

    std::string name;
    while (i < n && raw[i] != '[' && raw[i] != '/') name.push_back(raw[i++]);

    // std::map gives key ordering and repeated-key detection in one structure.
    std::map<std::string, std::string> predicates;
    while (i < n && raw[i] == '[') {
        ++i;  // past '['

        std::string key;
        while (i < n && raw[i] != '=' && raw[i] != ']') key.push_back(raw[i++]);
        if (i >= n || raw[i] != '=')
            throw std::invalid_argument("canonicalize: predicate missing '='");
        ++i;  // past '='

        // Value: optionally double-quoted, backslash-escaped; ends at the closing
        // ']' (unquoted) or the closing '"' then ']' (quoted).
        std::string value;
        const bool quoted = i < n && raw[i] == '"';
        if (quoted) ++i;
        while (i < n) {
            const char c = raw[i];
            if (c == '\\' && i + 1 < n) {
                value.push_back(raw[i + 1]);
                i += 2;
                continue;
            }
            if (quoted ? c == '"' : c == ']') break;
            value.push_back(c);
            ++i;
        }
        if (quoted) {
            if (i >= n || raw[i] != '"')
                throw std::invalid_argument("canonicalize: predicate missing closing quote");
            ++i;  // past closing '"'
        }
        if (i >= n || raw[i] != ']')
            throw std::invalid_argument("canonicalize: predicate missing ']'");
        ++i;  // past ']'

        if (!predicates.emplace(std::move(key), encodeValue(value)).second)
            throw std::invalid_argument("canonicalize: repeated predicate key in one element");
    }

    std::string out = std::move(name);
    for (const auto& [k, v] : predicates) {
        out.push_back('[');
        out.append(k);
        out.push_back('=');
        out.append(v);
        out.push_back(']');
    }
    return out;
}

}  // namespace

CanonicalPath canonicalize(std::string_view raw) {
    const std::size_t n = raw.size();

    std::vector<std::string> elements;
    std::size_t i = 0;
    while (i < n) {
        if (raw[i] == '/') {  // skip leading, trailing, and repeated separators
            ++i;
            continue;
        }
        elements.push_back(canonicalizeElement(raw, i));
    }

    std::string s = "/";  // root spelling; also the leading separator for elements
    for (std::size_t k = 0; k < elements.size(); ++k) {
        if (k > 0) s.push_back('/');
        s.append(elements[k]);
    }
    return CanonicalPath(std::move(s));
}

bool isUnderPrefix(const CanonicalPath& prefix, const CanonicalPath& path) noexcept {
    const std::string_view p = prefix.str();
    const std::string_view q = path.str();
    if (p.size() > q.size())          return false;
    if (q.substr(0, p.size()) != p)   return false;
    if (p.size() == q.size())         return true;   // equal node
    if (p == "/")                     return true;   // root is ancestor of all
    return q[p.size()] == '/';                        // element boundary
}

std::vector<std::string_view> ancestorPrefixes(const CanonicalPath& path) {
    const std::string_view p = path.str();

    // Collect depth-0 '/' positions (escape-aware: a '\' escapes the next char so
    // an escaped ']' inside a value never closes a predicate).
    std::vector<std::size_t> boundaries;
    int depth = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const char c = p[i];
        if (c == '\\') { ++i; continue; }
        if (c == '[')                       ++depth;
        else if (c == ']' && depth > 0)     --depth;
        else if (c == '/' && depth == 0)    boundaries.push_back(i);
    }

    // Longest-first: the deepest boundary yields the longest proper ancestor.
    // The leading '/' (position 0) would yield root "" — excluded by rule.
    std::vector<std::string_view> out;
    for (std::size_t k = boundaries.size(); k-- > 0;) {
        const std::size_t pos = boundaries[k];
        if (pos == 0) continue;
        out.push_back(p.substr(0, pos));
    }
    return out;
}

}  // namespace gnmid::core
