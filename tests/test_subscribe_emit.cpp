// Unit tests for the Subscribe emit logic (src/gnmi/subscribe_emit.cpp), the
// atomic-partition + changeSeq-diff + target-echo behaviour that drives both Get
// and Subscribe. The functions' contract is "Backend::View -> Notification(s)", so
// the tests build Views directly (full control of changeSeq/collectedNs/atomic) and
// exercise the branches the python e2e cannot reach deterministically: suppress-
// unchanged (changeSeq equal), per-leaf delete, atomic whole-record re-send,
// timestamp-collapse, and prefix.target echo. (Backlog #2; rebuilds the old
// test_subscribe_emit.cpp against the post-core-integration types.)

#include <gtest/gtest.h>

#include <memory>
#include <optional>

#include "gnmi/subscribe_emit.h"
#include <utils/utils.h>

using namespace impl;
using gnmid::Backend;
using gnmid::core::GroupView;
using gnmid::core::LeafSnapshot;
using gnmid::core::LeafType;
using gnmid::core::LeafValueSnapshot;

namespace {

gnmi::TypedValue dbl(double v) {
    gnmi::TypedValue t;
    t.set_double_val(v);
    return t;
}

// One snapshot leaf. changeSeq drives change-detection (D14); collectedNs drives
// the Notification timestamp (timestamp-collapse = max over members).
LeafValueSnapshot leaf(double v, int64_t collectedNs, uint64_t changeSeq,
                       LeafType type = LeafType::Operational) {
    return { std::make_shared<gnmi::TypedValue>(dbl(v)), collectedNs, changeSeq, type };
}

GroupView atomicGroup(const std::string& prefix, std::vector<std::string> members) {
    return { /*name*/ prefix, prefix, /*atomic*/ true, std::nullopt, std::move(members) };
}

// Relative leaf paths of a notification's updates, as xpath strings (sorted).
std::vector<std::string> updatePaths(const gnmi::Notification& n) {
    std::vector<std::string> out;
    for (const auto& u : n.update()) out.push_back(gnmi_to_xpath(u.path()));
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// buildFullNotifications — initial sync / ONCE / POLL / heartbeat
// ---------------------------------------------------------------------------

TEST(BuildFull, NonAtomicOneNotificationFullPathsMaxTimestamp) {
    Backend::View v;
    v.routed = true;
    v.leaves["/a/x"] = leaf(1.0, 100, 1);
    v.leaves["/a/y"] = leaf(2.0, 250, 2);

    auto notes = buildFullNotifications(v);

    ASSERT_EQ(notes.size(), 1u);
    EXPECT_FALSE(notes[0].atomic());
    EXPECT_EQ(notes[0].update_size(), 2);
    EXPECT_EQ(notes[0].timestamp(), 250);                 // max(collectedNs)
    EXPECT_EQ(updatePaths(notes[0]),
              (std::vector<std::string>{"/a/x", "/a/y"}));  // full paths
}

TEST(BuildFull, AtomicGroupOwnPrefixRelativePathsCollapsedTimestamp) {
    Backend::View v;
    v.routed = true;
    v.leaves["/sys/ntp/address"] = leaf(10.0, 100, 1);
    v.leaves["/sys/ntp/port"]    = leaf(123.0, 300, 2);
    v.groups.push_back(atomicGroup("/sys/ntp",
                                   {"/sys/ntp/address", "/sys/ntp/port"}));

    auto notes = buildFullNotifications(v);

    ASSERT_EQ(notes.size(), 1u);
    EXPECT_TRUE(notes[0].atomic());
    EXPECT_EQ(gnmi_to_xpath(notes[0].prefix()), "/sys/ntp");
    EXPECT_EQ(notes[0].update_size(), 2);
    EXPECT_EQ(notes[0].timestamp(), 300);                 // max over members
    EXPECT_EQ(updatePaths(notes[0]),
              (std::vector<std::string>{"/address", "/port"}));  // relative to prefix
}

TEST(BuildFull, MixedSplitsIntoNonAtomicPlusAtomic) {
    Backend::View v;
    v.routed = true;
    v.leaves["/a/x"]             = leaf(1.0, 100, 1);     // ungrouped
    v.leaves["/sys/ntp/address"] = leaf(10.0, 100, 2);    // atomic member
    v.groups.push_back(atomicGroup("/sys/ntp", {"/sys/ntp/address"}));

    auto notes = buildFullNotifications(v);

    ASSERT_EQ(notes.size(), 2u);
    int atomic = 0, nonAtomic = 0;
    for (const auto& n : notes) (n.atomic() ? atomic : nonAtomic)++;
    EXPECT_EQ(atomic, 1);
    EXPECT_EQ(nonAtomic, 1);
}

TEST(BuildFull, EmptyViewStillYieldsOneNotification) {
    Backend::View v;
    v.routed = true;                                      // no leaves, no groups

    auto notes = buildFullNotifications(v);

    ASSERT_EQ(notes.size(), 1u);                          // ONCE/POLL need one before sync
    EXPECT_FALSE(notes[0].atomic());
    EXPECT_EQ(notes[0].update_size(), 0);
    EXPECT_GT(notes[0].timestamp(), 0);                   // falls back to now()
}

// ---------------------------------------------------------------------------
// diffLeaves — change detection is by changeSeq (D14), not value
// ---------------------------------------------------------------------------

TEST(DiffLeaves, UpdatedRemovedAndSeqEqualSuppressed) {
    LeafSnapshot prev, cur;
    prev["/a/x"] = leaf(1.0, 100, 1);
    prev["/a/y"] = leaf(2.0, 100, 2);
    prev["/a/z"] = leaf(3.0, 100, 3);
    cur["/a/x"]  = leaf(99.0, 200, 1);   // value changed but SAME changeSeq -> not changed
    cur["/a/y"]  = leaf(2.0, 200, 5);    // changeSeq differs -> updated
    cur["/a/w"]  = leaf(4.0, 200, 7);    // new -> updated
    // /a/z dropped -> removed

    LeafDiff d = diffLeaves(prev, cur);

    EXPECT_EQ(d.updated.size(), 2u);     // /a/y, /a/w  (NOT /a/x — seq equal)
    ASSERT_EQ(d.removed.size(), 1u);
    EXPECT_EQ(d.removed[0], "/a/z");
}

// ---------------------------------------------------------------------------
// buildChangeNotifications — ON_CHANGE steady-state
// ---------------------------------------------------------------------------

TEST(BuildChange, NonAtomicUpdatePlusDelete) {
    Backend::View prev, cur;
    prev.leaves["/a/x"] = leaf(1.0, 100, 1);
    prev.leaves["/a/y"] = leaf(2.0, 100, 2);
    cur.leaves["/a/x"]  = leaf(5.0, 300, 9);   // changed
    // /a/y removed

    auto notes = buildChangeNotifications(prev, cur);

    ASSERT_EQ(notes.size(), 1u);
    EXPECT_FALSE(notes[0].atomic());
    EXPECT_EQ(notes[0].update_size(), 1);                 // /a/x
    EXPECT_EQ(notes[0].delete__size(), 1);                // /a/y
    EXPECT_EQ(notes[0].timestamp(), 300);
}

TEST(BuildChange, SeqEqualEmitsNothing) {
    Backend::View prev, cur;
    prev.leaves["/a/x"] = leaf(1.0, 100, 5);
    cur.leaves["/a/x"]  = leaf(99.0, 300, 5);  // value changed, changeSeq SAME

    auto notes = buildChangeNotifications(prev, cur);

    EXPECT_TRUE(notes.empty());                           // suppressed (no seq change)
}

TEST(BuildChange, AtomicResendsWholeRecordOnAnyMemberChange) {
    const GroupView g = atomicGroup("/sys/ntp", {"/sys/ntp/a", "/sys/ntp/b"});
    Backend::View prev, cur;
    prev.groups.push_back(g);
    cur.groups.push_back(g);
    prev.leaves["/sys/ntp/a"] = leaf(1.0, 100, 1);
    prev.leaves["/sys/ntp/b"] = leaf(2.0, 100, 2);
    cur.leaves["/sys/ntp/a"]  = leaf(1.0, 300, 1);        // a unchanged (seq 1)
    cur.leaves["/sys/ntp/b"]  = leaf(9.0, 300, 8);        // b changed (seq 2->8)

    auto notes = buildChangeNotifications(prev, cur);

    ASSERT_EQ(notes.size(), 1u);
    EXPECT_TRUE(notes[0].atomic());
    EXPECT_EQ(gnmi_to_xpath(notes[0].prefix()), "/sys/ntp");
    EXPECT_EQ(notes[0].update_size(), 2);                 // WHOLE record, not just b
    EXPECT_EQ(notes[0].timestamp(), 300);
    EXPECT_EQ(updatePaths(notes[0]),
              (std::vector<std::string>{"/a", "/b"}));
}

TEST(BuildChange, AtomicGroupGoneDeletesContainer) {
    Backend::View prev, cur;
    prev.groups.push_back(atomicGroup("/sys/ntp", {"/sys/ntp/a"}));
    prev.leaves["/sys/ntp/a"] = leaf(1.0, 100, 1);
    // cur: the whole record vanished (no group, no leaves)

    auto notes = buildChangeNotifications(prev, cur);

    ASSERT_EQ(notes.size(), 1u);
    EXPECT_TRUE(notes[0].atomic());
    EXPECT_EQ(gnmi_to_xpath(notes[0].prefix()), "/sys/ntp");
    EXPECT_EQ(notes[0].update_size(), 0);
    EXPECT_EQ(notes[0].delete__size(), 1);                // delete the container
}

// ---------------------------------------------------------------------------
// echoTarget — C5 / §2.2.2.1 (stamps every notification, atomic included)
// ---------------------------------------------------------------------------

TEST(EchoTarget, StampsEveryNotificationWhenSet) {
    Backend::View v;
    v.leaves["/a/x"]          = leaf(1.0, 100, 1);
    v.leaves["/sys/ntp/a"]    = leaf(2.0, 100, 2);
    v.groups.push_back(atomicGroup("/sys/ntp", {"/sys/ntp/a"}));
    auto notes = buildFullNotifications(v);          // 1 non-atomic + 1 atomic
    ASSERT_EQ(notes.size(), 2u);

    echoTarget(notes, "router-7");

    for (const auto& n : notes) EXPECT_EQ(n.prefix().target(), "router-7");
}

TEST(EchoTarget, EmptyTargetIsNoOp) {
    Backend::View v;
    v.leaves["/a/x"] = leaf(1.0, 100, 1);
    auto notes = buildFullNotifications(v);

    echoTarget(notes, "");

    for (const auto& n : notes) EXPECT_EQ(n.prefix().target(), "");  // MUST NOT set
}
