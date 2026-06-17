#include "notification_group.hpp"

#include "canonical_path.hpp"

namespace gnmid::core {

bool NotificationGroup::linkLeaf(LeafEntry* e) {
    // Element-aligned under-prefix check (D16); a bare list prefix does not capture
    // a keyed entry. The path is already canonical, so no re-normalization here.
    if (e == nullptr || !isUnderPrefix(*prefix_, e->path())) {
        return false;
    }
    members_.insert(e);
    e->group_ = this;  // back-pointer half of the bidirectional invariant
    return true;
}

void NotificationGroup::unlinkLeaf(LeafEntry* e) {
    if (e == nullptr) {
        return;
    }
    members_.erase(e);
    if (e->group_ == this) {
        e->group_ = nullptr;
    }
}

}  // namespace gnmid::core
