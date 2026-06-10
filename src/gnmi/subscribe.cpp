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
#include <thread>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <grpc/grpc.h>

#include "subscribe.h"
#include "subscribe_emit.h"
#include "backend/leaf_store.hpp"
#include <utils/utils.h>
#include <utils/log.h>

using namespace std;
using namespace chrono;
using google::protobuf::RepeatedPtrField;

namespace impl {

namespace {

// Write each Notification as its own SubscribeResponse. Atomic containers split
// a query across several Notifications (spec §2.1.1), so emit sites loop here.
void writeAll(std::vector<Notification>& notes,
              ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  SubscribeResponse response;
  for (auto& n : notes) {
    *response.mutable_update() = std::move(n);
    stream->Write(response);
    response.Clear();
  }
}

} // namespace

/**
 * buildSubscribeNotifications - Build the Notification(s) for a SubscriptionList.
 * gnmi specification highly recommends bundling multiple <xpath, value> into one
 * Notification. Atomic containers are the exception: each is delivered as its own
 * atomic Notification (spec §2.1.1), so this may produce several.
 * @param out  receives one or more Notifications, in emit order.
 * @param request the SubscriptionList from SubscribeRequest to answer to.
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
      break;

    default:
      BOOST_LOG_TRIVIAL(warning) << "Unsupported Encoding "
                                 << Encoding_Name(request.encoding());
      return Status(StatusCode::UNIMPLEMENTED, Encoding_Name(request.encoding()));
  }

  // use_aliases was reserved/removed in proto 0.10.0; no action needed.
  /* updates_only is handled by the caller (handleStream); no action here */

  /* Merge the current snapshot of every subscribed path, then partition it into
   * atomic + non-atomic Notifications. Timestamps are collection-accurate, not
   * "now" (spec §3.5.2.3); Phase 4 adds JSON_IETF subtree wrapping here. */
  Snapshot merged;
  for (int i = 0; i < request.subscription_size(); i++) {
    Subscription sub = request.subscription(i);

    Status status = validateGnmiPath(sub.path());
    if (!status.ok()) return status;

    const string xpath = gnmi_to_xpath(sub.path());
    SnapResult res = registry_.snapshot(xpath);
    if (!res.routed)                       // §3.5.2.4 not implemented
      return Status(StatusCode::UNIMPLEMENTED, "path not implemented: " + xpath);
    if (res.snap.empty())                  // §3.5.1.3 exists (yet): silent, do NOT close
      BOOST_LOG_TRIVIAL(warning) << "path exists but has no data (yet): " << xpath;

    merged.merge(res.snap);
  }

  out = buildFullNotifications(merged, registry_);

  // A request prefix applies to the non-atomic notification only; atomic ones
  // carry their own container prefix (mixing the two is out of scope — see §2.1.1).
  if (request.has_prefix())
    for (auto& n : out)
      if (!n.atomic()) n.mutable_prefix()->CopyFrom(request.prefix());

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

  // Checks that sample_interval values are not higher than INT64_MAX
  // i.e. 9223372036854775807 nanoseconds
  for (int i = 0; i < request.subscribe().subscription_size(); i++) {
    Subscription sub = request.subscribe().subscription(i);
    if (sub.sample_interval() > duration<long long, std::nano>::max().count()) {
      context->TryCancel();
      return Status(StatusCode::INVALID_ARGUMENT,
                    string("sample_interval must be less than ")
                    + to_string(INT64_MAX) + " nanoseconds");
    }
  }

  // Send initial snapshot unless client requested updates_only.
  // sync_response is always sent to mark initial synchronisation complete.
  if (!request.subscribe().updates_only()) {
    std::vector<Notification> notes;
    status = buildSubscribeNotifications(notes, request.subscribe());
    if (!status.ok()) {
      context->TryCancel();
      return status;
    }
    writeAll(notes, stream);
  }

  response.set_sync_response(true);
  stream->Write(response);
  response.Clear();

  // Per-subscriber streaming state, split by mode. A single SubscriptionList may
  // legally mix SAMPLE and ON_CHANGE entries, so both buckets coexist and the
  // one loop below services each on its own terms.
  //   SAMPLE    — emit on a timer (sample_interval)
  //   ON_CHANGE — emit when a polled snapshot differs from the previous one
  // chronomap stays a vector (iterated, not keyed).
  vector<pair<Subscription, time_point<high_resolution_clock>>> chronomap;

  struct OnChangeSub {
    Subscription                      sub;
    string                            query;
    Snapshot                          prev;           // last state seen by this subscriber
    time_point<high_resolution_clock> lastHeartbeat;
  };
  vector<OnChangeSub> onChange;

  for (int i=0; i<request.subscribe().subscription_size(); i++) {
    Subscription sub = request.subscribe().subscription(i);
    const auto now = high_resolution_clock::now();

    // Resolve TARGET_DEFINED per leaf via the owning provider (default SAMPLE).
    switch (resolveStreamMode(sub, registry_)) {
      case SAMPLE:
        chronomap.emplace_back(sub, now);
        break;
      case ON_CHANGE: {
        // Baseline snapshot is taken regardless of updates_only — the latter only
        // suppresses the *initial emit* (handled above), never the baseline, so
        // the first diff reports nothing already sent (spec §3.5.1.5.2).
        const string query = gnmi_to_xpath(sub.path());
        onChange.push_back({sub, query, registry_.snapshot(query).snap, now});
        break;
      }
      default:
        BOOST_LOG_TRIVIAL(warning) << "Unsupported subscription mode";
        break;
    }
  }

