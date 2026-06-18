/*
 * Backend unit tests — the device/schema layer over the core LeafRegistry,
 * exercised with stub providers (no gRPC, no real simulator). Covers routing,
 * the writability/schema plane (persists across delete), unset-leaf filtering,
 * atomic-group visibility, TARGET_DEFINED mode resolution, and the Set commit
 * path (update / delete / re-set).
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "backend/backend.hpp"
#include "backend/gnmi_value.hpp"
#include "backend/provider.hpp"
#include "leaf_sink.hpp"

using namespace gnmid;
using core::LeafType;

namespace {

const std::string NTP = "/system/ntp/servers/server[address=10.0.0.1]/config";

// /system config domain: flat scalars + a declared-but-unset leaf + an atomic
// NTP record (a group).
class SystemStub : public Provider {
public:
    explicit SystemStub(Backend& be) : Provider(be) {
        be_.declareGroup(NTP, /*atomic=*/true);
        be_.declareLeaf("/system/config/hostname", LeafType::Config, typedValue(std::string("psc-mock")));
        be_.declareLeaf("/system/config/timezone-name", LeafType::Config);          // declared, unset
        be_.declareLeaf(NTP + "/address", LeafType::Config, typedValue(std::string("10.0.0.1")));
        be_.declareLeaf(NTP + "/port", LeafType::Config, typedValue(uint64_t{123}));
    }
    std::string domainPrefix() const override { return "/system"; }
};

// /components sensor domain: one Operational leaf, no driver.
class SensorStub : public Provider {
public:
    explicit SensorStub(Backend& be) : Provider(be) {
        be_.declareLeaf("/components/component[name=PSC-0]/state/temperature/instant",
                        LeafType::Operational, typedValue(45.0));
    }
    std::string domainPrefix() const override { return "/components/component"; }
};

class BackendTest : public ::testing::Test {
protected:
    Backend be;
    void SetUp() override {
        be.addProvider(std::make_unique<SystemStub>(be));
        be.addProvider(std::make_unique<SensorStub>(be));
    }
};

TEST_F(BackendTest, RoutingDistinguishesOwnedNamespaces) {
    EXPECT_TRUE(be.routed("/system/config/hostname"));
    EXPECT_TRUE(be.routed("/components/component[name=PSC-0]/state/temperature/instant"));
    EXPECT_FALSE(be.routed("/interfaces/interface[name=eth0]"));
}

TEST_F(BackendTest, SnapshotSkipsUnsetLeaves) {
    auto v = be.snapshot("/system");
    ASSERT_TRUE(v.routed);
    EXPECT_TRUE(v.leaves.count("/system/config/hostname"));
    EXPECT_FALSE(v.leaves.count("/system/config/timezone-name"));  // declared but unset
}

TEST_F(BackendTest, AtomicGroupVisibleInSnapshot) {
    auto v = be.snapshot("/system");
    bool found = false;
    for (const auto& g : v.groups)
        if (g.atomic && g.prefix == NTP) found = true;
    EXPECT_TRUE(found);
}

TEST_F(BackendTest, WritabilityIsSchemaPlane) {
    EXPECT_TRUE(be.writable("/system/config/hostname"));
    EXPECT_TRUE(be.writable("/system/config/timezone-name"));  // declared, even if unset
    EXPECT_FALSE(be.writable("/components/component[name=PSC-0]/state/temperature/instant"));
    EXPECT_FALSE(be.writable("/system/config/undeclared"));
}

TEST_F(BackendTest, PreferredModeFromSchemaType) {
    EXPECT_EQ(be.preferredMode("/system"), gnmi::ON_CHANGE);                       // Config
    EXPECT_EQ(be.preferredMode("/components/component[name=PSC-0]"), gnmi::SAMPLE);  // Operational
}

TEST_F(BackendTest, CommitUpdateReflectedAndBumpsChangeSeq) {
    const uint64_t before = be.snapshot("/system").leaves.at("/system/config/hostname").changeSeq;
    be.commit({ SetOp{SetOp::Kind::Update, "/system/config/hostname",
                      typedValue(std::string("newname")), 100} });
    auto after = be.snapshot("/system").leaves.at("/system/config/hostname");
    EXPECT_EQ(after.value->string_val(), "newname");
    EXPECT_GT(after.changeSeq, before);
}

TEST_F(BackendTest, DeleteRemovesLeafButSchemaPlanePersists) {
    be.commit({ SetOp{SetOp::Kind::Delete, "/system/config/hostname", {}, 0} });
    EXPECT_FALSE(be.snapshot("/system").leaves.count("/system/config/hostname"));
    EXPECT_TRUE(be.writable("/system/config/hostname"));  // still Set-able

    be.commit({ SetOp{SetOp::Kind::Update, "/system/config/hostname",
                      typedValue(std::string("again")), 200} });
    EXPECT_EQ(be.snapshot("/system").leaves.at("/system/config/hostname").value->string_val(),
              "again");
}

