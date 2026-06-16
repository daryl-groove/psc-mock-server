#pragma once

#include <optional>
#include <string>
#include <unordered_set>

#include "canonical_path.hpp"
#include "leaf_entry.hpp"  // full definition needed for unordered_set<LeafEntry*>
#include "leaf_type.hpp"

namespace gnmid::core {

// A notification bundle — the set of leaves reported together in one gNMI
// Notification (D11). Encapsulated, registry-owned, non-copyable and non-movable;
// members_ are non-owning back-pointers into the registry's leaf map.
class NotificationGroup {
public:
    NotificationGroup(RegistryAccess, std::string name, CanonicalPath prefix, bool atomic,
                      std::optional<LeafType> preferredType)
        : name_(std::move(name)),
          prefix_(std::move(prefix)),
          atomic_(atomic),
          preferredType_(preferredType) {}

    const std::string&      name() const noexcept { return name_; }
    const CanonicalPath&    prefix() const noexcept { return prefix_; }
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

    std::string                    name_;
    CanonicalPath                  prefix_;  // guaranteed-normalized (D16)
    bool                           atomic_ = false;
    std::optional<LeafType>        preferredType_;
    std::unordered_set<LeafEntry*> members_;
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
