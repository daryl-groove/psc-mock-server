#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "leaf_registry.hpp"

using namespace gnmid::core;

namespace {

std::vector<std::string> membersOf(const LeafRegistry& reg, const std::string& group) {
    auto view = reg.getGroup(group);
    return view ? view->memberPaths : std::vector<std::string>{};
}

}  // namespace

TEST(LeafRegistryRegistration, RegisterGroupThenGetGroup) {
    LeafRegistry reg;
    reg.registerGroup("g", "/a/b/c", /*atomic=*/true);

    auto view = reg.getGroup("g");
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->name, "g");
    EXPECT_EQ(view->prefix, "/a/b/c");
    EXPECT_TRUE(view->atomic);
}

TEST(LeafRegistryRegistration, GetGroupMissingReturnsNullopt) {
    LeafRegistry reg;
    EXPECT_FALSE(reg.getGroup("nope").has_value());
}

TEST(LeafRegistryRegistration, GetLeafMissingReturnsNullopt) {
    LeafRegistry reg;
    EXPECT_FALSE(reg.getLeaf("/no/such/leaf").has_value());
}

TEST(LeafRegistryRegistration, RegisterLeafAutoAssignsToMatchingGroup) {
    LeafRegistry reg;
    reg.registerGroup("g", "/a/b/c", /*atomic=*/false);
    reg.registerLeaf("/a/b/c/d");

    EXPECT_EQ(membersOf(reg, "g"), (std::vector<std::string>{"/a/b/c/d"}));
}

TEST(LeafRegistryRegistration, RegisterLeafWithNoMatchingGroupIsUngrouped) {
    LeafRegistry reg;
    reg.registerGroup("g", "/a/b/c", /*atomic=*/false);
    reg.registerLeaf("/x/y/z");

    EXPECT_TRUE(membersOf(reg, "g").empty());
    EXPECT_TRUE(reg.getLeaf("/x/y/z").has_value());
}

TEST(LeafRegistryRegistration, AutoAssignPicksMostSpecificAncestor) {
    // Sibling groups, both legal under D5. The leaf belongs to /a/b only; the
    // dashed-sibling /a/b-x must not capture it — the case a single upper_bound step
    // gets wrong (/a/b < /a/b-x < /a/b/c).
    LeafRegistry reg;
    reg.registerGroup("gb", "/a/b", /*atomic=*/false);
    reg.registerGroup("gdash", "/a/b-x", /*atomic=*/false);
    reg.registerLeaf("/a/b/c");

    EXPECT_EQ(membersOf(reg, "gb"), (std::vector<std::string>{"/a/b/c"}));
    EXPECT_TRUE(membersOf(reg, "gdash").empty());
}

TEST(LeafRegistryRegistration, NonOverlappingStringPrefixGroupsCoexist) {
    // /a/b and /a/bc are string-prefixes but not path-ancestors → both legal (J).
    LeafRegistry reg;
    EXPECT_NO_THROW(reg.registerGroup("g1", "/a/b", /*atomic=*/true));
    EXPECT_NO_THROW(reg.registerGroup("g2", "/a/bc", /*atomic=*/false));

    reg.registerLeaf("/a/b/x");
    reg.registerLeaf("/a/bc/y");
    EXPECT_EQ(membersOf(reg, "g1"), (std::vector<std::string>{"/a/b/x"}));
    EXPECT_EQ(membersOf(reg, "g2"), (std::vector<std::string>{"/a/bc/y"}));
}

TEST(LeafRegistryRegistration, D5OverlapDescendantThrows) {
    LeafRegistry reg;
    reg.registerGroup("ancestor", "/a/b", /*atomic=*/true);
    EXPECT_THROW(reg.registerGroup("descendant", "/a/b/c/d", /*atomic=*/false),
                 std::invalid_argument);
}

TEST(LeafRegistryRegistration, D5OverlapAncestorThrows) {
    LeafRegistry reg;
    reg.registerGroup("descendant", "/a/b/c/d", /*atomic=*/false);
    EXPECT_THROW(reg.registerGroup("ancestor", "/a/b", /*atomic=*/true), std::invalid_argument);
}

TEST(LeafRegistryRegistration, D5OverlapIdenticalPrefixThrows) {
    LeafRegistry reg;
    reg.registerGroup("g1", "/a/b/c", /*atomic=*/false);
    EXPECT_THROW(reg.registerGroup("g2", "/a/b/c", /*atomic=*/true), std::invalid_argument);
}

TEST(LeafRegistryRegistration, SiblingPrefixesAreAllowed) {
    LeafRegistry reg;
    EXPECT_NO_THROW(reg.registerGroup("a", "/a/b/c/d1", /*atomic=*/true));
    EXPECT_NO_THROW(reg.registerGroup("b", "/a/b/c/d2", /*atomic=*/false));
}

TEST(LeafRegistryRegistration, DuplicateGroupNameThrows) {
    LeafRegistry reg;
    reg.registerGroup("g", "/a/b", /*atomic=*/false);
    EXPECT_THROW(reg.registerGroup("g", "/x/y", /*atomic=*/false), std::invalid_argument);
}

TEST(LeafRegistryRegistration, DuplicateLeafPathThrows) {
    LeafRegistry reg;
    reg.registerLeaf("/a/b/c");
    EXPECT_THROW(reg.registerLeaf("/a/b/c"), std::invalid_argument);
}

