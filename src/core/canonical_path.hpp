#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace gnmid::core {

// A gNMI path in its canonical form — the identity key for a leaf node (D16).
//
// One gNMI node has many string spellings (reordered list keys, quoted vs
// unquoted predicate values, a trailing slash). CanonicalPath collapses all of
// them to a single string so that string equality coincides with gNMI
// structured-Path node equality, for the subset the mock supports.
//
// The ONLY way to obtain one is canonicalize(): there is no public constructor
// from a bare string, so an un-normalized key is impossible to construct (A3).
// The core is single-origin (O2): canonicalize() and CanonicalPath see only
// origin-less paths; origin stays a protocol/provider-boundary concern.
class CanonicalPath {
public:
    const std::string& str() const noexcept { return s_; }

    bool operator==(const CanonicalPath& o) const noexcept { return s_ == o.s_; }
    bool operator<(const CanonicalPath& o) const noexcept { return s_ < o.s_; }

private:
    explicit CanonicalPath(std::string normalized) noexcept : s_(std::move(normalized)) {}
    friend CanonicalPath canonicalize(std::string_view raw);

    std::string s_;
};

// The sole factory. A free function (not a registry method), so path identity is
// independently testable and reusable. Applies, in order: strip a trailing slash
// (except root "/"), strip predicate quotes (escape-aware), sort each element's
// [key=value] predicates by key. Throws std::invalid_argument on a malformed
// predicate (missing '=' / ']', or a repeated key within one element).
CanonicalPath canonicalize(std::string_view raw);

// Element-aligned ancestor-or-equal on already-canonical paths: true if `prefix`
// equals `path` or is a proper path-ancestor of it (the char after the match is a
// '/' element separator). So /a/b covers /a/b and /a/b/c, but not /a/bc, and a
// bare list /a/b does not cover a keyed entry /a/b[name=x] (D16 element-aligned).
bool isUnderPrefix(const CanonicalPath& prefix, const CanonicalPath& path) noexcept;

// Proper ancestor prefixes of `path` (its truncation at every element boundary),
// LONGEST FIRST, as views into path.str() — used by D3 longest-ancestor
// auto-assign. Root "/" is not yielded. Views are valid for path's lifetime.
std::vector<std::string_view> ancestorPrefixes(const CanonicalPath& path);

}  // namespace gnmid::core

template <>
struct std::hash<gnmid::core::CanonicalPath> {
    std::size_t operator()(const gnmid::core::CanonicalPath& p) const noexcept {
        return std::hash<std::string>{}(p.str());
    }
};
