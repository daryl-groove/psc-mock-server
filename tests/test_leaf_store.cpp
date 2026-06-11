#include <gtest/gtest.h>
#include "backend/leaf_store.hpp"

namespace {

// Commit helpers — most tests only need a leaf present to exercise a read path;
// commit() itself is exercised directly in the Commit* tests below. A single
// write is just a one-op batch.
void put(LeafStore& s, const std::string& path, double value, int64_t ts) {
    s.commit(WriteBatch{}.set(path, value, ts));
}
void del(LeafStore& s, const std::string& path) {
    s.commit(WriteBatch{}.remove(path));
}

// Seed a small two-unit sensor tree mirroring the PSC layout, in one commit.
// LeafStore is non-movable (holds a shared_mutex), so seed a reference.
void seed(LeafStore& s) {
    s.commit(WriteBatch{}
        .set("/components/component[name=PSC-0]/state/temperature/instant", 45.0, 10)
        .set("/components/component[name=PSC-0]/power-supply/state/input-voltage", 54.0, 10)
        .set("/components/component[name=PSC-0]/power-supply/state/output-power", 240.0, 10)
        .set("/components/component[name=PSC-1]/state/temperature/instant", 46.0, 10));
}

} // namespace

// ---------------------------------------------------------------------------
// commit / get
// ---------------------------------------------------------------------------

TEST(LeafStore, GetReturnsValueAndTimestamp) {
    LeafStore s;
    put(s, "/a/b", 3.5, 123);

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
    put(s, "/a", 1.0, 10);
    put(s, "/a", 2.0, 20);

    auto leaf = s.get("/a");
    ASSERT_TRUE(leaf.has_value());
    EXPECT_DOUBLE_EQ(leaf->val.double_val(), 2.0);
    EXPECT_EQ(leaf->collectedNs, 20);
}

TEST(LeafStore, QuotedAndUnquotedKeysAreEquivalent) {
    LeafStore s;
    put(s, "/components/component[name=PSC-0]/x", 7.0, 1);
    EXPECT_TRUE(s.get("/components/component[name=\"PSC-0\"]/x").has_value());
}

TEST(LeafStore, RemoveDeletesLeaf) {
    LeafStore s;
    put(s, "/a", 1.0, 1);
    del(s, "/a");
    EXPECT_FALSE(s.get("/a").has_value());
}

// ---------------------------------------------------------------------------
// commit — one transaction, applied together
// ---------------------------------------------------------------------------

TEST(LeafStore, CommitAppliesEveryOp) {
    LeafStore s;
    s.commit(WriteBatch{}
        .set("/a", 1.0, 5)
        .set("/b", std::string("hi"), 5)
        .set("/c", true, 5));

    EXPECT_DOUBLE_EQ(s.get("/a")->val.double_val(), 1.0);
    EXPECT_EQ(s.get("/b")->val.string_val(), "hi");
    EXPECT_TRUE(s.get("/c")->val.bool_val());
}

// Ops apply in batch order, so a delete then a set of the same leaf in one
// transaction ends as the set (mirrors the gNMI Set delete -> update order).
TEST(LeafStore, CommitAppliesOpsInOrder) {
    LeafStore s;
    put(s, "/a", 1.0, 1);
    s.commit(WriteBatch{}.remove("/a").set("/a", 9.0, 2));

    auto leaf = s.get("/a");
    ASSERT_TRUE(leaf.has_value());
    EXPECT_DOUBLE_EQ(leaf->val.double_val(), 9.0);
    EXPECT_EQ(leaf->collectedNs, 2);
}

TEST(LeafStore, CommitMixesSetAndRemove) {
    LeafStore s;
    put(s, "/keep", 1.0, 1);
    put(s, "/drop", 2.0, 1);
    s.commit(WriteBatch{}.set("/add", 3.0, 2).remove("/drop"));

    EXPECT_TRUE(s.get("/keep").has_value());
    EXPECT_TRUE(s.get("/add").has_value());
    EXPECT_FALSE(s.get("/drop").has_value());
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
    put(s, "/a/power-supply/x", 1.0, 1);
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
    put(s, "/components/component[name=PSC-0]/state/temperature/instant", 45.0, 99);
    // Different value → must count as a change.
    put(s, "/components/component[name=PSC-1]/state/temperature/instant", 50.0, 99);

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

    s.commit(WriteBatch{}
        .set("/components/component[name=PSC-0]/alarm", true, 99)              // added
        .remove("/components/component[name=PSC-1]/state/temperature/instant")); // removed

    auto cur  = s.snapshot(query);
    auto diff = LeafStore::diff(prev, cur);

    ASSERT_EQ(diff.updated.size(), 1u);
    EXPECT_EQ(diff.updated[0].first, "/components/component[name=PSC-0]/alarm");
    ASSERT_EQ(diff.removed.size(), 1u);
    EXPECT_EQ(diff.removed[0],
              "/components/component[name=PSC-1]/state/temperature/instant");
}
