#include <gtest/gtest.h>

#include "gnmi/subscribe_emit.h"
#include "backend/data_provider.hpp"
#include "backend/leaf_store.hpp"
#include <utils/utils.h>

using namespace std::chrono;
using google::protobuf::RepeatedPtrField;

namespace {

gnmi::TypedValue dbl(double v) {
    gnmi::TypedValue t;
    t.set_double_val(v);
    return t;
}

Leaf leaf(double v, int64_t ns) { return Leaf{dbl(v), ns}; }

// Round-trip a gNMI Path back to xpath so assertions read in path terms.
std::string pathStr(const gnmi::Path& p) { return gnmi_to_xpath(p); }

// Minimal provider whose only job is to answer preferredMode().
class ModeProvider : public IDataProvider {
public:
    explicit ModeProvider(gnmi::SubscriptionMode m) : mode_(m) {}
    void fill(RepeatedPtrField<gnmi::Update>*, const std::string&) const override {}
    gnmi::SubscriptionMode preferredMode(const std::string&) const override {
        return mode_;
    }
    Snapshot snapshot(const std::string&) const override { return {}; }
private:
    gnmi::SubscriptionMode mode_;
};

gnmi::Subscription subWith(gnmi::SubscriptionMode mode, const std::string& xpath) {
    gnmi::Subscription s;
    s.set_mode(mode);
    xpath_to_gnmi_path(xpath, s.mutable_path());
    return s;
}

// Declares one atomic container: every leaf under `container` belongs to it.
class AtomicProvider : public IDataProvider {
public:
    explicit AtomicProvider(std::string container) : container_(std::move(container)) {}
    void fill(RepeatedPtrField<gnmi::Update>*, const std::string&) const override {}
    Snapshot snapshot(const std::string&) const override { return {}; }
    std::optional<std::string> atomicPrefix(const std::string& xpath) const override {
        if (xpath.compare(0, container_.size(), container_) == 0 &&
            (xpath.size() == container_.size() || xpath[container_.size()] == '/'))
            return container_;
        return std::nullopt;
    }
private:
    std::string container_;
};

// Registry routing "/c" to an atomic provider whose atomic container is "/c/cfg".
DataProviderRegistry atomicRegistry() {
    DataProviderRegistry reg;
    reg.addProvider("/c", std::make_unique<AtomicProvider>("/c/cfg"));
    return reg;
}

// Find the (single) atomic / non-atomic notification in a built list.
const gnmi::Notification* findAtomic(const std::vector<gnmi::Notification>& v, bool atomic) {
    for (const auto& n : v) if (n.atomic() == atomic) return &n;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// emitSnapshot
// ---------------------------------------------------------------------------

TEST(EmitSnapshot, EmitsEveryLeafAndReturnsNewestCollectionTime) {
    Snapshot snap;
    snap.emplace("/a", leaf(1.5, 100));
    snap.emplace("/b", leaf(2.5, 300));   // newest

    RepeatedPtrField<gnmi::Update> list;
    int64_t ts = impl::emitSnapshot(snap, &list);

    ASSERT_EQ(list.size(), 2);
    // Snapshot is ordered, so /a then /b.
    EXPECT_EQ(pathStr(list[0].path()), "/a");
    EXPECT_DOUBLE_EQ(list[0].val().double_val(), 1.5);
    EXPECT_EQ(pathStr(list[1].path()), "/b");
    EXPECT_EQ(ts, 300);                    // max collectedNs feeds the timestamp
}

TEST(EmitSnapshot, EmptySnapshotEmitsNothingAndReturnsZero) {
    Snapshot snap;
    RepeatedPtrField<gnmi::Update> list;
    EXPECT_EQ(impl::emitSnapshot(snap, &list), 0);
    EXPECT_EQ(list.size(), 0);
}

// ---------------------------------------------------------------------------
// emitDiff — updated → update, removed → delete (spec §3.5.2.3)
// ---------------------------------------------------------------------------

TEST(EmitDiff, UpdatedBecomeUpdatesRemovedBecomeDeletes) {
    LeafStore::Diff d;
    d.updated.emplace_back("/a", leaf(1.0, 100));
    d.updated.emplace_back("/b", leaf(2.0, 250));   // newest
    d.removed.push_back("/c");

    gnmi::Notification n;
    int64_t ts = impl::emitDiff(d, &n);

    ASSERT_EQ(n.update_size(), 2);
    EXPECT_EQ(pathStr(n.update(0).path()), "/a");
    EXPECT_EQ(pathStr(n.update(1).path()), "/b");
    ASSERT_EQ(n.delete__size(), 1);
    EXPECT_EQ(pathStr(n.delete_(0)), "/c");
    EXPECT_EQ(ts, 250);                              // newest among updated only
}

TEST(EmitDiff, OnlyRemovedProducesDeletesAndZeroTimestamp) {
    LeafStore::Diff d;
    d.removed.push_back("/gone");

    gnmi::Notification n;
    int64_t ts = impl::emitDiff(d, &n);

    EXPECT_EQ(n.update_size(), 0);
    ASSERT_EQ(n.delete__size(), 1);
    EXPECT_EQ(pathStr(n.delete_(0)), "/gone");
    EXPECT_EQ(ts, 0);   // caller falls back to "now" when nothing was collected
}

TEST(EmitDiff, EmptyDiffEmitsNothing) {
    LeafStore::Diff d;
    gnmi::Notification n;
    EXPECT_EQ(impl::emitDiff(d, &n), 0);
    EXPECT_EQ(n.update_size(), 0);
    EXPECT_EQ(n.delete__size(), 0);
}

// ---------------------------------------------------------------------------
// heartbeatDue
// ---------------------------------------------------------------------------

TEST(HeartbeatDue, ZeroIntervalNeverFires) {
    auto t0 = high_resolution_clock::now();
    EXPECT_FALSE(impl::heartbeatDue(0, t0, t0 + hours(1)));
}

TEST(HeartbeatDue, FiresOnlyAfterIntervalElapsed) {
    auto t0 = high_resolution_clock::now();
    const uint64_t interval = duration_cast<nanoseconds>(milliseconds(500)).count();

    EXPECT_FALSE(impl::heartbeatDue(interval, t0, t0 + milliseconds(499)));
    EXPECT_TRUE (impl::heartbeatDue(interval, t0, t0 + milliseconds(500)));
    EXPECT_TRUE (impl::heartbeatDue(interval, t0, t0 + milliseconds(900)));
}

// ---------------------------------------------------------------------------
// resolveStreamMode — TARGET_DEFINED expands per provider; explicit passes through
// ---------------------------------------------------------------------------

TEST(ResolveStreamMode, ExplicitModesPassThrough) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<ModeProvider>(gnmi::ON_CHANGE));

