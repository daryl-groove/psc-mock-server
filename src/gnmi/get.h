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

#ifndef _GNMI_GET_H
#define _GNMI_GET_H

#include <vector>

#include <gnmi.grpc.pb.h>
#include "backend/backend.hpp"

using namespace gnmi;
using grpc::Status;
using grpc::StatusCode;
using google::protobuf::RepeatedPtrField;

namespace impl {

class Get {
  public:
    explicit Get(gnmid::Backend& be) : be_(be) {}
    ~Get() = default;

    Status run(const GetRequest* req, GetResponse* response);

  private:
    // Append the Notification(s) answering one requested path. Atomic containers
    // are returned as their own atomic Notification (spec §3.5.2.5), so a single
    // path may yield more than one — hence appending to the response list.
    Status buildGetNotifications(RepeatedPtrField<Notification>* out,
                                 const Path* prefix, const Path& path,
                                 gnmi::Encoding encoding,
                                 GetRequest::DataType type);

  private:
    gnmid::Backend& be_;
};

}  // namespace impl

#endif  // _GNMI_GET_H
