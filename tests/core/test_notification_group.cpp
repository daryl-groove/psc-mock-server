#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "leaf_registry.hpp"
#include "leaf_type.hpp"

using namespace gnmid::core;

namespace {

// Group membership is observed via the registry's GroupView (encapsulation: the
// live NotificationGroup never escapes). memberPaths is already sorted.
std::vector<std::string> membersOf(const LeafRegistry& reg, const std::string& group) {
    auto view = reg.getGroup(group);
    return view ? view->memberPaths : std::vector<std::string>{};
}

LeafType effectiveTypeOf(const LeafRegistry& reg, const std::string& path) {
    auto snap = reg.getLeaf(path);
    return snap->effectiveType;
}

}  // namespace

TEST(NotificationGroup, RegisterLeafUnderPrefixAutoLinks) {
    LeafRegistry reg;
    reg.registerGroup("g", "/a/b/c", false);
    reg.registerLeaf("/a/b/c/d");

    EXPECT_EQ(membersOf(reg, "g"), (std::vector<std::string>{"/a/b/c/d"}));
}

TEST(NotificationGroup, LeafOutsideEveryPrefixStaysUngrouped) {
    LeafRegistry reg;
    reg.registerGroup("g", "/a/b/c", false);
    reg.registerLeaf("/x/y/z");

    EXPECT_TRUE(membersOf(reg, "g").empty());
    EXPECT_EQ(effectiveTypeOf(reg, "/x/y/z"), LeafType::Operational);  // ungrouped default
}

TEST(NotificationGroup, PrefixStringMatchThatIsNotPathDescendantDoesNotLink) {
    // "/a/bc" starts with "/a/b" as a string but is not a path descendant (finding J).
    LeafRegistry reg;
    reg.registerGroup("g", "/a/b", false);
    reg.registerLeaf("/a/bc");

    EXPECT_TRUE(membersOf(reg, "g").empty());
}

TEST(NotificationGroup, BareListPrefixDoesNotCaptureKeyedEntry) {
    // /a/b (bare list) vs /a/b[name=x] (a list entry) are different nodes (D16).
    LeafRegistry reg;
    reg.registerGroup("g", "/a/b", false);
    reg.registerLeaf("/a/b[name=x]/c");

    EXPECT_TRUE(membersOf(reg, "g").empty());
}

TEST(NotificationGroup, EffectiveTypePrefersLeafOwnType) {
    LeafRegistry reg;
    reg.registerGroup("g", "/a", false, LeafType::State);
    reg.registerLeaf("/a/b", LeafType::Config);

    EXPECT_EQ(effectiveTypeOf(reg, "/a/b"), LeafType::Config);
}

TEST(NotificationGroup, EffectiveTypeFallsBackToGroupPreferredType) {
    LeafRegistry reg;
    reg.registerGroup("g", "/a", false, LeafType::State);
    reg.registerLeaf("/a/b");

    EXPECT_EQ(effectiveTypeOf(reg, "/a/b"), LeafType::State);
}

TEST(NotificationGroup, EffectiveTypeDefaultsToOperational) {
    LeafRegistry reg;
    reg.registerGroup("g", "/a", false);  // no preferredType
    reg.registerLeaf("/a/b");

    EXPECT_EQ(effectiveTypeOf(reg, "/a/b"), LeafType::Operational);
}

TEST(NotificationGroup, MemberListIsSorted) {
    LeafRegistry reg;
    reg.registerGroup("g", "/a", false);
    reg.registerLeaf("/a/3");
    reg.registerLeaf("/a/1");
    reg.registerLeaf("/a/2");

    EXPECT_EQ(membersOf(reg, "g"), (std::vector<std::string>{"/a/1", "/a/2", "/a/3"}));
}
