/*
 * Copyright 2020 Yohan Pipereau
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <set>
#include <thread>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <grpc/grpc.h>

#include "subscribe.h"
#include "subscribe_emit.h"
#include "canonical_path.hpp"
#include <utils/utils.h>
#include <utils/log.h>

using namespace std;
using namespace chrono;
using google::protobuf::RepeatedPtrField;

namespace impl {

namespace {

// R1/P5: a TARGET_DEFINED subscription lets the target choose the rate; the mock
// uses one server-default sample interval for all target-defined SAMPLE leaves,
// matched to the sensor simulator's ~1s drift tick. (An explicit SAMPLE with
// sample_interval 0 means "lowest supported" and keeps the loop's 200ms floor.)
constexpr uint64_t kTargetDefinedSampleIntervalNs = 1'000'000'000;  // 1s

// The target's lowest supported SAMPLE interval (§3.5.1.5.2). An explicit
// sample_interval below this is rejected (防呆 / anti-flood); sample_interval == 0
// ("lowest supported") is created at this rate. Matches the historical 200ms loop
// floor — and is load-bearing now that the S2 push loop has no implicit cap.
constexpr uint64_t kMinSampleIntervalNs = 200'000'000;  // 200ms

// Stamp the request target onto every notification (C5/R2: the single emit sink
// owns the echo, so no current or future emit path can forget it), then write each
// as its own SubscribeResponse. Atomic containers split a query across several
// Notifications (spec §2.1.1), so emit sites loop here.
void writeAll(std::vector<Notification>& notes, const std::string& target,
              ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  echoTarget(notes, target);
  SubscribeResponse response;
  for (auto& n : notes) {
    *response.mutable_update() = std::move(n);
    stream->Write(response);
    response.Clear();
  }
}

// Merge another snapshot View into `into`: union the leaves, append groups not
// already present (deduped by prefix), OR-reduce routed.
void mergeView(gnmid::Backend::View& into, gnmid::Backend::View&& add,
               std::set<std::string>& seenGroups)
{
  into.routed = into.routed || add.routed;
  for (auto& [path, leaf] : add.leaves) into.leaves.emplace(path, leaf);
  for (auto& g : add.groups)
    if (seenGroups.insert(g.prefix).second) into.groups.push_back(std::move(g));
}

}  // namespace

/**
 * buildSubscribeNotifications - Build the Notification(s) for a SubscriptionList.
 * Bundles multiple <xpath, value> into one Notification; atomic containers are
 * the exception, each delivered as its own atomic Notification (spec §2.1.1), so
 * this may produce several.
 */
Status
Subscribe::buildSubscribeNotifications(std::vector<Notification>& out,
                                       const SubscriptionList& request)
{
  switch (request.encoding()) {
    case gnmi::JSON:
    case gnmi::JSON_IETF:
      BOOST_LOG_TRIVIAL(debug) << "JSON IETF";
      break;

    case gnmi::PROTO:
      BOOST_LOG_TRIVIAL(error) << "PROTO encoding will soon be supported";
      return Status(StatusCode::UNIMPLEMENTED, Encoding_Name(request.encoding()));

    default:
      BOOST_LOG_TRIVIAL(warning) << "Unsupported Encoding "
                                 << Encoding_Name(request.encoding());
      return Status(StatusCode::UNIMPLEMENTED, Encoding_Name(request.encoding()));
  }

  /* Merge the current snapshot of every subscribed path, then partition it into
   * atomic + non-atomic Notifications. Timestamps are collection-accurate, not
   * "now" (spec §3.5.2.3). */
  gnmid::Backend::View merged;
  std::set<std::string> seenGroups;
  for (int i = 0; i < request.subscription_size(); i++) {
    const Subscription& sub = request.subscription(i);

    Status status = validateGnmiPath(sub.path());
    if (!status.ok()) return status;

    const string xpath = gnmi_to_xpath(sub.path());
    gnmid::Backend::View v = be_.snapshot(xpath);
    if (!v.routed)                         // §3.5.2.4 not implemented
      return Status(StatusCode::UNIMPLEMENTED, "path not implemented: " + xpath);
    if (v.leaves.empty())                  // §3.5.1.3 exists (yet): silent, do NOT close
      BOOST_LOG_TRIVIAL(warning) << "path exists but has no data (yet): " << xpath;

    mergeView(merged, std::move(v), seenGroups);
  }

  out = buildFullNotifications(merged);
  // C5/R2 (§2.2.2.1): prefix.target is echoed at the writeAll sink (a single emit
  // chokepoint), not here, so every emit path gets it uniformly.
  return Status::OK;
}