  /* Single loop, capped at ~5 iterations/second, serving both buckets:
   * SAMPLE by timer, ON_CHANGE by snapshot diff. The store has no change-notify,
   * so ON_CHANGE detection polls each iteration (push/notify is a later optim).
   * Note: one Path per Subscription, but repeated Subscriptions in a list, each
   * with its own sample_interval / heartbeat_interval. */
  while(!context->IsCancelled()) {
    auto start = high_resolution_clock::now();

    // ---- SAMPLE: batch all due subscriptions into one Notification ----
    SubscribeRequest updateRequest(request);
    SubscriptionList* updateList(updateRequest.mutable_subscribe());
    updateList->clear_subscription();

    for (auto& pair : chronomap) {
      duration<long long, std::nano> duration =
        high_resolution_clock::now()-pair.second;
      if (duration > nanoseconds{pair.first.sample_interval()}) {
        pair.second = high_resolution_clock::now();
        Subscription* sub = updateList->add_subscription();
        sub->CopyFrom(pair.first);
      }
    }

    if (updateList->subscription_size() > 0) {
      std::vector<Notification> notes;
      status = buildSubscribeNotifications(notes, updateRequest.subscribe());
      if(!status.ok()) {
        context->TryCancel();
        return status;
      }
      writeAll(notes, stream);
    }

    // ---- ON_CHANGE: poll each subscriber's snapshot and diff vs its previous ----
    for (auto& oc : onChange) {
      Snapshot cur = registry_.snapshot(oc.query).snap;
      const auto now = high_resolution_clock::now();

      if (heartbeatDue(oc.sub.heartbeat_interval(), oc.lastHeartbeat, now)) {
        // Heartbeat re-emits the full state regardless of change (spec §3.5.1.5.2),
        // atomic containers re-sent atomically.
        std::vector<Notification> notes = buildFullNotifications(cur, registry_);
        writeAll(notes, stream);
        oc.lastHeartbeat = now;
      } else {
        // Non-atomic leaves diff; an atomic container that changed re-sends in full.
        std::vector<Notification> notes =
            buildChangeNotifications(oc.prev, cur, registry_);
        writeAll(notes, stream);
      }

      oc.prev = std::move(cur);
    }

    // Caps the loop at 5 iterations per second
    auto loopTime = high_resolution_clock::now() - start;
    this_thread::sleep_for(milliseconds(200) - loopTime);
  }

  return Status::OK;
}

/**
 * Handles SubscribeRequest messages with ONCE subscription mode by updating
 * all the Subscriptions once, sending a SYNC message, then closing the RPC.
 */
Status Subscribe::handleOnce(ServerContext* context, SubscribeRequest request,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  Status status;

  // Sends a Notification message that updates all Subcriptions once
  SubscribeResponse response;
  std::vector<Notification> notes;
  status = buildSubscribeNotifications(notes, request.subscribe());
  if (!status.ok()) {
    context->TryCancel();
    return status;
  }

  writeAll(notes, stream);

  // Sends a message that indicates that initial synchronization
  // has completed, i.e. each Subscription has been updated once
  response.set_sync_response(true);
  stream->Write(response);
  response.Clear();

  return Status::OK;
}

/**
 * Handles SubscribeRequest messages with POLL subscription mode by updating
 * all the Subscriptions each time a Poll request is received.
 */
Status Subscribe::handlePoll(ServerContext* context, SubscribeRequest request,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  SubscribeRequest subscription = request;
  Status status;

  // Send initial snapshot + sync_response on subscription establishment.
  // Aligns with Go reference impl (spec/gnmi/subscribe/subscribe.go:435):
  // client gets consistent state immediately without needing a first Poll trigger.
  {
    SubscribeResponse response;
    std::vector<Notification> notes;
    status = buildSubscribeNotifications(notes, subscription.subscribe());
    if (!status.ok()) {
      context->TryCancel();
      return status;
    }
    writeAll(notes, stream);
    response.set_sync_response(true);
    stream->Write(response);
    response.Clear();
  }

  while (stream->Read(&request)) {
    switch (request.request_case()) {
      case request.kPoll:
        {
          // Sends a Notification message that updates all Subcriptions once
          SubscribeResponse response;
          std::vector<Notification> notes;
          status = buildSubscribeNotifications(notes, subscription.subscribe());
          if (!status.ok()) {
            context->TryCancel();
            return status;
          }
          writeAll(notes, stream);

          response.set_sync_response(true);
          stream->Write(response);
          response.Clear();
          break;
        }
      // kAliases removed in proto 0.10.0 (field reserved)
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
    context->TryCancel();
    return Status(StatusCode::INVALID_ARGUMENT,
                  "SubscribeRequest needs non-empty SubscriptionList");
  }

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

}
