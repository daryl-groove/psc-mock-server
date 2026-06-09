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
              const std::string& xpath) override {
        addLeaf(list, xpath, 0.0);
    }

    gnmi::SubscriptionMode preferredMode(const std::string&) const override {
        return mode_;
    }

private:
    gnmi::SubscriptionMode mode_;
};

// ---------------------------------------------------------------------------
// DataProviderRegistry — routing and fan-out
// ---------------------------------------------------------------------------

TEST(DataProviderRegistry, fillRoutesToMatchingProvider) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());

    RepeatedPtrField<Update> list;
    EXPECT_TRUE(reg.fill(&list, "/foo/bar"));
    EXPECT_EQ(list.size(), 1);
}

TEST(DataProviderRegistry, fillSkipsNonMatchingProvider) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());

    RepeatedPtrField<Update> list;
    EXPECT_FALSE(reg.fill(&list, "/bar/baz"));
    EXPECT_EQ(list.size(), 0);
}

TEST(DataProviderRegistry, fillMatchesExactPrefix) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());

    RepeatedPtrField<Update> list;
    EXPECT_TRUE(reg.fill(&list, "/foo"));
    EXPECT_EQ(list.size(), 1);
}

TEST(DataProviderRegistry, fillDoesNotMatchPartialSegment) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());

    RepeatedPtrField<Update> list;
    EXPECT_FALSE(reg.fill(&list, "/foobar"));
    EXPECT_EQ(list.size(), 0);
}

TEST(DataProviderRegistry, fillFansOutToAllMatchingProviders) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());
    reg.addProvider("/foo", std::make_unique<FakeProvider>());

    RepeatedPtrField<Update> list;
    EXPECT_TRUE(reg.fill(&list, "/foo/bar"));
    EXPECT_EQ(list.size(), 2);
}

TEST(DataProviderRegistry, fillReturnsTrueWhenAnyProviderMatches) {
    DataProviderRegistry reg;
    reg.addProvider("/foo", std::make_unique<FakeProvider>());
    reg.addProvider("/bar", std::make_unique<FakeProvider>());

    RepeatedPtrField<Update> list;
    EXPECT_TRUE(reg.fill(&list, "/foo/x"));
    EXPECT_EQ(list.size(), 1);
}

TEST(DataProviderRegistry, fillReturnsFalseWhenNoProviderRegistered) {
    DataProviderRegistry reg;

    RepeatedPtrField<Update> list;
    EXPECT_FALSE(reg.fill(&list, "/anything"));
    EXPECT_EQ(list.size(), 0);
}

TEST(DataProviderRegistry, fillStripsQuotesForMatching) {
    DataProviderRegistry reg;
    reg.addProvider("/components/component", std::make_unique<FakeProvider>());

    RepeatedPtrField<Update> list;
    // gnmi_to_xpath produces quoted keys; registry must still match
    EXPECT_TRUE(reg.fill(&list, "/components/component[name=\"PSC-0\"]/state"));
    EXPECT_EQ(list.size(), 1);
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