/**
 * Handles SubscribeRequest messages with STREAM subscription mode by
 * periodically sending updates to the client.
 */
Status Subscribe::handleStream(
    ServerContext* context, SubscribeRequest request,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  SubscribeResponse response;
  Status status;
  // Request prefix.target — echoed onto every notification at the writeAll sink.
  const std::string& target = request.subscribe().prefix().target();

  // Per-subscription sample_interval setup validation (§3.5.1.5.2).
  for (int i = 0; i < request.subscribe().subscription_size(); i++) {
    const Subscription& sub = request.subscribe().subscription(i);
    if (sub.sample_interval() > duration<long long, std::nano>::max().count()) {
      return Status(StatusCode::INVALID_ARGUMENT,
                    string("sample_interval must be less than ")
                    + to_string(INT64_MAX) + " nanoseconds");
    }
    // C3 / S-P5-c: the target owns the interval for TARGET_DEFINED, so a
    // client-pinned sample_interval is rejected (§3.5.1.5.2 L1790-1792).
    // sample_interval == 0 means "no pin" and stays allowed.
    // Return-only (no TryCancel): the spec MUST is to close with InvalidArgument,
    // and TryCancel() forces the client to observe CANCELLED instead (verified).
    if (sub.mode() == gnmi::TARGET_DEFINED && sub.sample_interval() != 0)
      return Status(StatusCode::INVALID_ARGUMENT,
                    "sample_interval must not be set with TARGET_DEFINED");

    // A non-zero explicit SAMPLE interval below the lowest the target supports is
    // unsupportable → InvalidArgument (§3.5.1.5.2). 0 ("lowest supported") is allowed
    // and floored to kMinSampleIntervalNs in the loop. Without this a client pinning a
    // near-zero interval would flood the push loop (no implicit cap since S2).
    if (sub.mode() == gnmi::SAMPLE && sub.sample_interval() != 0 &&
        sub.sample_interval() < kMinSampleIntervalNs)
      return Status(StatusCode::INVALID_ARGUMENT,
                    "sample_interval below the minimum supported "
                    + to_string(kMinSampleIntervalNs) + " ns");
  }

  // Send initial snapshot unless client requested updates_only. sync_response is
  // always sent to mark initial synchronisation complete.
  if (!request.subscribe().updates_only()) {
    std::vector<Notification> notes;
    status = buildSubscribeNotifications(notes, request.subscribe());
    if (!status.ok()) return status;
    writeAll(notes, target, stream);
  }

  response.set_sync_response(true);
  stream->Write(response);
  response.Clear();

  // Per-subscriber streaming state, split by mode. A single SubscriptionList may
  // legally mix SAMPLE and ON_CHANGE entries, so both buckets coexist and the one
  // loop below services each on its own terms.
  vector<pair<Subscription, time_point<high_resolution_clock>>> chronomap;

  struct OnChangeSub {
    Subscription                      sub;
    string                            query;
    gnmid::Backend::View              prev;          // last state seen by this subscriber
    time_point<high_resolution_clock> lastHeartbeat;
  };
  vector<OnChangeSub> onChange;

  for (int i = 0; i < request.subscribe().subscription_size(); i++) {
    const Subscription& sub = request.subscribe().subscription(i);
    const auto now = high_resolution_clock::now();

    // Resolve TARGET_DEFINED per leaf via the Backend (schema-derived, P5).
    switch (resolveStreamMode(sub, be_)) {
      case SAMPLE: {
        // R1/P5: a TARGET_DEFINED leaf carries no client interval (C3 rejects a
        // pinned one), so without a default it free-runs at the loop's 200ms
        // floor. Apply the server default; an explicit SAMPLE keeps its own.
        Subscription s = sub;
        if (sub.mode() == gnmi::TARGET_DEFINED && s.sample_interval() == 0)
          s.set_sample_interval(kTargetDefinedSampleIntervalNs);
        chronomap.emplace_back(std::move(s), now);
        break;
      }
      case ON_CHANGE: {
        // Baseline snapshot regardless of updates_only — the latter only
        // suppresses the *initial emit* (handled above), never the baseline.
        const string query = gnmi_to_xpath(sub.path());
        onChange.push_back({ sub, query, be_.snapshot(query), now });
        break;
      }
      default:
        BOOST_LOG_TRIVIAL(warning) << "Unsupported subscription mode";
        break;
    }
  }

  // Register with the push hub so a core commit on another thread (a Set, or a sensor
  // driver) wakes us the instant a change touches one of our ON_CHANGE query subtrees
  // — real-time delivery instead of the old poll tick (P1/P2, S2). SAMPLE is
  // timer-driven and needs no push, so only ON_CHANGE queries are registered; a
  // pure-SAMPLE stream registers none and is simply never push-woken. queries are
  // finalised here, before the waker is constructed (and so published to the hub).
  std::vector<gnmid::core::CanonicalPath> ocQueries;
  ocQueries.reserve(onChange.size());
  for (const auto& oc : onChange) ocQueries.push_back(gnmid::core::canonicalize(oc.query));
  StreamWaker waker(hub_, std::move(ocQueries));

  // Effective SAMPLE interval: an explicit 0 means "lowest supported", floored to
  // 200ms (the historical loop floor); a TARGET_DEFINED->SAMPLE leaf already had a
  // server default stamped at setup. Used for BOTH the wait deadline and the due-test,
  // so a 0-interval sub neither spins nor over-emits when push-woken.
  auto effInterval = [](const Subscription& s) -> nanoseconds {
    nanoseconds iv{s.sample_interval()};
    return iv.count() == 0 ? nanoseconds{kMinSampleIntervalNs} : iv;  // 0 -> lowest supported
  };

  /* One loop serving both buckets: SAMPLE by timer, ON_CHANGE by push (woken by the
   * hub) with snapshot+diff as the source of truth. The wait blocks until the next
   * SAMPLE/heartbeat deadline, a push wake, or a ~1s liveness cap (so an idle stream
   * still re-checks IsCancelled() — sync gRPC has no cancel->cv event; backlog "Push
   * layer"). S3 replaces the snapshot+diff with the batch payload + a watermark. */
  while (!context->IsCancelled()) {
    auto now = high_resolution_clock::now();
    auto deadline = now + seconds(1);  // liveness cap
    for (auto& [s, last] : chronomap)
      deadline = std::min(deadline, last + effInterval(s));
    for (auto& oc : onChange)
      if (oc.sub.heartbeat_interval() > 0)
        deadline = std::min(deadline,
                            oc.lastHeartbeat + nanoseconds{oc.sub.heartbeat_interval()});

    waker.waitUntil(deadline);
    if (context->IsCancelled()) break;
    now = high_resolution_clock::now();

    // ---- SAMPLE: batch all due subscriptions into one Notification ----
    SubscribeRequest updateRequest(request);
    SubscriptionList* updateList(updateRequest.mutable_subscribe());
    updateList->clear_subscription();
    for (auto& [s, last] : chronomap) {
      if (now - last >= effInterval(s)) {
        last = now;
        updateList->add_subscription()->CopyFrom(s);
      }
    }
    if (updateList->subscription_size() > 0) {
      std::vector<Notification> notes;
      status = buildSubscribeNotifications(notes, updateRequest.subscribe());
      if (!status.ok()) return status;
      writeAll(notes, target, stream);
    }

    // ---- ON_CHANGE: snapshot each subscriber and diff vs its previous. S2 keeps the
    // existing builder; the only change vs the old loop is that the push wake (not a
    // 200ms timer) is what drove us here. ----
    for (auto& oc : onChange) {
      gnmid::Backend::View cur = be_.snapshot(oc.query);
      std::vector<Notification> notes;
      if (heartbeatDue(oc.sub.heartbeat_interval(), oc.lastHeartbeat, now)) {
        // Heartbeat re-emits the full state regardless of change (spec §3.5.1.5.2).
        notes = buildFullNotifications(cur);
        oc.lastHeartbeat = now;
      } else {
        notes = buildChangeNotifications(oc.prev, cur);
      }
      writeAll(notes, target, stream);
      oc.prev = std::move(cur);
    }
  }

  return Status::OK;
}

