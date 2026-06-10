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

// Accepts writes and records the last one, so the registry's write fan-out is
// observable. FakeProvider/EmptyProvider keep the read-only defaults, serving as
// the refusal case (routed && !applied).
class WritableProvider : public IDataProvider {
public:
    void fill(RepeatedPtrField<Update>*, const std::string&) const override {}
    Snapshot snapshot(const std::string&) const override { return {}; }

    bool writable(const std::string&) const override { return true; }
    bool applyUpdate(const std::string& xpath, const gnmi::TypedValue&,
                     int64_t ts) override {
        ++updates; lastUpdate = xpath; lastTs = ts; return true;
    }
    bool applyDelete(const std::string& xpath) override {
        ++deletes; lastDelete = xpath; return true;
    }

    int         updates = 0, deletes = 0;
    std::string lastUpdate, lastDelete;
    int64_t     lastTs = 0;
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

// ---------------------------------------------------------------------------
// DataProviderRegistry — write fan-out (writable / set / del)
//
// Two signals, like fill(): routed = a prefix owns the path (else NOT_FOUND);
// applied = a provider accepted the write (routed && !applied = read-only →
// INVALID_ARGUMENT). writable() is Set's dry-run validate phase.
// ---------------------------------------------------------------------------

TEST(DataProviderRegistryWrite, writableReportsWritableProvider) {
    DataProviderRegistry reg;
    reg.addProvider("/cfg", std::make_unique<WritableProvider>());

    WriteResult wr = reg.writable("/cfg/x");
    EXPECT_TRUE(wr.routed);
    EXPECT_TRUE(wr.applied);
}

TEST(DataProviderRegistryWrite, writableReportsReadOnlyProvider) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());  // read-only default

    WriteResult wr = reg.writable("/foo/x");
    EXPECT_TRUE(wr.routed);
    EXPECT_FALSE(wr.applied);   // routed but not writable → INVALID_ARGUMENT
}

TEST(DataProviderRegistryWrite, writableNotRoutedWhenNoPrefixMatches) {
    DataProviderRegistry reg;
    reg.addProvider("/cfg", std::make_unique<WritableProvider>());

    WriteResult wr = reg.writable("/other");
    EXPECT_FALSE(wr.routed);    // no owner → NOT_FOUND
    EXPECT_FALSE(wr.applied);
}

TEST(DataProviderRegistryWrite, setAppliesToWritableProvider) {
    DataProviderRegistry reg;
    auto p = std::make_unique<WritableProvider>();
    WritableProvider* raw = p.get();
    reg.addProvider("/cfg", std::move(p));

    gnmi::TypedValue v;
    v.set_string_val("psc-mock");
    WriteResult wr = reg.set("/cfg/hostname", v, 99);

    EXPECT_TRUE(wr.routed);
    EXPECT_TRUE(wr.applied);
    EXPECT_EQ(raw->updates, 1);
    EXPECT_EQ(raw->lastUpdate, "/cfg/hostname");
    EXPECT_EQ(raw->lastTs, 99);             // transaction timestamp is forwarded
}

TEST(DataProviderRegistryWrite, setRefusedByReadOnlyProvider) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());

    gnmi::TypedValue v;
    v.set_double_val(1.0);
    WriteResult wr = reg.set("/foo/x", v, 0);

    EXPECT_TRUE(wr.routed);
    EXPECT_FALSE(wr.applied);   // default applyUpdate refuses
}

TEST(DataProviderRegistryWrite, delAppliesToWritableProvider) {
    DataProviderRegistry reg;
    auto p = std::make_unique<WritableProvider>();
    WritableProvider* raw = p.get();
    reg.addProvider("/cfg", std::move(p));

    WriteResult wr = reg.del("/cfg/hostname");

    EXPECT_TRUE(wr.routed);
    EXPECT_TRUE(wr.applied);
    EXPECT_EQ(raw->deletes, 1);
    EXPECT_EQ(raw->lastDelete, "/cfg/hostname");
}

TEST(DataProviderRegistryWrite, delNotRoutedWhenNoPrefixMatches) {
    DataProviderRegistry reg;
    reg.addProvider("/cfg", std::make_unique<WritableProvider>());

    WriteResult wr = reg.del("/other");
    EXPECT_FALSE(wr.routed);
    EXPECT_FALSE(wr.applied);
}
