#include "subscribe_emit.h"

#include <algorithm>
#include <string>

#include <utils/utils.h>

using google::protobuf::RepeatedPtrField;
using gnmi::Notification;
using gnmi::Update;

namespace impl {

namespace {

// Append one Update (path + value) to a Notification's update list.
void appendLeaf(RepeatedPtrField<Update>* list, const std::string& path,
                const gnmi::TypedValue& val)
{
  Update* u = list->Add();
  xpath_to_gnmi_path(path, u->mutable_path());
  *u->mutable_val() = val;
}

} // namespace

int64_t emitSnapshot(const Snapshot& snap, RepeatedPtrField<Update>* list)
{
  int64_t ts = 0;
  for (const auto& [path, leaf] : snap) {
    appendLeaf(list, path, leaf.val);
    ts = std::max(ts, leaf.collectedNs);
  }
  return ts;
}

int64_t emitDiff(const LeafStore::Diff& diff, Notification* notification)
{
  int64_t ts = 0;
  for (const auto& [path, leaf] : diff.updated) {       // changed / added
    appendLeaf(notification->mutable_update(), path, leaf.val);
    ts = std::max(ts, leaf.collectedNs);
  }
  for (const auto& path : diff.removed)                 // vanished (§3.5.2.3)
    xpath_to_gnmi_path(path, notification->add_delete_());
  return ts;
}

bool heartbeatDue(uint64_t intervalNs,
                  std::chrono::high_resolution_clock::time_point last,
                  std::chrono::high_resolution_clock::time_point now)
{
  if (intervalNs == 0) return false;                    // heartbeats disabled
  return now - last >=
         std::chrono::nanoseconds{static_cast<long long>(intervalNs)};
}

gnmi::SubscriptionMode resolveStreamMode(const gnmi::Subscription& sub,
                                         const DataProviderRegistry& registry)
{
  if (sub.mode() == gnmi::TARGET_DEFINED)
    return registry.preferredMode(gnmi_to_xpath(sub.path()));
  return sub.mode();
}

} // namespace impl
