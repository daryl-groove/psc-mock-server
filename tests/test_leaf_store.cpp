#include <gtest/gtest.h>
#include "backend/leaf_store.hpp"

namespace {

// Seed a small two-unit sensor tree mirroring the PSC layout.
// LeafStore is non-movable (holds a shared_mutex), so seed a reference.
void seed(LeafStore& s) {
    s.set("/components/component[name=PSC-0]/state/temperature/instant", 45.0, 10);
    s.set("/components/component[name=PSC-0]/power-supply/state/input-voltage", 54.0, 10);
    s.set("/components/component[name=PSC-0]/power-supply/state/output-power", 240.0, 10);
    s.set("/components/component[name=PSC-1]/state/temperature/instant", 46.0, 10);
}

} // namespace

// ---------------------------------------------------------------------------
// set / get
// ---------------------------------------------------------------------------

TEST(LeafStore, GetReturnsValueAndTimestamp) {
    LeafStore s;
    s.set("/a/b", 3.5, 123);

    auto leaf = s.get("/a/b");
    ASSERT_TRUE(leaf.has_value());
    EXPECT_DOUBLE_EQ(leaf->val.double_val(), 3.5);
    EXPECT_EQ(leaf->collectedNs, 123);
}

TEST(LeafStore, GetMissingReturnsNullopt) {
    LeafStore s;
    EXPECT_FALSE(s.get("/nope").has_value());
}

TEST(LeafStore, SetOverwritesValueAndTimestamp) {
    LeafStore s;
    s.set("/a", 1.0, 10);
    s.set("/a", 2.0, 20);

    auto leaf = s.get("/a");
    ASSERT_TRUE(leaf.has_value());
    EXPECT_DOUBLE_EQ(leaf->val.double_val(), 2.0);
    EXPECT_EQ(leaf->collectedNs, 20);
}

TEST(LeafStore, QuotedAndUnquotedKeysAreEquivalent) {
    LeafStore s;
    s.set("/components/component[name=PSC-0]/x", 7.0, 1);
    EXPECT_TRUE(s.get("/components/component[name=\"PSC-0\"]/x").has_value());
}

TEST(LeafStore, RemoveDeletesLeaf) {
    LeafStore s;
    s.set("/a", 1.0, 1);
    s.remove("/a");
    EXPECT_FALSE(s.get("/a").has_value());
}

// ---------------------------------------------------------------------------
// collect — subtree match
// ---------------------------------------------------------------------------

TEST(LeafStore, CollectExactLeaf) {
    LeafStore s; seed(s);
    RepeatedPtrField<Update> list;
    EXPECT_TRUE(s.collect(
        "/components/component[name=PSC-0]/state/temperature/instant", &list));
    EXPECT_EQ(list.size(), 1);
}

TEST(LeafStore, CollectSubtree) {
    LeafStore s; seed(s);
    RepeatedPtrField<Update> list;
    EXPECT_TRUE(s.collect(
        "/components/component[name=PSC-0]/power-supply/state", &list));
    EXPECT_EQ(list.size(), 2);  // input-voltage + output-power
}

TEST(LeafStore, CollectKeylessFansOutToAllUnits) {
    LeafStore s; seed(s);
    RepeatedPtrField<Update> list;
    // No [name=...] → both PSC-0 and PSC-1 temperature leaves.
    EXPECT_TRUE(s.collect("/components/component/state/temperature/instant", &list));
    EXPECT_EQ(list.size(), 2);
}

TEST(LeafStore, CollectNoMatchReturnsFalse) {
    LeafStore s; seed(s);
    RepeatedPtrField<Update> list;
    EXPECT_FALSE(s.collect("/components/component[name=FAN-0]/state", &list));
    EXPECT_EQ(list.size(), 0);
}

TEST(LeafStore, CollectRespectsSegmentBoundary) {
    LeafStore s;
    s.set("/a/power-supply/x", 1.0, 1);
    RepeatedPtrField<Update> list;
    // "/a/power" must not match "/a/power-supply/..."
    EXPECT_FALSE(s.collect("/a/power", &list));
    EXPECT_EQ(list.size(), 0);
}

// ---------------------------------------------------------------------------
// snapshot / diff
// ---------------------------------------------------------------------------

TEST(LeafStore, SnapshotIsScopedToQuery) {
    LeafStore s; seed(s);
    auto snap = s.snapshot("/components/component[name=PSC-0]");
    EXPECT_EQ(snap.size(), 3u);  // PSC-0's three leaves, not PSC-1
}

TEST(LeafStore, DiffDetectsChangedValueOnly) {
    LeafStore s; seed(s);
    const std::string query = "/components/component";
    auto prev = s.snapshot(query);

    // Same value, new timestamp → must NOT count as a change.
    s.set("/components/component[name=PSC-0]/state/temperature/instant", 45.0, 99);
    // Different value → must count as a change.
    s.set("/components/component[name=PSC-1]/state/temperature/instant", 50.0, 99);

    auto cur  = s.snapshot(query);
    auto diff = LeafStore::diff(prev, cur);

    ASSERT_EQ(diff.updated.size(), 1u);
    EXPECT_EQ(diff.updated[0].first,
              "/components/component[name=PSC-1]/state/temperature/instant");
    EXPECT_TRUE(diff.removed.empty());
}

TEST(LeafStore, DiffReportsAddedAndRemoved) {
    LeafStore s; seed(s);
    const std::string query = "/components/component";
    auto prev = s.snapshot(query);

    s.set("/components/component[name=PSC-0]/alarm", true, 99);  // added
    s.remove("/components/component[name=PSC-1]/state/temperature/instant");  // removed

    auto cur  = s.snapshot(query);
    auto diff = LeafStore::diff(prev, cur);

    ASSERT_EQ(diff.updated.size(), 1u);
    EXPECT_EQ(diff.updated[0].first, "/components/component[name=PSC-0]/alarm");
    ASSERT_EQ(diff.removed.size(), 1u);
    EXPECT_EQ(diff.removed[0],
              "/components/component[name=PSC-1]/state/temperature/instant");
}
