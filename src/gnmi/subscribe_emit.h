/*
 * subscribe_emit — pure helpers for turning the provider read model into gNMI
 * Notifications, factored out of subscribe.cpp's handleStream loop so the
 * ON_CHANGE semantics (diff → update/delete, heartbeat, collection timestamp,
 * TARGET_DEFINED resolution) are unit-testable without a gRPC stream or timing.
 */

#ifndef _GNMI_SUBSCRIBE_EMIT_H
#define _GNMI_SUBSCRIBE_EMIT_H

#include <chrono>
#include <cstdint>

#include <gnmi.grpc.pb.h>

#include "backend/data_provider.hpp"   // Snapshot, DataProviderRegistry
#include "backend/leaf_store.hpp"      // LeafStore::Diff

namespace impl {

// Append every snapshot leaf as an Update; return the newest collection time
// seen (0 if empty). That time becomes Notification.timestamp — it MUST be the
// collection time, not the emission time (spec §3.5.2.3).
int64_t emitSnapshot(const Snapshot& snap,
                     google::protobuf::RepeatedPtrField<gnmi::Update>* list);

// Emit an ON_CHANGE diff into a Notification: changed/added leaves as Updates,
// removed paths as Notification.delete (spec §3.5.2.3). Returns the newest
// collection time among the updated leaves (0 if none).
int64_t emitDiff(const LeafStore::Diff& diff, gnmi::Notification* notification);

// True when a heartbeat is due. intervalNs == 0 disables heartbeats.
bool heartbeatDue(uint64_t intervalNs,
                  std::chrono::high_resolution_clock::time_point last,
                  std::chrono::high_resolution_clock::time_point now);

// Resolve a subscription's effective stream mode, expanding TARGET_DEFINED via
// the owning provider (default SAMPLE). Returns SAMPLE or ON_CHANGE.
gnmi::SubscriptionMode resolveStreamMode(const gnmi::Subscription& sub,
                                         const DataProviderRegistry& registry);

} // namespace impl

#endif // _GNMI_SUBSCRIBE_EMIT_H
