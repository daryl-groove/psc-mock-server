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

#include "gnmi.h"
#include <utils/log.h>

using namespace gnmi;
using namespace std;

// Static model list, keyed to the local yang/ reference files. Versions are
// YANG revision dates. openconfig-system is served too (the /system config +
// atomic NTP record, device-modeling-conventions §9) so it is advertised here
// (backlog C4); its date is the real yang/openconfig-system.yang revision.
// Phase 4: keep every version synced to its yang revision (the platform entry
// is an older pinned date pending that sync).
struct ModelInfo {
    const char* name;
    const char* organization;
    const char* version;
};

static const ModelInfo kSupportedModels[] = {
    { "openconfig-platform",     "OpenConfig working group", "2021-01-18" },
    { "openconfig-platform-psu", "OpenConfig working group", "2018-11-21" },
    { "openconfig-system",       "OpenConfig working group", "2026-03-31" },
};

Status GNMIService::Capabilities(ServerContext* context,
                                  const CapabilityRequest* request,
                                  CapabilityResponse* response)
{
  (void)context;

  if (request->extension_size() > 0) {
    BOOST_LOG_TRIVIAL(error) << "Capabilities: extensions not implemented";
    return Status(StatusCode::UNIMPLEMENTED, "Extensions not implemented");
  }

  for (const auto& m : kSupportedModels) {
    auto* model = response->add_supported_models();
    model->set_name(m.name);
    model->set_organization(m.organization);
    model->set_version(m.version);
  }

  // gNMI service version from proto file option
  const string gnmi_version =
      response->GetDescriptor()->file()->options()
               .GetExtension(gnmi::gnmi_service);
  response->set_gnmi_version(gnmi_version);

  response->add_supported_encodings(gnmi::Encoding::JSON_IETF);
  response->add_supported_encodings(gnmi::Encoding::JSON);

  return Status::OK;
}
