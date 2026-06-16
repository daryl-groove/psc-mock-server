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

#include <grpc/grpc.h>

#include "get.h"
#include "subscribe_emit.h"   // buildFullNotifications — atomic-aware emit, shared with Subscribe
#include <utils/utils.h>
#include <utils/log.h>

using namespace std;
using google::protobuf::RepeatedPtrField;
using gnmid::core::LeafType;

namespace impl {

// gNMI DataType filter (spec §3.3.4, GetRequest.type). The spec treats CONFIG/
// STATE/OPERATIONAL as a disjoint partition annotated per leaf in the schema, so
// each leaf carries exactly one LeafType — its effectiveType in the registry.
static bool matchesType(LeafType leaf, GetRequest::DataType requested)
{
  switch (requested) {
    case GetRequest::CONFIG:      return leaf == LeafType::Config;
    case GetRequest::STATE:       return leaf == LeafType::State;
    case GetRequest::OPERATIONAL: return leaf == LeafType::Operational;
    default:                      return true;   // ALL — handled by caller anyway
  }
}

static void filterByDataType(gnmid::Backend::View& view, GetRequest::DataType type)
{
  if (type == GetRequest::ALL) return;
  for (auto it = view.leaves.begin(); it != view.leaves.end(); ) {
    if (matchesType(it->second.effectiveType, type)) ++it;
    else                                             it = view.leaves.erase(it);
  }
}

Status
Get::buildGetNotifications(RepeatedPtrField<Notification>* out,
                           const Path* prefix, const Path& path,
                           gnmi::Encoding encoding,
                           GetRequest::DataType type)
{
  // Encoding is already validated in verifyGetRequest; only JSON/JSON_IETF reach
  // here. Phase 4 wraps values in JSON_IETF once the encoding layer is added.
  if (encoding != gnmi::JSON && encoding != gnmi::JSON_IETF)
    return Status(StatusCode::UNIMPLEMENTED, Encoding_Name(encoding));

  Status status = validateGnmiPath(path);
  if (!status.ok()) return status;

  // C1/D16: a non-openconfig origin names an unimplemented schema → UNIMPLEMENTED,
  // before routing. Checked after the structural validation so a malformed path
  // still wins INVALID_ARGUMENT. origin is then stripped for free (gnmi_to_xpath).
  if (prefix != nullptr) {
    status = validateOrigin(prefix->origin());
    if (!status.ok()) return status;
  }
  status = validateOrigin(path.origin());
  if (!status.ok()) return status;

  string fullpath;
  if (prefix != nullptr)
    fullpath += gnmi_to_xpath(*prefix);
  fullpath += gnmi_to_xpath(path);
  BOOST_LOG_TRIVIAL(debug) << "GetRequest Path " << fullpath;

  gnmid::Backend::View view = be_.snapshot(fullpath);
  if (!view.routed)                        // §3.3.4 not implemented
    return Status(StatusCode::UNIMPLEMENTED, "path not implemented: " + fullpath);

  // Drop leaves outside the requested config/state type before the empty check,
  // so a path that exists but holds nothing of the requested type is NOT_FOUND.
  filterByDataType(view, type);
  if (view.leaves.empty())                 // §3.3.4 exists (yet) but no data
    return Status(StatusCode::NOT_FOUND, "path has no data: " + fullpath);

  // Same atomic-aware partition as Subscribe: each atomic container becomes its
  // own atomic Notification (spec §3.5.2.5), the rest a single non-atomic one.
  std::vector<Notification> notes = buildFullNotifications(view);
  // C5/R (§2.2.2.1): echo prefix.target onto every notification (atomic
  // included) — target only, not the path-prefix (updates carry full paths) nor
  // origin (C1 strip-only).
  if (prefix != nullptr) echoTarget(notes, prefix->target());
  for (auto& n : notes)
    *out->Add() = std::move(n);
  return Status::OK;
}

static inline Status verifyGetRequest(const GetRequest* request)
{
  switch (request->encoding()) {
    case gnmi::JSON:
    case gnmi::JSON_IETF:
      break;
    default:
      BOOST_LOG_TRIVIAL(warning) << "Unsupported Encoding "
                                 << Encoding_Name(request->encoding());
      return Status(StatusCode::UNIMPLEMENTED, Encoding_Name(request->encoding()));
  }

  if (!GetRequest_DataType_IsValid(request->type())) {
    BOOST_LOG_TRIVIAL(warning) << "Invalid Data Type "
                               << GetRequest_DataType_Name(request->type());
    return Status(StatusCode::UNIMPLEMENTED,
                  GetRequest_DataType_Name(request->type()));
  }

  if (request->use_models_size() > 0) {
    BOOST_LOG_TRIVIAL(warning) << "use_models unsupported";
    return Status(StatusCode::UNIMPLEMENTED, "use_models unsupported");
  }

  if (request->extension_size() > 0) {
    BOOST_LOG_TRIVIAL(warning) << "extension unsupported";
    return Status(StatusCode::UNIMPLEMENTED, "extension unsupported");
  }

  return Status::OK;
}

Status Get::run(const GetRequest* req, GetResponse* response)
{
  Status status = verifyGetRequest(req);
  if (!status.ok()) return status;

  BOOST_LOG_TRIVIAL(debug) << "GetRequest DataType "
                           << GetRequest::DataType_Name(req->type())
                           << " Encoding " << Encoding_Name(req->encoding());

  auto* notificationList = response->mutable_notification();
  for (auto path : req->path()) {
    // One path may yield several Notifications when it spans atomic containers.
    status = req->has_prefix()
      ? buildGetNotifications(notificationList, &req->prefix(), path,
                              req->encoding(), req->type())
      : buildGetNotifications(notificationList, nullptr,       path,
                              req->encoding(), req->type());

    if (!status.ok()) {
      BOOST_LOG_TRIVIAL(error) << "buildGetNotifications failed: "
                               << status.error_message();
      return status;
    }
  }

  return Status::OK;
}

}  // namespace impl
