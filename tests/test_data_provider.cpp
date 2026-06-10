#include <gtest/gtest.h>
#include "backend/data_provider.hpp"

// ---------------------------------------------------------------------------
// FakeProvider — minimal IDataProvider stub for registry tests
// ---------------------------------------------------------------------------

class FakeProvider : public IDataProvider {
public:
    explicit FakeProvider(gnmi::SubscriptionMode mode = gnmi::SAMPLE)
        : mode_(mode) {}

    void fill(RepeatedPtrField<Update>* list,
              const std::string& xpath) const override {
        addLeaf(list, xpath, 0.0);
    }

    gnmi::SubscriptionMode preferredMode(const std::string&) const override {
        return mode_;
    }

    // One leaf at the queried path, stamped with a fixed collection time so
    // timestamp wiring is observable in tests.
    Snapshot snapshot(const std::string& xpath) const override {
        gnmi::TypedValue v;
        v.set_double_val(0.0);
        Snapshot s;
        s.emplace(xpath, Leaf{v, 42});
        return s;
    }

private:
    gnmi::SubscriptionMode mode_;
};

// Routed but never produces a value — exercises the NOT_FOUND branch
// (registered prefix matches, but no leaf is available).
class EmptyProvider : public IDataProvider {
public:
    void fill(RepeatedPtrField<Update>*, const std::string&) const override {}
    Snapshot snapshot(const std::string&) const override { return {}; }
};

// ---------------------------------------------------------------------------
// DataProviderRegistry — routing and fan-out
// ---------------------------------------------------------------------------

TEST(DataProviderRegistry, fillRoutesToMatchingProvider) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());

    RepeatedPtrField<Update> list;
    FillResult res = reg.fill(&list, "/foo/bar");
    EXPECT_TRUE(res.routed);
    EXPECT_TRUE(res.produced);
    EXPECT_EQ(list.size(), 1);
}

TEST(DataProviderRegistry, fillSkipsNonMatchingProvider) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());

    RepeatedPtrField<Update> list;
    FillResult res = reg.fill(&list, "/bar/baz");
    EXPECT_FALSE(res.routed);
    EXPECT_FALSE(res.produced);
    EXPECT_EQ(list.size(), 0);
}

TEST(DataProviderRegistry, fillMatchesExactPrefix) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());

    RepeatedPtrField<Update> list;
    EXPECT_TRUE(reg.fill(&list, "/foo").produced);
    EXPECT_EQ(list.size(), 1);
}

TEST(DataProviderRegistry, fillDoesNotMatchPartialSegment) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());

    RepeatedPtrField<Update> list;
    EXPECT_FALSE(reg.fill(&list, "/foobar").routed);
    EXPECT_EQ(list.size(), 0);
}

// Registered prefix matches but the provider yields nothing → routed && !produced.
// This is the NOT_FOUND (Get) / silent (Subscribe) case, distinct from UNIMPLEMENTED.
TEST(DataProviderRegistry, fillRoutedButNotProduced) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<EmptyProvider>());

    RepeatedPtrField<Update> list;
    FillResult res = reg.fill(&list, "/foo/bar");
    EXPECT_TRUE(res.routed);
    EXPECT_FALSE(res.produced);
    EXPECT_EQ(list.size(), 0);
}

TEST(DataProviderRegistry, fillFansOutToAllMatchingProviders) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());
    reg.addProvider("/foo", std::make_unique<FakeProvider>());

    RepeatedPtrField<Update> list;
    EXPECT_TRUE(reg.fill(&list, "/foo/bar").produced);
    EXPECT_EQ(list.size(), 2);
}

TEST(DataProviderRegistry, fillReturnsTrueWhenAnyProviderMatches) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());
    reg.addProvider("/bar", std::make_unique<FakeProvider>());

    RepeatedPtrField<Update> list;
    EXPECT_TRUE(reg.fill(&list, "/foo/x").produced);
    EXPECT_EQ(list.size(), 1);
}

