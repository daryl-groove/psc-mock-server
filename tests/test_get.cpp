#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>

#include "gnmi/get.h"
#include "backend/data_provider.hpp"
#include <utils/utils.h>

using impl::Get;

// ---------------------------------------------------------------------------
// GetRequest.type (CONFIG/STATE/OPERATIONAL) filtering — spec §3.3.4.
//
// The spec treats the three types as a disjoint partition annotated per leaf, so
// a single Get must return ONLY the type requested and never the same leaf under
// two types. MixedProvider produces one leaf of each type, stamping LeafType on
// each — exactly what a real provider does on the way out of snapshot().
// ---------------------------------------------------------------------------

namespace {

class MixedProvider : public IDataProvider {
public:
    void fill(RepeatedPtrField<Update>*, const std::string&) const override {}

    Snapshot snapshot(const std::string&) const override {
        gnmi::TypedValue v; v.set_string_val("x");
        Snapshot s;
        s.emplace("/data/config/hostname",          Leaf{v, 1, LeafType::Config});
        s.emplace("/data/state/mtu",                Leaf{v, 1, LeafType::State});
        s.emplace("/data/state/counters/in-octets", Leaf{v, 1, LeafType::Operational});
        return s;
    }
};

// Operational-only provider (inherits the default Operational class): a CONFIG or
// STATE request against it must filter to empty.
class SensorLikeProvider : public IDataProvider {
public:
    void fill(RepeatedPtrField<Update>*, const std::string&) const override {}
    Snapshot snapshot(const std::string&) const override {
        gnmi::TypedValue v; v.set_double_val(1.5);
        Snapshot s;
        s.emplace("/ro/state/value", Leaf{v, 1});
        return s;
    }
};

DataProviderRegistry makeRegistry() {
    DataProviderRegistry reg;
    reg.addProvider("/data", std::make_unique<MixedProvider>());
    reg.addProvider("/ro",   std::make_unique<SensorLikeProvider>());
    return reg;
}

GetRequest makeRequest(const std::string& xpath, GetRequest::DataType type) {
    GetRequest req;
    req.set_type(type);
    req.set_encoding(gnmi::JSON_IETF);
    xpath_to_gnmi_path(xpath, req.add_path());
    return req;
}

std::set<std::string> leafPaths(const GetResponse& resp) {
    std::set<std::string> paths;
    for (const auto& n : resp.notification())
        for (const auto& u : n.update())
            paths.insert(gnmi_to_xpath(u.path()));
    return paths;
}

std::set<std::string> getPaths(const std::string& xpath, GetRequest::DataType type) {
    DataProviderRegistry reg = makeRegistry();
    Get get(reg);
    GetResponse resp;
    GetRequest req = makeRequest(xpath, type);
    EXPECT_TRUE(get.run(&req, &resp).ok());
    return leafPaths(resp);
}

} // namespace

TEST(GetDataType, AllReturnsEveryClass) {
    auto paths = getPaths("/data", GetRequest::ALL);
    EXPECT_TRUE(paths.count("/data/config/hostname"));
    EXPECT_TRUE(paths.count("/data/state/mtu"));
    EXPECT_TRUE(paths.count("/data/state/counters/in-octets"));
}

TEST(GetDataType, ConfigKeepsOnlyConfig) {
    auto paths = getPaths("/data", GetRequest::CONFIG);
    EXPECT_EQ(paths, (std::set<std::string>{"/data/config/hostname"}));
}

// STATE is the applied-config leaf ONLY — the operational counter must NOT appear.
TEST(GetDataType, StateKeepsOnlyStateNotOperational) {
    auto paths = getPaths("/data", GetRequest::STATE);
    EXPECT_EQ(paths, (std::set<std::string>{"/data/state/mtu"}));
}

// OPERATIONAL is the counter ONLY — the applied-config state leaf must NOT appear.
// Together with the test above this proves STATE and OPERATIONAL are disjoint.
TEST(GetDataType, OperationalKeepsOnlyOperationalNotState) {
    auto paths = getPaths("/data", GetRequest::OPERATIONAL);
    EXPECT_EQ(paths, (std::set<std::string>{"/data/state/counters/in-octets"}));
}

// A CONFIG request against an operational-only path filters to empty — the path
// exists but carries nothing of the requested type (§3.3.4).
TEST(GetDataType, ConfigOnOperationalOnlyPathIsNotFound) {
    DataProviderRegistry reg = makeRegistry();
    Get get(reg);
    GetResponse resp;
    GetRequest req = makeRequest("/ro", GetRequest::CONFIG);
    EXPECT_EQ(get.run(&req, &resp).error_code(), StatusCode::NOT_FOUND);
}