    EXPECT_EQ(impl::resolveStreamMode(subWith(gnmi::SAMPLE, "/foo/x"), reg),
              gnmi::SAMPLE);
    EXPECT_EQ(impl::resolveStreamMode(subWith(gnmi::ON_CHANGE, "/foo/x"), reg),
              gnmi::ON_CHANGE);
}

TEST(ResolveStreamMode, TargetDefinedUsesProviderPreference) {
    DataProviderRegistry reg;
    reg.addProvider("/sample", std::make_unique<ModeProvider>(gnmi::SAMPLE));
    reg.addProvider("/onchange", std::make_unique<ModeProvider>(gnmi::ON_CHANGE));

    EXPECT_EQ(impl::resolveStreamMode(subWith(gnmi::TARGET_DEFINED, "/sample/x"), reg),
              gnmi::SAMPLE);
    EXPECT_EQ(impl::resolveStreamMode(subWith(gnmi::TARGET_DEFINED, "/onchange/x"), reg),
              gnmi::ON_CHANGE);
}

TEST(ResolveStreamMode, TargetDefinedDefaultsSampleWhenNoProviderMatches) {
    DataProviderRegistry reg;
    EXPECT_EQ(impl::resolveStreamMode(subWith(gnmi::TARGET_DEFINED, "/nope"), reg),
              gnmi::SAMPLE);
}

// ---------------------------------------------------------------------------
// emitAtomic — prefix set, paths relativised, atomic=true (spec §2.1.1)
// ---------------------------------------------------------------------------

TEST(EmitAtomic, SetsPrefixRelativisesPathsAndFlagsAtomic) {
    Snapshot snap;
    snap.emplace("/c/cfg/x", leaf(1.0, 100));
    snap.emplace("/c/cfg/y", leaf(2.0, 400));   // newest

    gnmi::Notification n;
    int64_t ts = impl::emitAtomic(snap, "/c/cfg", &n);

    EXPECT_TRUE(n.atomic());
    EXPECT_EQ(pathStr(n.prefix()), "/c/cfg");
    ASSERT_EQ(n.update_size(), 2);
    EXPECT_EQ(pathStr(n.update(0).path()), "/x");   // relative: prefix ++ path == leaf
    EXPECT_EQ(pathStr(n.update(1).path()), "/y");
    EXPECT_EQ(ts, 400);
}

// ---------------------------------------------------------------------------
// buildFullNotifications — atomic container split off from non-atomic remainder
// ---------------------------------------------------------------------------

