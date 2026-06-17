// Worked examples (run as tests) for the canonical gNMI behaviour Scenarios 1, 2, 4,
// 6, 7 from docs/core-data-model-design.md. These are kept as a small REFERENCE set —
// the point is to read them to understand how the protocol layer sits on the core, not
// to exhaustively test every usage permutation. (Device-modelling conventions —
// slots, hot-plug, dynamic leaves — live in docs/device-modeling-conventions.md.)
//
// The monitored-set expansion, diffing, and notification-payload filtering shown here
// belong to the (deferred) protocol layer; each example implements that logic directly
// on top of the core primitives (collectForSubscription, collectLeaves, GroupView,
// writeValues, changeSeq, attach/detachSubtree) to show the core exposes everything
// that layer needs and the spec behaviours fall out (§2.4.2, §3.5.2.5, §3.5.2.1).

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "leaf_registry.hpp"

using namespace gnmid::core;

namespace {

gnmi::TypedValue intValue(int64_t n) {
    gnmi::TypedValue v;
    v.set_int_val(n);
    return v;
}

// The slice of protocol-layer state a subscription derives from the core.
struct Subscription {
    std::set<std::string>  subscribed;  // §2.4.2-expanded leaf set
    std::vector<GroupView> groups;      // distinct owning groups (full member lists)
    std::set<std::string>  monitored;   // subscribed + atomic-group expansion
};

Subscription buildSubscription(const LeafRegistry& reg, const std::vector<std::string>& queries) {
    Subscription sub;
    std::set<std::string> seenGroups;
    for (const std::string& q : queries) {
        SubscriptionView view = reg.collectForSubscription(q);
        for (const auto& [path, _] : view.leaves) sub.subscribed.insert(path);
        for (const auto& g : view.groups) {
            if (seenGroups.insert(g.prefix).second) sub.groups.push_back(g);
        }
    }
    // Monitored set = subscribed leaves + ALL members of any atomic group, so an
    // unsubscribed member's change still triggers a re-send (Scenario 2).
    sub.monitored = sub.subscribed;
    for (const GroupView& g : sub.groups) {
        if (g.atomic) {
            for (const std::string& m : g.memberPaths) sub.monitored.insert(m);
        }
    }
    return sub;
}

const GroupView* groupOf(const Subscription& sub, const std::string& path) {
    for (const GroupView& g : sub.groups) {
        if (std::find(g.memberPaths.begin(), g.memberPaths.end(), path) != g.memberPaths.end()) {
            return &g;
        }
    }
    return nullptr;
}

struct Notification {
    std::string              prefix;
    bool                     atomic = false;
    int64_t                  timestamp = 0;  // §3.5.2.3 single per-Notification stamp
    std::vector<std::string> updates;        // sorted leaf paths
};

// §3.5.2.5: an atomic group re-sends all of its SUBSCRIBED members (timestamp =
// max collectedNs over them, D14 collapse); a non-atomic leaf reports only itself.
Notification notificationFor(const LeafRegistry& reg, const std::string& changedPath,
                             const GroupView* group, const std::set<std::string>& subscribed) {
    Notification n;
    if (group != nullptr && group->atomic) {
        n.prefix = group->prefix;
        n.atomic = true;
        for (const std::string& m : group->memberPaths) {
            if (subscribed.count(m)) {
                n.updates.push_back(m);
                n.timestamp = std::max(n.timestamp, reg.getLeaf(m)->collectedNs);
            }
        }
    } else {
        n.updates.push_back(changedPath);
        n.timestamp = reg.getLeaf(changedPath)->collectedNs;
    }
    std::sort(n.updates.begin(), n.updates.end());
    return n;
}

// Diff two snapshots over a monitored set, using changeSeq (D14).
std::vector<std::string> changedUnder(const LeafSnapshot& before, const LeafSnapshot& after,
                                      const std::set<std::string>& monitored) {
    std::vector<std::string> changed;
    for (const auto& [path, snap] : after) {
        if (!monitored.count(path)) continue;
        auto it = before.find(path);
        if (it == before.end() || it->second.changeSeq != snap.changeSeq) changed.push_back(path);
    }
    std::sort(changed.begin(), changed.end());
    return changed;
}

std::vector<std::string> sortedGroupPrefixes(const Subscription& sub) {
    std::vector<std::string> prefixes;
    for (const GroupView& g : sub.groups) prefixes.push_back(g.prefix);
    std::sort(prefixes.begin(), prefixes.end());
    return prefixes;
}

// A registry pre-populated with the Scenario 1/2 group + leaf topology.
class ScenarioFixture : public ::testing::Test {
protected:
    LeafRegistry              reg;
    std::map<std::string, LeafId> ids;

