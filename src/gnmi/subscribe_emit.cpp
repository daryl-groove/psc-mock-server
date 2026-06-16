#include "subscribe_emit.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <utils/utils.h>

using google::protobuf::RepeatedPtrField;
using gnmi::Notification;
using gnmi::Update;
using gnmid::core::LeafSnapshot;
using gnmid::core::LeafValueSnapshot;

namespace impl {

namespace {

void appendLeaf(RepeatedPtrField<Update>* list, const std::string& path,
                const gnmi::TypedValue& val)
{
    Update* u = list->Add();
    xpath_to_gnmi_path(path, u->mutable_path());
    *u->mutable_val() = val;
}

// path -> atomic-group prefix, from the view's atomic groups (the GroupView's
// full member list, D13). Only atomic groups bundle.
std::unordered_map<std::string, std::string> atomicIndex(const gnmid::Backend::View& v)
{
    std::unordered_map<std::string, std::string> idx;
    for (const auto& g : v.groups)
        if (g.atomic)
            for (const auto& m : g.memberPaths) idx[m] = g.prefix;
    return idx;
}

// Split a view's leaves into the non-atomic remainder + per-atomic-prefix groups.
void partition(const gnmid::Backend::View& v, LeafSnapshot& nonAtomic,
               std::map<std::string, LeafSnapshot>& groups)
{
    const auto idx = atomicIndex(v);
    for (const auto& [path, leaf] : v.leaves) {
        auto it = idx.find(path);
        if (it != idx.end()) groups[it->second].emplace(path, leaf);
        else                 nonAtomic.emplace(path, leaf);
    }
}

int64_t emitSnapshot(const LeafSnapshot& snap, RepeatedPtrField<Update>* list)
{
    int64_t ts = 0;
    for (const auto& [path, leaf] : snap) {
        appendLeaf(list, path, *leaf.value);
        ts = std::max(ts, leaf.collectedNs);
    }
    return ts;
}

int64_t emitAtomic(const LeafSnapshot& snap, const std::string& prefix, Notification* n)
{
    xpath_to_gnmi_path(prefix, n->mutable_prefix());
    n->set_atomic(true);
    int64_t ts = 0;
    for (const auto& [path, leaf] : snap) {
        // path starts with prefix; emit relative so prefix ++ rel == path.
        const std::string rel = path.substr(prefix.size());
        appendLeaf(n->mutable_update(), rel, *leaf.value);
        ts = std::max(ts, leaf.collectedNs);
    }
    return ts;
}

}  // namespace

LeafDiff diffLeaves(const LeafSnapshot& prev, const LeafSnapshot& cur)
{
    LeafDiff d;
    for (const auto& [path, c] : cur) {
        auto it = prev.find(path);
        if (it == prev.end() || it->second.changeSeq != c.changeSeq)
            d.updated.push_back({ path, c });          // added or value-changed
    }
    for (const auto& [path, p] : prev)
        if (!cur.count(path)) d.removed.push_back(path);   // vanished
    return d;
}

std::vector<Notification> buildFullNotifications(const gnmid::Backend::View& view)
{
    LeafSnapshot nonAtomic;
    std::map<std::string, LeafSnapshot> groups;
    partition(view, nonAtomic, groups);

    std::vector<Notification> out;

    // Emit the non-atomic notification when it has leaves, OR when there are no
    // atomic groups at all — so an empty / purely-scalar query still yields one
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

std::vector<Notification> buildChangeNotifications(const gnmid::Backend::View& prev,
                                                   const gnmid::Backend::View& cur)
{
    LeafSnapshot naPrev, naCur;
    std::map<std::string, LeafSnapshot> gPrev, gCur;
    partition(prev, naPrev, gPrev);
    partition(cur,  naCur, gCur);

    std::vector<Notification> out;

    // Non-atomic leaves: an ordinary diff (updates + deletes).
    LeafDiff d = diffLeaves(naPrev, naCur);
    if (!d.updated.empty() || !d.removed.empty()) {
        Notification n;
        int64_t ts = 0;
        for (const auto& [path, leaf] : d.updated) {
            appendLeaf(n.mutable_update(), path, *leaf.value);
            ts = std::max(ts, leaf.collectedNs);
        }
        for (const auto& path : d.removed)
            xpath_to_gnmi_path(path, n.add_delete_());
        n.set_timestamp(ts ? ts : get_time_nanosec());
        n.set_atomic(false);
        out.push_back(std::move(n));
    }

    // Atomic groups: re-send the whole record on any change (spec §2.1.1).
    std::set<std::string> prefixes;
    for (const auto& kv : gPrev) prefixes.insert(kv.first);
    for (const auto& kv : gCur)  prefixes.insert(kv.first);

    const LeafSnapshot empty;
    for (const auto& prefix : prefixes) {
        auto itPrev = gPrev.find(prefix);
        auto itCur  = gCur.find(prefix);
        const LeafSnapshot& pg = (itPrev != gPrev.end()) ? itPrev->second : empty;
        const LeafSnapshot& cg = (itCur  != gCur.end())  ? itCur->second  : empty;

        LeafDiff gd = diffLeaves(pg, cg);
        if (gd.updated.empty() && gd.removed.empty()) continue;   // unchanged record

        Notification n;
        if (cg.empty()) {
            // Whole record gone: delete the container.
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
           std::chrono::nanoseconds{ static_cast<long long>(intervalNs) };
}

gnmi::SubscriptionMode resolveStreamMode(const gnmi::Subscription& sub,
                                         const gnmid::Backend& be)
{
    if (sub.mode() == gnmi::TARGET_DEFINED)
        return be.preferredMode(gnmi_to_xpath(sub.path()));
    return sub.mode();
}

}  // namespace impl
