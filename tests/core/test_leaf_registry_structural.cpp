#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "leaf_registry.hpp"

using namespace gnmid::core;

namespace {

std::vector<std::string> membersOf(const LeafRegistry& reg, const std::string& prefix) {
    auto view = reg.getGroup(prefix);
    return view ? view->memberPaths : std::vector<std::string>{};
}

std::vector<std::string> sortedLeaves(const SubscriptionView& view) {
    std::vector<std::string> paths;
    for (const auto& [p, _] : view.leaves) paths.push_back(p);
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::vector<std::string> sortedGroupPrefixes(const SubscriptionView& view) {
    std::vector<std::string> prefixes;
    for (const auto& g : view.groups) prefixes.push_back(g.prefix);
    std::sort(prefixes.begin(), prefixes.end());
    return prefixes;
}

}  // namespace

// --- unregisterLeaf (D1 erase order) --------------------------------------

TEST(Structural, UnregisterLeafRemovesFromRegistryAndGroup) {
    LeafRegistry reg;
    reg.registerGroup("/a/b", /*atomic=*/false);
    reg.registerLeaf("/a/b/c");
    ASSERT_EQ(membersOf(reg, "/a/b"), (std::vector<std::string>{"/a/b/c"}));

    reg.unregisterLeaf("/a/b/c");
    EXPECT_FALSE(reg.getLeaf("/a/b/c").has_value());
    EXPECT_TRUE(membersOf(reg, "/a/b").empty());  // gone from the group's member set
}

TEST(Structural, UnregisterLeafNormalizesPath) {
    LeafRegistry reg;
    reg.registerLeaf("/a/b[name=eth0]/c");

    reg.unregisterLeaf("/a/b[name=\"eth0\"]/c");  // quoted spelling, same node
    EXPECT_FALSE(reg.getLeaf("/a/b[name=eth0]/c").has_value());
}

TEST(Structural, UnregisterUngroupedOrMissingLeafIsSafe) {
    LeafRegistry reg;
    reg.registerLeaf("/lonely");
    EXPECT_NO_THROW(reg.unregisterLeaf("/lonely"));
    EXPECT_FALSE(reg.getLeaf("/lonely").has_value());
    EXPECT_NO_THROW(reg.unregisterLeaf("/does/not/exist"));
}

// --- unregisterGroup (D9 survivors) ---------------------------------------

TEST(Structural, UnregisterGroupLeavesMembersAsUngrouped) {
    LeafRegistry reg;
    reg.registerGroup("/a/b", /*atomic=*/false, LeafType::State);
    reg.registerLeaf("/a/b/c1");
    reg.registerLeaf("/a/b/c2");

    reg.unregisterGroup("/a/b");

    EXPECT_FALSE(reg.getGroup("/a/b").has_value());
    // Members remain as ungrouped units; effectiveType reverts to the default (D9).
    ASSERT_TRUE(reg.getLeaf("/a/b/c1").has_value());
    ASSERT_TRUE(reg.getLeaf("/a/b/c2").has_value());
    EXPECT_EQ(reg.getLeaf("/a/b/c1")->effectiveType, LeafType::Operational);
}

TEST(Structural, UnregisterGroupFreesPrefixForReuse) {
    LeafRegistry reg;
    reg.registerGroup("/a/b", /*atomic=*/false);
    reg.unregisterGroup("/a/b");

    EXPECT_NO_THROW(reg.registerGroup("/a/b", /*atomic=*/true));  // prefix index released
    EXPECT_NO_THROW(reg.unregisterGroup("/nope"));                 // missing is a no-op
}

TEST(Structural, NewLeafUnderFreedPrefixIsUngrouped) {
    LeafRegistry reg;
    reg.registerGroup("/a/b", /*atomic=*/false);
    reg.unregisterGroup("/a/b");
    reg.registerLeaf("/a/b/c");

    EXPECT_EQ(reg.getLeaf("/a/b/c")->effectiveType, LeafType::Operational);
}

// --- attachSubtree / detachSubtree (D12) ----------------------------------

TEST(Structural, AttachSubtreeAddsGroupsAndLeavesAndAutoAssigns) {
    LeafRegistry reg;
    SubtreeSpec spec;
    spec.groups.push_back({"/components/psu[name=PSU3]", true, std::nullopt});
    spec.leaves.push_back({"/components/psu[name=PSU3]/temp", std::nullopt, std::nullopt});
    spec.leaves.push_back({"/components/psu[name=PSU3]/volt", std::nullopt, std::nullopt});

    auto ids = reg.attachSubtree(spec);

    EXPECT_EQ(ids.size(), 2u);
    EXPECT_TRUE(reg.getGroup("/components/psu[name=PSU3]").has_value());
    EXPECT_EQ(membersOf(reg, "/components/psu[name=PSU3]"),
              (std::vector<std::string>{"/components/psu[name=PSU3]/temp",
                                        "/components/psu[name=PSU3]/volt"}));
}

TEST(Structural, DetachSubtreeRemovesWholeBranch) {
    LeafRegistry reg;
    reg.registerLeaf("/components/other");  // outside the branch, must survive
    SubtreeSpec spec;
    spec.groups.push_back({"/components/psu[name=PSU3]", true, std::nullopt});
    spec.leaves.push_back({"/components/psu[name=PSU3]/temp", std::nullopt, std::nullopt});
    spec.leaves.push_back({"/components/psu[name=PSU3]/volt", std::nullopt, std::nullopt});
    reg.attachSubtree(spec);

    reg.detachSubtree("/components/psu[name=PSU3]");

    EXPECT_FALSE(reg.getGroup("/components/psu[name=PSU3]").has_value());
    EXPECT_FALSE(reg.getLeaf("/components/psu[name=PSU3]/temp").has_value());
    EXPECT_FALSE(reg.getLeaf("/components/psu[name=PSU3]/volt").has_value());
    EXPECT_TRUE(reg.getLeaf("/components/other").has_value());  // sibling untouched
}

TEST(Structural, StaleLeafIdAfterDetachSetReturnsFalse) {
    LeafRegistry reg;
    SubtreeSpec spec;
    spec.leaves.push_back({"/components/psu[name=PSU3]/temp", std::nullopt, std::nullopt});
    auto ids = reg.attachSubtree(spec);
    ASSERT_EQ(ids.size(), 1u);

    reg.detachSubtree("/components/psu[name=PSU3]");

    auto w = reg.writeValues();
    gnmi::TypedValue v;
    v.set_string_val("42");
    EXPECT_FALSE(w.set(ids.at("/components/psu[name=PSU3]/temp"), v, 1));  // clean miss, never UB
}

TEST(Structural, DetachLeavesBroaderGroupAliveMinusUnlinkedLeaves) {
    // A group whose prefix is an ancestor of the detached branch survives; only the
    // leaves under the detached sub-branch are unlinked and removed.
    LeafRegistry reg;
    reg.registerGroup("/components", /*atomic=*/false);
    reg.registerLeaf("/components/keep");
    reg.registerLeaf("/components/sub/gone");

    reg.detachSubtree("/components/sub");

    EXPECT_TRUE(reg.getGroup("/components").has_value());
    EXPECT_EQ(membersOf(reg, "/components"), (std::vector<std::string>{"/components/keep"}));
    EXPECT_FALSE(reg.getLeaf("/components/sub/gone").has_value());
}

// --- collectForSubscription (D13) -----------------------------------------

TEST(Structural, CollectForSubscriptionReturnsLeavesAndOwningGroups) {
    LeafRegistry reg;
    reg.registerGroup("/a/b/c1/d1", /*atomic=*/true);
    reg.registerGroup("/a/b/c1/d2", /*atomic=*/false);
    reg.registerGroup("/a/b/c2/f", /*atomic=*/true);
    reg.registerLeaf("/a/b/c1/d1/e1");
    reg.registerLeaf("/a/b/c1/d1/e2");  // same group -> deduped in view.groups
    reg.registerLeaf("/a/b/c1/d2/e1");
    reg.registerLeaf("/a/b/c2/f/g1");
    reg.registerLeaf("/a/ungrouped");

    SubscriptionView v1 = reg.collectForSubscription("/a/b/c1");
    EXPECT_EQ(sortedLeaves(v1), (std::vector<std::string>{"/a/b/c1/d1/e1", "/a/b/c1/d1/e2",
                                                          "/a/b/c1/d2/e1"}));
    EXPECT_EQ(sortedGroupPrefixes(v1),
              (std::vector<std::string>{"/a/b/c1/d1", "/a/b/c1/d2"}));

    SubscriptionView vAll = reg.collectForSubscription("/a");
    EXPECT_EQ(sortedGroupPrefixes(vAll),
              (std::vector<std::string>{"/a/b/c1/d1", "/a/b/c1/d2", "/a/b/c2/f"}));
}

TEST(Structural, CollectForSubscriptionGroupViewHasFullMemberList) {
    // Scenario 2: an atomic group's view carries ALL members, including ones outside
    // the query, so the protocol layer can expand the monitored set.
    LeafRegistry reg;
    reg.registerGroup("/a/b/c2/f", /*atomic=*/true);
    reg.registerLeaf("/a/b/c2/f/g1");
    reg.registerLeaf("/a/b/c2/f/g2");

    SubscriptionView v = reg.collectForSubscription("/a/b/c2/f/g1");  // subscribe to g1 only
    EXPECT_EQ(sortedLeaves(v), (std::vector<std::string>{"/a/b/c2/f/g1"}));
    ASSERT_EQ(v.groups.size(), 1u);
    EXPECT_EQ(v.groups[0].memberPaths,
              (std::vector<std::string>{"/a/b/c2/f/g1", "/a/b/c2/f/g2"}));  // g2 included
}