/**
 * Handles SubscribeRequest messages with ONCE subscription mode by updating all
 * the Subscriptions once, sending a SYNC message, then closing the RPC.
 */
Status Subscribe::handleOnce(ServerContext* context, SubscribeRequest request,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  Status status;

  SubscribeResponse response;
  std::vector<Notification> notes;
  status = buildSubscribeNotifications(notes, request.subscribe());
  if (!status.ok()) return status;

  writeAll(notes, request.subscribe().prefix().target(), stream);

  // Indicate that initial synchronization has completed.
  response.set_sync_response(true);
  stream->Write(response);
  response.Clear();

  return Status::OK;
}

/**
 * Handles SubscribeRequest messages with POLL subscription mode by updating all
 * the Subscriptions each time a Poll request is received.
 */
Status Subscribe::handlePoll(ServerContext* context, SubscribeRequest request,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  SubscribeRequest subscription = request;
  Status status;

  // Send initial snapshot + sync_response on subscription establishment.
  {
    SubscribeResponse response;
    std::vector<Notification> notes;
    status = buildSubscribeNotifications(notes, subscription.subscribe());
    if (!status.ok()) return status;
    writeAll(notes, subscription.subscribe().prefix().target(), stream);
    response.set_sync_response(true);
    stream->Write(response);
    response.Clear();
  }

  while (stream->Read(&request)) {
    switch (request.request_case()) {
      case request.kPoll:
        {
          SubscribeResponse response;
          std::vector<Notification> notes;
          status = buildSubscribeNotifications(notes, subscription.subscribe());
          if (!status.ok()) return status;
          writeAll(notes, subscription.subscribe().prefix().target(), stream);

          response.set_sync_response(true);
          stream->Write(response);
          response.Clear();
          break;
        }
      case request.kSubscribe:
        return Status(StatusCode::INVALID_ARGUMENT,
                      "A SubscriptionList has already been received for this RPC");
      default:
        return Status(StatusCode::INVALID_ARGUMENT,
                      "Unknown content for SubscribeRequest message");
    }
  }

  return Status::OK;
}

