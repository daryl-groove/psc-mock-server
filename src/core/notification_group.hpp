#pragma once

#include <memory>
#include <optional>
#include <unordered_set>

#include "canonical_path.hpp"
#include "leaf_entry.hpp"  // full definition needed for unordered_set<LeafEntry*>
#include "leaf_type.hpp"

namespace gnmid::core {

// A notification bundle — the set of leaves reported together in one gNMI
// Notification (D11). Encapsulated, registry-owned, non-copyable and non-movable;
// members_ are non-owning back-pointers into the registry's leaf map. A group is
// identified solely by its prefix (D4): there is no separate name.
class NotificationGroup {
public:
    NotificationGroup(RegistryAccess, std::shared_ptr<const CanonicalPath> prefix, bool atomic,
                      std::optional<LeafType> preferredType)
        : prefix_(std::move(prefix)), atomic_(atomic), preferredType_(preferredType) {}

    // prefix_ is the SAME shared handle used as the registry's groups_ map key (L=B / D16),
    // mirroring LeafEntry::path_ — materialised once, shared zero-copy.
    const CanonicalPath&    prefix() const noexcept { return *prefix_; }
    bool                    atomic() const noexcept { return atomic_; }
    std::optional<LeafType> preferredType() const noexcept { return preferredType_; }
    const std::unordered_set<LeafEntry*>& members() const noexcept { return members_; }

    NotificationGroup(const NotificationGroup&)            = delete;
    NotificationGroup& operator=(const NotificationGroup&) = delete;
    NotificationGroup(NotificationGroup&&)                 = delete;  // members_ back-pointer stability
    NotificationGroup& operator=(NotificationGroup&&)      = delete;

private:
    friend class LeafRegistry;  // creates, links, and tears down

    // Link/unlink a leaf, maintaining the bidirectional invariant (leaf.group_ <->
    // members_). linkLeaf validates the path is under prefix_. Registry-only:
    // exposing manual linking would let a caller create a half-linked state.
    bool linkLeaf(LeafEntry* e);
    void unlinkLeaf(LeafEntry* e);

    std::shared_ptr<const CanonicalPath> prefix_;  // SAME handle as the groups_ map key (L=B / D16)
    bool                                 atomic_ = false;
    std::optional<LeafType>              preferredType_;
    std::unordered_set<LeafEntry*>       members_;
};

// Defined here because resolving group->preferredType() requires NotificationGroup
// to be complete (see leaf_entry.hpp). Priority: own type > group preferred >
// Operational (D8).
inline LeafType LeafEntry::effectiveType() const noexcept {
    if (type_)                              return *type_;
    if (group_ && group_->preferredType())  return *group_->preferredType();
    return LeafType::Operational;
}

}  // namespace gnmid::core
