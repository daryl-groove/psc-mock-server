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

#include "set.h"
#include <utils/utils.h>
#include <utils/log.h>

using namespace std;

namespace impl {

// Mock server: Set is accepted but not applied to any real state.
// Phase 4+: wire to backend configuration store if needed.

StatusCode Set::handleUpdate(Update in, UpdateResult* out, string prefix)
{
  if (!in.has_path() || !in.has_val()) {
    BOOST_LOG_TRIVIAL(error) << "Set: update missing path or value";
    return StatusCode::INVALID_ARGUMENT;
  }

  string fullpath = prefix + gnmi_to_xpath(in.path());
  BOOST_LOG_TRIVIAL(debug) << "Set (mock no-op): " << fullpath;

  out->set_allocated_path(in.release_path());
  return StatusCode::OK;
}

Status Set::run(const SetRequest* request, SetResponse* response)
{
  if (request->extension_size() > 0) {
    BOOST_LOG_TRIVIAL(error) << "Set: extensions not implemented";
    return Status(StatusCode::UNIMPLEMENTED, "Extensions not implemented");
  }

  response->set_timestamp(get_time_nanosec());

  string prefix;
  if (request->has_prefix()) {
    prefix = gnmi_to_xpath(request->prefix());
    response->mutable_prefix()->CopyFrom(request->prefix());
  }

  for (auto delpath : request->delete_()) {
    string fullpath = prefix + gnmi_to_xpath(delpath);
    BOOST_LOG_TRIVIAL(debug) << "Set delete (mock no-op): " << fullpath;
    UpdateResult* res = response->add_response();
    *(res->mutable_path()) = delpath;
    res->set_op(gnmi::UpdateResult::DELETE);
  }

  for (auto& upd : request->replace()) {
    UpdateResult* res = response->add_response();
    auto rc = handleUpdate(upd, res, prefix);
    if (rc != StatusCode::OK)
      return Status(rc, "replace failed");
    res->set_op(gnmi::UpdateResult::REPLACE);
  }

  for (auto& upd : request->update()) {
    UpdateResult* res = response->add_response();
    auto rc = handleUpdate(upd, res, prefix);
    if (rc != StatusCode::OK)
      return Status(rc, "update failed");
    res->set_op(gnmi::UpdateResult::UPDATE);
  }

  return Status::OK;
}

} // namespace impl