TEST(DataProviderRegistry, fillReturnsFalseWhenNoProviderRegistered) {
    DataProviderRegistry reg;

    RepeatedPtrField<Update> list;
    FillResult res = reg.fill(&list, "/anything");
    EXPECT_FALSE(res.routed);
    EXPECT_FALSE(res.produced);
    EXPECT_EQ(list.size(), 0);
}

TEST(DataProviderRegistry, fillStripsQuotesForMatching) {
    DataProviderRegistry reg;
    reg.addProvider("/components/component", std::make_unique<FakeProvider>());

    RepeatedPtrField<Update> list;
    // gnmi_to_xpath produces quoted keys; registry must still match
    EXPECT_TRUE(reg.fill(&list, "/components/component[name=\"PSC-0\"]/state").produced);
    EXPECT_EQ(list.size(), 1);
}

// ---------------------------------------------------------------------------
// DataProviderRegistry::snapshot — fan-out read model for Subscribe
// ---------------------------------------------------------------------------

TEST(DataProviderRegistry, snapshotRoutesAndCarriesCollectionTime) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());

    SnapResult res = reg.snapshot("/foo/bar");
    EXPECT_TRUE(res.routed);
    ASSERT_EQ(res.snap.size(), 1u);
    const Leaf& leaf = res.snap.at("/foo/bar");
    EXPECT_DOUBLE_EQ(leaf.val.double_val(), 0.0);
    EXPECT_EQ(leaf.collectedNs, 42);          // feeds Notification.timestamp
}

TEST(DataProviderRegistry, snapshotNotRoutedWhenNoPrefixMatches) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());

    SnapResult res = reg.snapshot("/bar/baz");
    EXPECT_FALSE(res.routed);
    EXPECT_TRUE(res.snap.empty());
}

// Routed but empty → owned namespace, no data yet (Subscribe stays silent).
TEST(DataProviderRegistry, snapshotRoutedButEmpty) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<EmptyProvider>());

    SnapResult res = reg.snapshot("/foo/bar");
    EXPECT_TRUE(res.routed);
    EXPECT_TRUE(res.snap.empty());
}

TEST(DataProviderRegistry, preferredModeReturnsFirstMatch) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>(gnmi::ON_CHANGE));

    EXPECT_EQ(reg.preferredMode("/foo/bar"), gnmi::ON_CHANGE);
}

TEST(DataProviderRegistry, preferredModeDefaultsSampleWhenNoMatch) {
    DataProviderRegistry reg;

    EXPECT_EQ(reg.preferredMode("/nonexistent"), gnmi::SAMPLE);
}

// ---------------------------------------------------------------------------
// addLeaf — TypedValue dispatch per overload
// ---------------------------------------------------------------------------

TEST(AddLeaf, DoubleSetsDoubleVal) {
    RepeatedPtrField<Update> list;
    addLeaf(&list, "/foo", 3.14);

    ASSERT_EQ(list.size(), 1);
    EXPECT_DOUBLE_EQ(list[0].val().double_val(), 3.14);
}

TEST(AddLeaf, StringSetsStringVal) {
    RepeatedPtrField<Update> list;
    addLeaf(&list, "/foo", std::string("hello"));

    ASSERT_EQ(list.size(), 1);
    EXPECT_EQ(list[0].val().string_val(), "hello");
}

TEST(AddLeaf, BoolSetsBoolVal) {
    RepeatedPtrField<Update> list;
    addLeaf(&list, "/foo", true);

    ASSERT_EQ(list.size(), 1);
    EXPECT_TRUE(list[0].val().bool_val());
}

TEST(AddLeaf, Int64SetsIntVal) {
    RepeatedPtrField<Update> list;
    addLeaf(&list, "/foo", int64_t{-42});

    ASSERT_EQ(list.size(), 1);
    EXPECT_EQ(list[0].val().int_val(), -42);
}

TEST(AddLeaf, Uint64SetsUintVal) {
    RepeatedPtrField<Update> list;
    addLeaf(&list, "/foo", uint64_t{100});

    ASSERT_EQ(list.size(), 1);
    EXPECT_EQ(list[0].val().uint_val(), 100u);
}
