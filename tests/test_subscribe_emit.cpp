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
