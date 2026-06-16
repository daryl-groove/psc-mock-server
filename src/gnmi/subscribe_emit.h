/*
 * subscribe_emit — pure helpers for turning the Backend read model (a snapshot
 * View = leaf values + owning groups) into gNMI Notifications, factored out of
 * subscribe.cpp's loop so the ON_CHANGE semantics (diff → update/delete,
 * heartbeat, collection timestamp, TARGET_DEFINED resolution, atomic bundling)
 * are unit-testable without a gRPC stream or timing.
 */

#ifndef _GNMI_SUBSCRIBE_EMIT_H
#define _GNMI_SUBSCRIBE_EMIT_H

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gnmi.grpc.pb.h>

#include "backend/backend.hpp"

namespace impl {

// Change between two leaf snapshots, by changeSeq (D14): a leaf whose changeSeq
// differs (or is newly present) is `updated`; one present before and gone now is
// `removed` → a Notification.delete (spec §3.5.2.3).
struct LeafDiff {
    std::vector<std::pair<std::string, gnmid::core::LeafValueSnapshot>> updated;
    std::vector<std::string>                                           removed;
};
LeafDiff diffLeaves(const gnmid::core::LeafSnapshot& prev,
                    const gnmid::core::LeafSnapshot& cur);

// Partition a full View into Notifications: one atomic Notification per atomic
// group present (atomic=true, relativised, spec §2.1.1), plus one non-atomic
// Notification for the rest. Each carries its freshest-leaf timestamp. Used for
// the initial emit, ONCE, POLL, and the ON_CHANGE heartbeat (full re-send).
std::vector<gnmi::Notification> buildFullNotifications(const gnmid::Backend::View& view);

// ON_CHANGE per poll: only the Notifications representing a change. Non-atomic
// leaves emit a diff (updates + deletes); an atomic group that changed at all
// re-sends its COMPLETE current state (omitted leaves implicitly deleted), and a
// group that became empty emits a prefix delete.
std::vector<gnmi::Notification> buildChangeNotifications(const gnmid::Backend::View& prev,
                                                        const gnmid::Backend::View& cur);

// True when a heartbeat is due. intervalNs == 0 disables heartbeats.
bool heartbeatDue(uint64_t intervalNs,
                  std::chrono::high_resolution_clock::time_point last,
                  std::chrono::high_resolution_clock::time_point now);

// Resolve a subscription's effective stream mode, expanding TARGET_DEFINED via
// the Backend (schema-derived, P5). Returns SAMPLE or ON_CHANGE.
gnmi::SubscriptionMode resolveStreamMode(const gnmi::Subscription& sub,
                                         const gnmid::Backend& be);

}  // namespace impl

#endif  // _GNMI_SUBSCRIBE_EMIT_H