TEST(BuildFullNotifications, SplitsAtomicContainerFromNonAtomicLeaves) {
    DataProviderRegistry reg = atomicRegistry();
    Snapshot snap;
    snap.emplace("/c/cfg/x", leaf(1.0, 100));   // atomic container
    snap.emplace("/c/cfg/y", leaf(2.0, 100));
    snap.emplace("/c/plain", leaf(9.0, 100));   // non-atomic

    auto notes = impl::buildFullNotifications(snap, reg);
    ASSERT_EQ(notes.size(), 2u);

    const gnmi::Notification* na = findAtomic(notes, false);
    const gnmi::Notification* at = findAtomic(notes, true);
    ASSERT_NE(na, nullptr);
    ASSERT_NE(at, nullptr);

    // Non-atomic carries the plain leaf with its full path, no prefix.
    ASSERT_EQ(na->update_size(), 1);
    EXPECT_EQ(pathStr(na->update(0).path()), "/c/plain");

    // Atomic carries the whole container, relativised, under its prefix.
    EXPECT_EQ(pathStr(at->prefix()), "/c/cfg");
    ASSERT_EQ(at->update_size(), 2);
    EXPECT_EQ(pathStr(at->update(0).path()), "/x");
    EXPECT_EQ(pathStr(at->update(1).path()), "/y");
}

TEST(BuildFullNotifications, PureAtomicQueryYieldsOnlyAtomicNotification) {
    DataProviderRegistry reg = atomicRegistry();
    Snapshot snap;
    snap.emplace("/c/cfg/x", leaf(1.0, 100));

    auto notes = impl::buildFullNotifications(snap, reg);
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_TRUE(notes[0].atomic());
}

// ---------------------------------------------------------------------------
// buildChangeNotifications — atomic = full re-send on any change (spec §2.1.1)
// ---------------------------------------------------------------------------

TEST(BuildChangeNotifications, AtomicContainerResendsCompleteStateOnChange) {
    DataProviderRegistry reg = atomicRegistry();
    Snapshot prev, cur;
    prev.emplace("/c/cfg/x", leaf(1.0, 100));
    prev.emplace("/c/cfg/y", leaf(2.0, 100));
    cur.emplace("/c/cfg/x", leaf(1.0, 100));    // unchanged
    cur.emplace("/c/cfg/y", leaf(3.0, 500));    // changed

    auto notes = impl::buildChangeNotifications(prev, cur, reg);
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_TRUE(notes[0].atomic());
    // Whole record re-sent, not just the changed leaf.
    ASSERT_EQ(notes[0].update_size(), 2);
    EXPECT_EQ(pathStr(notes[0].prefix()), "/c/cfg");
}

TEST(BuildChangeNotifications, OmittedLeafIsImplicitlyDeletedViaFullResend) {
    DataProviderRegistry reg = atomicRegistry();
    Snapshot prev, cur;
    prev.emplace("/c/cfg/x", leaf(1.0, 100));
    prev.emplace("/c/cfg/y", leaf(2.0, 100));
    cur.emplace("/c/cfg/x", leaf(1.0, 100));    // y removed

    auto notes = impl::buildChangeNotifications(prev, cur, reg);
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_TRUE(notes[0].atomic());
    // No explicit delete: the absent leaf is implicitly deleted by re-sending
    // the (now smaller) complete state.
    ASSERT_EQ(notes[0].update_size(), 1);
    EXPECT_EQ(pathStr(notes[0].update(0).path()), "/x");
    EXPECT_EQ(notes[0].delete__size(), 0);
}

TEST(BuildChangeNotifications, EmptiedContainerEmitsPrefixDelete) {
    DataProviderRegistry reg = atomicRegistry();
    Snapshot prev, cur;
    prev.emplace("/c/cfg/x", leaf(1.0, 100));   // whole record gone in cur

    auto notes = impl::buildChangeNotifications(prev, cur, reg);
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_TRUE(notes[0].atomic());
    EXPECT_EQ(pathStr(notes[0].prefix()), "/c/cfg");
    EXPECT_GE(notes[0].delete__size(), 1);
    EXPECT_EQ(notes[0].update_size(), 0);
}

TEST(BuildChangeNotifications, UnchangedContainerEmitsNothing) {
    DataProviderRegistry reg = atomicRegistry();
    Snapshot prev, cur;
    prev.emplace("/c/cfg/x", leaf(1.0, 100));
    cur.emplace("/c/cfg/x", leaf(1.0, 999));    // same value, newer ts only

    auto notes = impl::buildChangeNotifications(prev, cur, reg);
    EXPECT_TRUE(notes.empty());                 // value unchanged → no re-send
}

TEST(BuildChangeNotifications, NonAtomicLeavesStillDiff) {
    DataProviderRegistry reg = atomicRegistry();
    Snapshot prev, cur;
    prev.emplace("/c/plain", leaf(1.0, 100));
    cur.emplace("/c/plain", leaf(2.0, 200));    // changed, non-atomic

    auto notes = impl::buildChangeNotifications(prev, cur, reg);
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_FALSE(notes[0].atomic());
    ASSERT_EQ(notes[0].update_size(), 1);
    EXPECT_EQ(pathStr(notes[0].update(0).path()), "/c/plain");
}