/**
 * Handles the first SubscribeRequest message.
 * If it does not have the "subscribe" field set, the RPC MUST be cancelled.
 * Ref: 3.5.1.1
 */
Status Subscribe::run(ServerContext* context,
                 ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  SubscribeRequest request;

  stream->Read(&request);

  if (request.extension_size() > 0) {
    BOOST_LOG_TRIVIAL(error) << "Extensions not implemented";
    return Status(StatusCode::UNIMPLEMENTED, "Extensions not implemented");
  }

  if (!request.has_subscribe()) {
    return Status(StatusCode::INVALID_ARGUMENT,
                  "SubscribeRequest needs non-empty SubscriptionList");
  }

  // Validate origin once at setup (D16/C1, Resolver pipeline step 1): a
  // non-openconfig origin names an unimplemented schema → UNIMPLEMENTED. Done
  // here, not in buildSubscribeNotifications, which is skipped under
  // updates_only and otherwise re-run per tick.
  const SubscriptionList& sl = request.subscribe();
  if (Status s = validateOrigin(sl.prefix().origin()); !s.ok()) return s;
  for (const auto& sub : sl.subscription())
    if (Status s = validateOrigin(sub.path().origin()); !s.ok()) return s;

  switch (request.subscribe().mode()) {
    case SubscriptionList_Mode_STREAM:
      return handleStream(context, request, stream);
    case SubscriptionList_Mode_ONCE:
      return handleOnce(context, request, stream);
    case SubscriptionList_Mode_POLL:
      return handlePoll(context, request, stream);
    default:
      BOOST_LOG_TRIVIAL(error) << "Unknown subscription mode";
      return Status(StatusCode::UNKNOWN, "Unknown subscription mode");
  }
}

}  // namespace impl
