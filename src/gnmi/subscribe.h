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

#ifndef _GNMI_SUBSCRIBE_H
#define _GNMI_SUBSCRIBE_H

#include <vector>

#include <gnmi.grpc.pb.h>
#include "backend/backend.hpp"
#include "subscription.h"

using namespace gnmi;
using google::protobuf::RepeatedPtrField;
using grpc::ServerReaderWriter;
using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;

namespace impl {

class Subscribe {
  public:
    Subscribe(gnmid::Backend& be, SubscriptionHub& hub) : be_(be), hub_(hub) {}
    ~Subscribe() = default;

    Status run(ServerContext* context,
               ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream);

  private:
    // Build the Notification(s) for a SubscriptionList's initial/ONCE/POLL emit.
    // Returns more than one when the query spans atomic containers: each atomic
    // container is its own Notification (spec §2.1.1). Callers write each in turn.
    Status buildSubscribeNotifications(std::vector<Notification>& out,
                                       const SubscriptionList& request);
    Status handleStream(ServerContext* context, SubscribeRequest request,
              ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream);
    Status handleOnce(ServerContext* context, SubscribeRequest request,
              ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream);
    Status handlePoll(ServerContext* context, SubscribeRequest request,
              ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream);

  private:
    gnmid::Backend& be_;
    SubscriptionHub& hub_;   // push wake (P1/P2); streams register a StreamWaker here
};

}  // namespace impl

#endif  // _GNMI_SUBSCRIBE_H