// ---- runtime hot-plug: attach/detach a device branch after serving has begun ----
// The branch lands under /components/component, already an owned namespace (SensorStub),
// so routing needs no change — only the registry contents + bindings grow/shrink.

const std::string PSU9 = "/components/component[name=PSU-9]";

core::SubtreeSpec psuSensorSpec() {
    core::SubtreeSpec spec;
    spec.leaves.push_back({ PSU9 + "/state/temperature/instant", LeafType::Operational, typedValue(50.0) });
    spec.leaves.push_back({ PSU9 + "/power-supply/state/output-power", LeafType::Operational, typedValue(240.0) });
    return spec;
}

TEST_F(BackendTest, AttachSubtreeIsRoutedAndVisibleAtRuntime) {
    EXPECT_TRUE(be.snapshot(PSU9).leaves.empty());  // not present yet

    auto ids = be.attachSubtree(psuSensorSpec());
    EXPECT_EQ(ids.size(), 2u);

    auto v = be.snapshot(PSU9);
    ASSERT_TRUE(v.routed);
    EXPECT_TRUE(v.leaves.count(PSU9 + "/state/temperature/instant"));
    EXPECT_TRUE(v.leaves.count(PSU9 + "/power-supply/state/output-power"));
}

TEST_F(BackendTest, AttachSyncsWritablePlaneByEffectiveType) {
    core::SubtreeSpec spec;
    spec.leaves.push_back({ PSU9 + "/config/serial-no", LeafType::Config, std::nullopt });
    spec.leaves.push_back({ PSU9 + "/state/output-power", LeafType::Operational, typedValue(240.0) });
    be.attachSubtree(spec);
    EXPECT_TRUE(be.writable(PSU9 + "/config/serial-no"));    // Config -> writable
    EXPECT_FALSE(be.writable(PSU9 + "/state/output-power"));  // Operational -> not
}

TEST_F(BackendTest, DetachSubtreeRemovesLeavesAndStalesIds) {
    auto ids = be.attachSubtree(psuSensorSpec());
    const core::LeafId held = ids.begin()->second;

    be.detachSubtree(PSU9);
    EXPECT_TRUE(be.snapshot(PSU9).leaves.empty());

    // a held id is now stale — a value write through it is a clean miss, not UB
    core::ValueWriter w = be.registry().writeValues();
    EXPECT_FALSE(w.set(held, typedValue(1.0), 0));
}

TEST_F(BackendTest, DetachClearsWritablePlane) {
    core::SubtreeSpec spec;
    spec.leaves.push_back({ PSU9 + "/config/serial-no", LeafType::Config, std::nullopt });
    be.attachSubtree(spec);
    ASSERT_TRUE(be.writable(PSU9 + "/config/serial-no"));
    be.detachSubtree(PSU9);
    EXPECT_FALSE(be.writable(PSU9 + "/config/serial-no"));
}

TEST_F(BackendTest, AttachEmitsAddedDetachEmitsRemoved) {
    struct RecordingSink : core::ILeafSink {
        std::vector<std::shared_ptr<const core::ChangeBatch>> batches;
        void onChange(std::shared_ptr<const core::ChangeBatch> b) noexcept override {
            batches.push_back(std::move(b));
        }
    } sink;
    be.registry().setSink(&sink);

    be.attachSubtree(psuSensorSpec());
    ASSERT_FALSE(sink.batches.empty());
    EXPECT_EQ(sink.batches.back()->added.size(), 2u);          // one batch, both leaves
    EXPECT_TRUE(sink.batches.back()->removedPrefixes.empty());

    sink.batches.clear();
    be.detachSubtree(PSU9);
    ASSERT_FALSE(sink.batches.empty());
    EXPECT_FALSE(sink.batches.back()->removedPrefixes.empty());
    EXPECT_TRUE(sink.batches.back()->added.empty());

    be.registry().setSink(nullptr);  // unhook before the local sink dies
}

TEST_F(BackendTest, SetReachesHotPluggedConfigLeaf) {
    core::SubtreeSpec spec;
    spec.leaves.push_back({ PSU9 + "/config/serial-no", LeafType::Config, std::nullopt });
    be.attachSubtree(spec);
    be.commit({ SetOp{SetOp::Kind::Update, PSU9 + "/config/serial-no",
                      typedValue(std::string("SN-123")), 100} });
    EXPECT_EQ(be.snapshot(PSU9).leaves.at(PSU9 + "/config/serial-no").value->string_val(),
              "SN-123");
}

}  // namespace
