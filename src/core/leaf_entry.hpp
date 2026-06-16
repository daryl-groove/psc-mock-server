#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "canonical_path.hpp"
#include "gnmi.pb.h"
#include "leaf_type.hpp"

namespace gnmid::core {

class NotificationGroup;  // fwd — LeafEntry stores only a back-pointer (breaks the cycle)

// Construction passkey (D11). LeafEntry / NotificationGroup are non-copyable and
// non-movable, so the registry must build them in place inside its node-based
// std::map (try_emplace) — which constructs via the allocator, not the registry,
// so a literally-private constructor would be unreachable. Instead the ctors are
// public but take a RegistryAccess, whose own constructor is private to
// LeafRegistry. Net effect: only the registry can originate an entry/group, yet
// the allocator can still forward the (copyable) passkey. This realizes "registry
// is the sole owner & constructor" as a mechanism, not a comment.
class RegistryAccess {
    RegistryAccess() = default;
    friend class LeafRegistry;

public:
    RegistryAccess(const RegistryAccess&) = default;
};

// A single gNMI leaf node — encapsulated, registry-owned, non-copyable and
// non-movable (D11). Outside code reads via const accessors and never holds a
// mutable reference; the registry (friend) is the sole writer, and
// NotificationGroup (friend) maintains the back-pointer half of the link.
class LeafEntry {
public:
    LeafEntry(RegistryAccess, std::shared_ptr<const CanonicalPath> path,
              std::optional<LeafType> type, std::shared_ptr<const gnmi::TypedValue> value)
        : path_(std::move(path)), value_(std::move(value)), type_(type) {}

    const CanonicalPath& path() const noexcept { return *path_; }
    // A shared handle to an immutable value version (nullptr = unset); a writer
    // swaps in a NEW version, so this handle never mutates underfoot (D17).
    std::shared_ptr<const gnmi::TypedValue> value() const noexcept { return value_; }
    int64_t  collectedNs() const noexcept { return collectedNs_; }
    uint64_t changeSeq() const noexcept { return changeSeq_; }
    LeafType effectiveType() const noexcept;  // D8; defined where NotificationGroup is complete
    const NotificationGroup* group() const noexcept { return group_; }

    LeafEntry(const LeafEntry&)            = delete;
    LeafEntry& operator=(const LeafEntry&) = delete;
    LeafEntry(LeafEntry&&)                 = delete;  // back-pointer stability (D1/D11)
    LeafEntry& operator=(LeafEntry&&)      = delete;

private:
    friend class LeafRegistry;       // constructs, writes values, links/unlinks
    friend class NotificationGroup;  // maintains the leaf->group back-pointer

    // path_ is the SAME shared handle used as the registry map key, inside LeafId,
    // and in change events — materialised once, shared zero-copy, refcount-safe (L=B).
    std::shared_ptr<const CanonicalPath>    path_;
    std::shared_ptr<const gnmi::TypedValue> value_;        // COW versioned value, nullptr = unset (D17)
    std::optional<LeafType>                 type_;
    int64_t                                 collectedNs_ = 0;
    uint64_t                                changeSeq_   = 0;  // bumped on a real value change (D14)
    NotificationGroup*                      group_       = nullptr;
};

}  // namespace gnmid::core
