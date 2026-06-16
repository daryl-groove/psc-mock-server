#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "canonical_path.hpp"

namespace gnmid::core {

// The public handle a caller (provider) holds instead of a raw LeafEntry* (D1).
// Internally it carries the leaf's shared_ptr<const CanonicalPath> — the SAME
// handle the registry keys its map on (L=B) — so resolving it is a direct map
// lookup with no string copy. Under L=B single-materialisation one live leaf maps
// to one shared handle, so the defaulted equality (shared_ptr identity) coincides
// with path identity; a stale id (leaf detached, then a different node
// materialised) compares unequal, which is exactly what callers want.
class LeafId {
public:
    LeafId() = default;  // empty / "no leaf"

    bool valid() const noexcept { return path_ != nullptr; }

    bool operator==(const LeafId&) const = default;

private:
    friend class LeafRegistry;  // only the registry mints a LeafId
    friend struct std::hash<LeafId>;

    explicit LeafId(std::shared_ptr<const CanonicalPath> p) : path_(std::move(p)) {}

    const std::shared_ptr<const CanonicalPath>& path() const noexcept { return path_; }

    std::shared_ptr<const CanonicalPath> path_;  // SAME handle the registry keys on (L=B)
};

}  // namespace gnmid::core

template <>
struct std::hash<gnmid::core::LeafId> {
    std::size_t operator()(const gnmid::core::LeafId& id) const noexcept {
        return std::hash<const void*>{}(id.path_.get());  // consistent with pointer-identity ==
    }
};
