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

namespace {

// Map a write target to a transaction-abort status, or OK if it may be written.
// Per §3.4.7: an update/replace to a path not in the supported schema is
// NOT_FOUND; a path that exists but is read-only (state, not config true) cannot
// be modified, which we report as INVALID_ARGUMENT.
Status checkWritable(const DataProviderRegistry& registry, const string& xpath)
{
  WriteResult wr = registry.writable(xpath);
  if (!wr.routed)
    return Status(StatusCode::NOT_FOUND, "path not found: " + xpath);
  if (!wr.applied)
    return Status(StatusCode::INVALID_ARGUMENT, "path is read-only: " + xpath);
  return Status::OK;
}

// Structural + writability validation for one update/replace operation.
Status validateWrite(const DataProviderRegistry& registry, const Update& upd,
                     const string& prefix)
{
  if (!upd.has_path() || !upd.has_val())
    return Status(StatusCode::INVALID_ARGUMENT, "update missing path or value");
  return checkWritable(registry, prefix + gnmi_to_xpath(upd.path()));
}

} // namespace

// Set is a transaction (§3.4.3): validate every operation first and apply none
// if any fails, so the store is never left half-mutated. With every path proven
// writable up front, the apply phase cannot fail — no rollback is needed.
Status Set::run(const SetRequest* request, SetResponse* response)
{
  if (request->extension_size() > 0) {
    BOOST_LOG_TRIVIAL(error) << "Set: extensions not implemented";
    return Status(StatusCode::UNIMPLEMENTED, "Extensions not implemented");
  }

  // One transaction timestamp: response stamp and every applied leaf's
  // collection time share it, so a Set surfaces as one coherent ON_CHANGE tick.
  const int64_t now = get_time_nanosec();
  response->set_timestamp(now);

  string prefix;
  if (request->has_prefix()) {
    prefix = gnmi_to_xpath(request->prefix());
    response->mutable_prefix()->CopyFrom(request->prefix());
  }

  // ---- Phase 1: validate in spec order (delete, replace, update); mutate nothing.
  for (const auto& delpath : request->delete_()) {
    Status s = checkWritable(registry_, prefix + gnmi_to_xpath(delpath));
    if (!s.ok()) return s;
  }
  for (const auto& upd : request->replace()) {
    Status s = validateWrite(registry_, upd, prefix);
    if (!s.ok()) return s;
  }
  for (const auto& upd : request->update()) {
    Status s = validateWrite(registry_, upd, prefix);
    if (!s.ok()) return s;
  }

  // ---- Phase 2: apply. Validation passed, so these cannot fail.
  // Deleting a nonexistent in-schema leaf is silently accepted (§3.4.6): del()
  // is idempotent. We treat replace as update — flat scalar leaves have no
  // subtree to prune, so there are no omitted siblings to revert.
  for (const auto& delpath : request->delete_()) {
    registry_.del(prefix + gnmi_to_xpath(delpath));
    UpdateResult* res = response->add_response();
    *(res->mutable_path()) = delpath;
    res->set_op(gnmi::UpdateResult::DELETE);
  }
  for (const auto& upd : request->replace()) {
    registry_.set(prefix + gnmi_to_xpath(upd.path()), upd.val(), now);
    UpdateResult* res = response->add_response();
    res->mutable_path()->CopyFrom(upd.path());
    res->set_op(gnmi::UpdateResult::REPLACE);
  }
  for (const auto& upd : request->update()) {
    registry_.set(prefix + gnmi_to_xpath(upd.path()), upd.val(), now);
    UpdateResult* res = response->add_response();
    res->mutable_path()->CopyFrom(upd.path());
    res->set_op(gnmi::UpdateResult::UPDATE);
  }

  return Status::OK;
}

} // namespace impl