TEST(LeafRegistryRegistration, CanonicalEquivalenceQuotedUnquotedSameLeaf) {
    LeafRegistry reg;
    reg.registerLeaf("/a/b[name=\"eth0\"]/c");

    // Either spelling resolves to the same canonical leaf.
    EXPECT_TRUE(reg.getLeaf("/a/b[name=\"eth0\"]/c").has_value());
    EXPECT_TRUE(reg.getLeaf("/a/b[name=eth0]/c").has_value());
    // Re-registering the other spelling is a duplicate (same node).
    EXPECT_THROW(reg.registerLeaf("/a/b[name=eth0]/c"), std::invalid_argument);
}

TEST(LeafRegistryRegistration, CanonicalEquivalenceKeyOrderAndTrailingSlash) {
    LeafRegistry reg;
    reg.registerLeaf("/c[k1=v1][k2=v2]/x");

    EXPECT_TRUE(reg.getLeaf("/c[k2=v2][k1=v1]/x").has_value());  // reordered keys
    EXPECT_TRUE(reg.getLeaf("/c[k1=v1][k2=v2]/x/").has_value());  // trailing slash
}

TEST(LeafRegistryRegistration, NormalizedLeafAutoAssignsUnderNormalizedGroupPrefix) {
    LeafRegistry reg;
    reg.registerGroup("g", "/if/i[name=\"eth0\"]", /*atomic=*/true);
    EXPECT_EQ(reg.getGroup("g")->prefix, "/if/i[name=eth0]");

    reg.registerLeaf("/if/i[name=\"eth0\"]/state/oper");
    EXPECT_EQ(membersOf(reg, "g"), (std::vector<std::string>{"/if/i[name=eth0]/state/oper"}));
}

TEST(LeafRegistryRegistration, LeafRegisteredBeforeGroupIsNotRetroactivelyAssigned) {
    // "Groups before leaves" ordering convention (D3): a leaf registered first finds
    // no group and stays ungrouped even after the group is later registered.
    LeafRegistry reg;
    reg.registerLeaf("/a/b/c/d");
    reg.registerGroup("g", "/a/b/c", /*atomic=*/false);

    EXPECT_TRUE(membersOf(reg, "g").empty());
}

// --- Advisory D5 pre-checks (registeredPrefixes / wouldConflict) -----------

TEST(LeafRegistryRegistration, RegisteredPrefixesListsSortedCanonicalPrefixes) {
    LeafRegistry reg;
    reg.registerGroup("g2", "/a/x", /*atomic=*/false);
    reg.registerGroup("g1", "/a/b[name=\"eth0\"]", /*atomic=*/true);  // quoted spelling

    EXPECT_EQ(reg.registeredPrefixes(),
              (std::vector<std::string>{"/a/b[name=eth0]", "/a/x"}));  // sorted + canonical
}

TEST(LeafRegistryRegistration, WouldConflictReportsTheClashingAncestorOrDescendant) {
    LeafRegistry reg;
    reg.registerGroup("g", "/a/b/c", /*atomic=*/false);

    EXPECT_EQ(reg.wouldConflict("/a/b"), "/a/b/c");        // new is ancestor of existing
    EXPECT_EQ(reg.wouldConflict("/a/b/c/d"), "/a/b/c");    // new is descendant of existing
    EXPECT_EQ(reg.wouldConflict("/a/b/c"), "/a/b/c");      // identical
    EXPECT_EQ(reg.wouldConflict("/a/b/c/"), "/a/b/c");     // canonicalized (trailing slash)
}

TEST(LeafRegistryRegistration, WouldConflictReturnsNulloptForFreeOrSiblingPrefix) {
    LeafRegistry reg;
    reg.registerGroup("g", "/a/b/c", /*atomic=*/false);

    EXPECT_FALSE(reg.wouldConflict("/a/b/d").has_value());  // sibling
    EXPECT_FALSE(reg.wouldConflict("/a/bc").has_value());   // string-prefix, not path-ancestor
    EXPECT_FALSE(reg.wouldConflict("/x/y").has_value());    // unrelated
}

TEST(LeafRegistryRegistration, WouldConflictAgreesWithRegisterGroupOutcome) {
    LeafRegistry reg;
    reg.registerGroup("g", "/a/b/c", /*atomic=*/false);

    // Advisory check agrees with the authoritative throw: conflicting -> throws,
    // free -> registers.
    ASSERT_TRUE(reg.wouldConflict("/a/b").has_value());
    EXPECT_THROW(reg.registerGroup("clash", "/a/b", /*atomic=*/false), std::invalid_argument);

    ASSERT_FALSE(reg.wouldConflict("/a/b/d").has_value());
    EXPECT_NO_THROW(reg.registerGroup("ok", "/a/b/d", /*atomic=*/false));
}

TEST(LeafRegistryRegistration, OverlapErrorMessageExplainsWhy) {
    LeafRegistry reg;
    reg.registerGroup("g", "/a/b", /*atomic=*/true);
    try {
        reg.registerGroup("clash", "/a/b/c", /*atomic=*/false);
        FAIL() << "expected overlap to throw";
    } catch (const std::invalid_argument& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("/a/b"), std::string::npos);     // names both prefixes
        EXPECT_NE(msg.find("§2.1.1"), std::string::npos);   // states the gNMI reason
        EXPECT_NE(msg.find("wouldConflict"), std::string::npos);  // points at the pre-check
    }
}