    void SetUp() override {
        reg.registerGroup("/a/b/c1/d1", /*atomic=*/true);
        reg.registerGroup("/a/b/c1/d2", /*atomic=*/false);
        reg.registerGroup("/a/b/c2/f", /*atomic=*/true);

        int64_t seed = 0;
        for (const char* p : {"/a/b/c1/d1/e1", "/a/b/c1/d1/e2", "/a/b/c1/d2/e1", "/a/b/c1/d2/e2",
                              "/a/b/c2/f/g1", "/a/b/c2/f/g2"}) {
            ids.emplace(p, reg.registerLeaf(p, std::nullopt, intValue(seed++)));
        }
    }

    void setValue(const std::string& path, int64_t v, int64_t ts) {
        auto w = reg.writeValues();
        w.set(ids.at(path), intValue(v), ts);
    }
};

}  // namespace

// Scenario 1 — §2.4.2 container expansion.
TEST_F(ScenarioFixture, ContainerSubscriptionExpandsToAllLeaves) {
    LeafSnapshot c1 = reg.collectLeaves("/a/b/c1");
    std::vector<std::string> paths;
    for (const auto& [p, _] : c1) paths.push_back(p);
    std::sort(paths.begin(), paths.end());
    EXPECT_EQ(paths, (std::vector<std::string>{"/a/b/c1/d1/e1", "/a/b/c1/d1/e2", "/a/b/c1/d2/e1",
                                               "/a/b/c1/d2/e2"}));

    EXPECT_EQ(reg.collectLeaves("/a/b/c2/f/g1").size(), 1u);  // single leaf returns itself
}

// Scenario 2 — atomic group pulls an unsubscribed member into the monitored set.
TEST_F(ScenarioFixture, MonitoredSetExpandsAtomicGroup) {
    Subscription sub = buildSubscription(reg, {"/a/b/c1", "/a/b/c2/f/g1"});

    EXPECT_EQ(sortedGroupPrefixes(sub),
              (std::vector<std::string>{"/a/b/c1/d1", "/a/b/c1/d2", "/a/b/c2/f"}));
    EXPECT_EQ(sub.subscribed.count("/a/b/c2/f/g2"), 0u);  // not subscribed
    EXPECT_EQ(sub.monitored.count("/a/b/c2/f/g2"), 1u);   // but monitored (atomic groupC)
    EXPECT_EQ(sub.monitored.size(), sub.subscribed.size() + 1);
}

// Scenario 2 — §3.5.2.5 an unsubscribed atomic member changes: re-send SUBSCRIBED only.
TEST_F(ScenarioFixture, OnChangeUnsubscribedAtomicMemberResendsSubscribedOnly) {
    Subscription sub = buildSubscription(reg, {"/a/b/c1", "/a/b/c2/f/g1"});
    LeafSnapshot before = reg.collectLeaves("/a/b");

    setValue("/a/b/c2/f/g2", 999, 50);  // the UNSUBSCRIBED atomic member changes

    LeafSnapshot after = reg.collectLeaves("/a/b");
    std::vector<std::string> changed = changedUnder(before, after, sub.monitored);
    ASSERT_EQ(changed, (std::vector<std::string>{"/a/b/c2/f/g2"}));

    Notification n = notificationFor(reg, changed.front(), groupOf(sub, changed.front()),
                                     sub.subscribed);
    EXPECT_EQ(n.prefix, "/a/b/c2/f");
    EXPECT_TRUE(n.atomic);
    EXPECT_EQ(n.updates, (std::vector<std::string>{"/a/b/c2/f/g1"}));  // g2 omitted (§3.5.2.5)
}

