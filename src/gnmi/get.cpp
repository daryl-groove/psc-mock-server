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
#include <utils/utils.h>
#include <utils/log.h>

using namespace std;
using google::protobuf::RepeatedPtrField;

namespace impl {

Status
Get::buildGetUpdate(RepeatedPtrField<Update>* updateList,
                    const Path& path, string fullpath,
                    gnmi::Encoding encoding)
{
  switch (encoding) {
    case gnmi::JSON:
    case gnmi::JSON_IETF:
      // Delegate entirely to backend — it populates path + double_val per leaf.
      // Phase 4: wrap values in JSON_IETF once encoding layer is added.
      if (!registry_.fill(updateList, fullpath))
        return Status(StatusCode::NOT_FOUND, "path not handled: " + fullpath);
      break;

    default:
      return Status(StatusCode::UNIMPLEMENTED, Encoding_Name(encoding));
  }

  return Status::OK;
}

Status
Get::buildGetNotification(Notification* notification, const Path* prefix,
                          const Path& path, gnmi::Encoding encoding)
{
  RepeatedPtrField<Update>* updateList = notification->mutable_update();
  string fullpath;

  notification->set_timestamp(get_time_nanosec());

  if (prefix != nullptr) {
    string str = gnmi_to_xpath(*prefix);
    BOOST_LOG_TRIVIAL(debug) << "prefix is " << str;
    notification->mutable_prefix()->CopyFrom(*prefix);
    fullpath += str;
  }

  fullpath += gnmi_to_xpath(path);
  BOOST_LOG_TRIVIAL(debug) << "GetRequest Path " << fullpath;

  Status status = validateGnmiPath(path);
  if (!status.ok()) return status;

  /* TODO: DataType CONFIG/STATE/OPERATIONAL filtering — get.cpp:120 */
  return buildGetUpdate(updateList, path, fullpath, encoding);
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
    auto* notification = notificationList->Add();

    status = req->has_prefix()
      ? buildGetNotification(notification, &req->prefix(), path, req->encoding())
      : buildGetNotification(notification, nullptr,       path, req->encoding());

    if (!status.ok()) {
      BOOST_LOG_TRIVIAL(error) << "buildGetNotification failed: "
                               << status.error_message();
      return status;
    }
  }

  return Status::OK;
}

} // namespace impl
