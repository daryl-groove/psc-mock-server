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

#ifndef _GNMI_SERVER_H
#define _GNMI_SERVER_H

#include <gnmi.grpc.pb.h>
#include "backend/data_provider.hpp"

using namespace grpc;
using namespace gnmi;
using google::protobuf::RepeatedPtrField;

class GNMIService final : public gNMI::Service
{
  public:
    // Providers are registered in main.cpp and injected here.
    // GNMIService is unaware of specific provider types.
    explicit GNMIService(DataProviderRegistry registry)
        : registry_(std::move(registry)) {}

    ~GNMIService() { std::cout << "Quitting GNMI Server" << std::endl; }

    Status Capabilities(ServerContext* context,
        const CapabilityRequest* request, CapabilityResponse* response);

    Status Get(ServerContext* context,
        const GetRequest* request, GetResponse* response);

    Status Set(ServerContext* context,
        const SetRequest* request, SetResponse* response);

    Status Subscribe(ServerContext* context,
        ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream);

  private:
    DataProviderRegistry registry_;
};

#endif //_GNMI_SERVER_H
