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

#include <gnmi.grpc.pb.h>
#include "backend/data_provider.hpp"

using namespace gnmi;
using grpc::Status;
using grpc::StatusCode;
using google::protobuf::RepeatedPtrField;

namespace impl {

class Get {
  public:
    explicit Get(DataProviderRegistry& registry) : registry_(registry) {}
    ~Get() = default;

    Status run(const GetRequest* req, GetResponse* response);

  private:
    Status BuildGetNotification(Notification* notification, const Path* prefix,
                                const Path& path, gnmi::Encoding encoding);
    Status BuildGetUpdate(RepeatedPtrField<Update>* updateList,
                          const Path& path, std::string fullpath,
                          gnmi::Encoding encoding);

  private:
    DataProviderRegistry& registry_;
};

} // namespace impl

#endif //_GNMI_GET_H
