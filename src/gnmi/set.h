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

#ifndef _GNMI_SET_H
#define _GNMI_SET_H

#include <gnmi.grpc.pb.h>
#include "backend/data_provider.hpp"

using namespace gnmi;
using grpc::Status;
using grpc::StatusCode;

namespace impl {

class Set {
  public:
    // The registry is the write target: validated paths are applied to the
    // owning provider's store, which the Subscribe poll+diff loop turns into
    // ON_CHANGE notifications. Writable providers (config true) accept writes;
    // read-only ones refuse them.
    explicit Set(DataProviderRegistry& registry) : registry_(registry) {}
    ~Set() = default;

    Status run(const SetRequest* request, SetResponse* response);

  private:
    DataProviderRegistry& registry_;
};

} // namespace impl

#endif //_GNMI_SET_H
