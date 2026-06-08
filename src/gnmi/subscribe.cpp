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

#include <thread>
#include <chrono>
#include <string>

#include <grpc/grpc.h>

#include "subscribe.h"
#include <utils/utils.h>
#include <utils/log.h>

using namespace std;
using namespace chrono;
using google::protobuf::RepeatedPtrField;

namespace impl {

Status
Subscribe::BuildSubsUpdate(RepeatedPtrField<Update>* updateList,
                            const Path& path, string fullpath,
                            gnmi::Encoding encoding)
{
  switch (encoding) {
    case gnmi::JSON:
    case gnmi::JSON_IETF:
      // Delegate entirely to backend — populates path + double_val per leaf.
      // Phase 4: add JSON_IETF wrapping once encoding layer is introduced.
      registry_.Fill(updateList, fullpath);
      break;

    default:
      return Status(StatusCode::UNIMPLEMENTED, Encoding_Name(encoding));
  }

  return Status::OK;
}

/**
 * BuildSubscribeNotification - Build a Notification message.
 * Contrary to Get Notification, gnmi specification highly recommands to
 * put multiple <xpath, value> in the same Notification message.
 * @param notification the notification that is constructed by this function.
 * @param request the SubscriptionList from SubscribeRequest to answer to.
 */
Status
Subscribe::BuildSubscribeNotification(Notification *notification,
                                      const SubscriptionList& request)
{
  RepeatedPtrField<Update>* updateList = notification->mutable_update();
  Status status;

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

  /* Get time since epoch in milliseconds */
  notification->set_timestamp(get_time_nanosec());

  if (request.has_prefix())
    notification->mutable_prefix()->CopyFrom(request.prefix());

  /* Fill Update RepeatedPtrField in Notification message
   * Update field contains only data elements that have changed values. */
  for (int i = 0; i < request.subscription_size(); i++) {
    Subscription sub = request.subscription(i);

    // Fetch all found counters value for a requested path
    status = BuildSubsUpdate(updateList, sub.path(), gnmi_to_xpath(sub.path()),
                             request.encoding());
    if (!status.ok()) {
      BOOST_LOG_TRIVIAL(error) << "Fail building update for "
                               << gnmi_to_xpath(sub.path());
      return status;
    }
  }

  notification->set_atomic(false);

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
    status = BuildSubscribeNotification(response.mutable_update(),
                                        request.subscribe());
    if (!status.ok()) {
      context->TryCancel();
      return status;
    }
    stream->Write(response);
    response.Clear();
  }

  response.set_sync_response(true);
  stream->Write(response);
  response.Clear();

  // We use a vector of pairs instead of a map as we are going to iterate more
  // than we are going to retrieve specific keys.
  vector<pair<Subscription, time_point<high_resolution_clock>>> chronomap;
  for (int i=0; i<request.subscribe().subscription_size(); i++) {
    Subscription sub = request.subscribe().subscription(i);
    switch (sub.mode()) {
      case TARGET_DEFINED:  // all PSC leaves are continuous sensors → SAMPLE
      case SAMPLE:
        chronomap.emplace_back(sub, high_resolution_clock::now());
        break;
      default:
        BOOST_LOG_TRIVIAL(warning) << "Unsupported mode";
        // TODO: Handle ON_CHANGE mode (Phase 3)
        // Ref: 3.5.1.5.2
        break;
    }
  }

  /* Periodically updates paths that require SAMPLE updates
   * Note : There is only one Path per Subscription, but repeated
   * Subscriptions in a SubscriptionList, each Subscription can
   * have its own sample interval */
  while(!context->IsCancelled()) {
    auto start = high_resolution_clock::now();

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
      status = BuildSubscribeNotification(response.mutable_update(),
                                          updateRequest.subscribe());
      if(!status.ok()) {
        context->TryCancel();
        return status;
      }
      stream->Write(response);
      response.Clear();
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
  status = BuildSubscribeNotification(response.mutable_update(),
                                      request.subscribe());
  if (!status.ok()) {
    context->TryCancel();
    return status;
  }

  stream->Write(response);
  response.Clear();

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
    status = BuildSubscribeNotification(response.mutable_update(),
                                        subscription.subscribe());
    if (!status.ok()) {
      context->TryCancel();
      return status;
    }
    stream->Write(response);
    response.Clear();
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
          status = BuildSubscribeNotification(response.mutable_update(),
                                              subscription.subscribe());
          if (!status.ok()) {
            context->TryCancel();
            return status;
          }
          stream->Write(response);
          response.Clear();

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
