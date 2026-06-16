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
#include "backend/backend.hpp"

using namespace gnmi;
using grpc::Status;
using grpc::StatusCode;

namespace impl {

class Set {
  public:
    // The Backend is the write target: validated paths are applied to the shared
    // registry, which the Subscribe poll+diff loop turns into ON_CHANGE
    // notifications. Config paths are writable; read-only state is refused.
    explicit Set(gnmid::Backend& be) : be_(be) {}
    ~Set() = default;

    Status run(const SetRequest* request, SetResponse* response);

  private:
    gnmid::Backend& be_;
};

}  // namespace impl

#endif  // _GNMI_SET_H
