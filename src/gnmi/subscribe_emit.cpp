#include "subscribe_emit.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

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

// Split a snapshot into atomic groups (keyed by their atomic-container prefix)
// and the non-atomic remainder, asking the registry which prefix owns each leaf.
void partition(const Snapshot& snap, const DataProviderRegistry& registry,
               Snapshot& nonAtomic, std::map<std::string, Snapshot>& groups)
{
  for (const auto& [path, leaf] : snap) {
    if (auto ap = registry.atomicPrefix(path))
      groups[*ap].emplace(path, leaf);
    else
      nonAtomic.emplace(path, leaf);
  }
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

int64_t emitAtomic(const Snapshot& snap, const std::string& atomicPrefix,
                   Notification* n)
{
  xpath_to_gnmi_path(atomicPrefix, n->mutable_prefix());
  n->set_atomic(true);
  int64_t ts = 0;
  for (const auto& [path, leaf] : snap) {
    // path starts with atomicPrefix; emit it relative so prefix ++ path == path.
    const std::string rel = path.substr(atomicPrefix.size());
    appendLeaf(n->mutable_update(), rel, leaf.val);
    ts = std::max(ts, leaf.collectedNs);
  }
  return ts;
}

std::vector<Notification> buildFullNotifications(
    const Snapshot& snap, const DataProviderRegistry& registry)
{
  Snapshot nonAtomic;
  std::map<std::string, Snapshot> groups;
  partition(snap, registry, nonAtomic, groups);

  std::vector<Notification> out;

  // Emit the non-atomic notification when it has leaves, OR when there are no
  // atomic groups at all — so an empty or purely-scalar query still yields one
  // notification (ONCE/POLL must send a notification before sync_response).
  if (!nonAtomic.empty() || groups.empty()) {
    Notification n;
    int64_t ts = emitSnapshot(nonAtomic, n.mutable_update());
    n.set_timestamp(ts ? ts : get_time_nanosec());
    n.set_atomic(false);
    out.push_back(std::move(n));
  }

  for (const auto& [prefix, group] : groups) {
    Notification n;
    int64_t ts = emitAtomic(group, prefix, &n);
    n.set_timestamp(ts ? ts : get_time_nanosec());
    out.push_back(std::move(n));
  }
  return out;
}

std::vector<Notification> buildChangeNotifications(
    const Snapshot& prev, const Snapshot& cur,
    const DataProviderRegistry& registry)
{
  Snapshot naPrev, naCur;
  std::map<std::string, Snapshot> gPrev, gCur;
  partition(prev, registry, naPrev, gPrev);
  partition(cur,  registry, naCur, gCur);

  std::vector<Notification> out;

  // Non-atomic leaves: an ordinary diff (updates + deletes).
  LeafStore::Diff d = LeafStore::diff(naPrev, naCur);
  if (!d.updated.empty() || !d.removed.empty()) {
    Notification n;
    int64_t ts = emitDiff(d, &n);
    n.set_timestamp(ts ? ts : get_time_nanosec());
    n.set_atomic(false);
    out.push_back(std::move(n));
  }

  // Atomic containers: re-send the whole record on any change (spec §2.1.1).
  std::set<std::string> prefixes;
  for (const auto& [p, _] : gPrev) prefixes.insert(p);
  for (const auto& [p, _] : gCur)  prefixes.insert(p);

  const Snapshot empty;
  for (const auto& prefix : prefixes) {
    auto itPrev = gPrev.find(prefix);
    auto itCur  = gCur.find(prefix);
    const Snapshot& pg = (itPrev != gPrev.end()) ? itPrev->second : empty;
    const Snapshot& cg = (itCur  != gCur.end())  ? itCur->second  : empty;

    LeafStore::Diff gd = LeafStore::diff(pg, cg);
    if (gd.updated.empty() && gd.removed.empty()) continue;   // unchanged record

    Notification n;
    if (cg.empty()) {
      // Whole record gone: delete the container (empty path under its prefix).
      xpath_to_gnmi_path(prefix, n.mutable_prefix());
      n.set_atomic(true);
      n.add_delete_();
      n.set_timestamp(get_time_nanosec());
    } else {
      int64_t ts = emitAtomic(cg, prefix, &n);              // complete state
      n.set_timestamp(ts ? ts : get_time_nanosec());
    }
    out.push_back(std::move(n));
  }
  return out;
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