// Scenario 2 — a non-atomic member change reports only itself.
TEST_F(ScenarioFixture, OnChangeNonAtomicMemberReportsOnlyItself) {
    Subscription sub = buildSubscription(reg, {"/a/b/c1", "/a/b/c2/f/g1"});
    LeafSnapshot before = reg.collectLeaves("/a/b");

    setValue("/a/b/c1/d2/e2", 777, 50);  // groupB (non-atomic)

    LeafSnapshot after = reg.collectLeaves("/a/b");
    std::vector<std::string> changed = changedUnder(before, after, sub.monitored);
    ASSERT_EQ(changed, (std::vector<std::string>{"/a/b/c1/d2/e2"}));

    Notification n = notificationFor(reg, changed.front(), groupOf(sub, changed.front()),
                                     sub.subscribed);
    EXPECT_FALSE(n.atomic);
    EXPECT_EQ(n.updates, (std::vector<std::string>{"/a/b/c1/d2/e2"}));
}

// Scenario 6 — atomic coherence via one writeValues scope + D14 timestamp collapse.
TEST_F(ScenarioFixture, AtomicGroupOneScopeProducesCollapsedTimestamp) {
    Subscription sub = buildSubscription(reg, {"/a/b/c1/d1"});  // groupA, both members
    LeafSnapshot before = reg.collectLeaves("/a/b");

    {
        auto w = reg.writeValues();  // one scope = atomic coherence boundary
        w.set(ids.at("/a/b/c1/d1/e1"), intValue(11), 100);
        w.set(ids.at("/a/b/c1/d1/e2"), intValue(22), 130);
    }

    LeafSnapshot after = reg.collectLeaves("/a/b");
    std::vector<std::string> changed = changedUnder(before, after, sub.monitored);
    ASSERT_FALSE(changed.empty());

    Notification n = notificationFor(reg, changed.front(), groupOf(sub, changed.front()),
                                     sub.subscribed);
    EXPECT_TRUE(n.atomic);
    EXPECT_EQ(n.prefix, "/a/b/c1/d1");
    EXPECT_EQ(n.updates, (std::vector<std::string>{"/a/b/c1/d1/e1", "/a/b/c1/d1/e2"}));
    EXPECT_EQ(n.timestamp, 130);  // max(collectedNs) over included members (D14)
}

// Scenario 4 — hot-plug attach/detach of a whole device branch.
TEST_F(ScenarioFixture, HotPlugAttachThenDetachBranch) {
    const std::string psu = "/components/psu[name=PSU3]";
    SubtreeSpec spec;
    spec.groups.push_back({psu, /*atomic=*/true, std::nullopt});
    spec.leaves.push_back({psu + "/temp", std::nullopt, std::nullopt});
    spec.leaves.push_back({psu + "/volt", std::nullopt, std::nullopt});

    auto attached = reg.attachSubtree(spec);
    Subscription sub = buildSubscription(reg, {"/components"});
    EXPECT_EQ(sub.subscribed.size(), 2u);
    EXPECT_EQ(sub.monitored.count(psu + "/temp"), 1u);

    reg.detachSubtree(psu);
    EXPECT_TRUE(reg.collectLeaves("/components").empty());

    auto w = reg.writeValues();
    EXPECT_FALSE(w.set(attached.at(psu + "/temp"), intValue(1), 1));  // stale id -> clean miss
}

// Scenario 7 — one node, many spellings, one monitored leaf.
TEST_F(ScenarioFixture, CanonicalEquivalenceAcrossSpellings) {
    reg.registerGroup("/components/component[name=psu0]", /*atomic=*/true);
    reg.registerLeaf("/components/component[class=POWER_SUPPLY][name=psu0]/state/temperature");

    // Reordered keys + trailing slash subtree query must hit the same node.
    SubscriptionView v = reg.collectForSubscription(
        "/components/component[name=\"psu0\"][class=POWER_SUPPLY]/state/");
    ASSERT_EQ(v.leaves.size(), 1u);
    EXPECT_EQ(v.leaves.begin()->first,
              "/components/component[class=POWER_SUPPLY][name=psu0]/state/temperature");
}
