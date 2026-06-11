#include <gtest/gtest.h>
#include "backend/system_config_provider.hpp"

// ---------------------------------------------------------------------------
// SystemConfigProvider — read side (Piece A). Seeds default config leaves and
// serves them via fill()/snapshot(); no simulator, so values are stable.
// ---------------------------------------------------------------------------

TEST(SystemConfigFill, ExactLeafProducesOneUpdate) {
    SystemConfigProvider p;
    RepeatedPtrField<Update> list;
    p.fill(&list, "/system/config/hostname");
    ASSERT_EQ(list.size(), 1);
    EXPECT_EQ(list[0].val().string_val(), "psc-mock");
}

TEST(SystemConfigFill, SubtreeProducesAllConfigLeaves) {
    SystemConfigProvider p;
    RepeatedPtrField<Update> list;
    p.fill(&list, "/system/config");
    EXPECT_EQ(list.size(), 3);  // hostname, login-banner, motd-banner
}

TEST(SystemConfigFill, UnknownLeafProducesNoUpdates) {
    SystemConfigProvider p;
    RepeatedPtrField<Update> list;
    p.fill(&list, "/system/config/nonexistent");
    EXPECT_EQ(list.size(), 0);
}

TEST(SystemConfigSnapshot, CarriesValueAndCollectionTime) {
    SystemConfigProvider p;
    Snapshot snap = p.snapshot("/system/config/hostname");
    ASSERT_EQ(snap.size(), 1u);
    const Leaf& leaf = snap.at("/system/config/hostname");
    EXPECT_EQ(leaf.val.string_val(), "psc-mock");
    EXPECT_GT(leaf.collectedNs, 0);
}

// Config is event-driven: TARGET_DEFINED must resolve to ON_CHANGE here.
TEST(SystemConfigMode, PrefersOnChange) {
    SystemConfigProvider p;
    EXPECT_EQ(p.preferredMode("/system/config/hostname"), gnmi::ON_CHANGE);
}

// ---------------------------------------------------------------------------
// Write side (Piece B/C). gNMI Set drives applyUpdate/applyDelete; the mutation
// must show up in snapshot(), which is what the Subscribe loop diffs to emit
// ON_CHANGE. config true → every owned path is writable.
// ---------------------------------------------------------------------------

TEST(SystemConfigWrite, EveryConfigPathIsWritable) {
    SystemConfigProvider p;
    EXPECT_TRUE(p.writable("/system/config/hostname"));
}

TEST(SystemConfigWrite, ApplyBatchChangesSnapshot) {
    SystemConfigProvider p;
    EXPECT_TRUE(p.applyBatch(
        WriteBatch{}.set("/system/config/hostname",
                         std::string("edge-psc-7"), 12345)));

    Snapshot snap = p.snapshot("/system/config/hostname");
    ASSERT_EQ(snap.size(), 1u);
    const Leaf& leaf = snap.at("/system/config/hostname");
    EXPECT_EQ(leaf.val.string_val(), "edge-psc-7");
    EXPECT_EQ(leaf.collectedNs, 12345);   // Set's transaction timestamp
}

TEST(SystemConfigWrite, ApplyBatchCreatesNewLeaf) {
    SystemConfigProvider p;
    EXPECT_TRUE(p.applyBatch(
        WriteBatch{}.set("/system/config/timezone-name", std::string("UTC"), 1)));

    Snapshot snap = p.snapshot("/system/config/timezone-name");
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap.at("/system/config/timezone-name").val.string_val(), "UTC");
}

TEST(SystemConfigWrite, ApplyBatchRemovesLeaf) {
    SystemConfigProvider p;
    EXPECT_TRUE(p.applyBatch(WriteBatch{}.remove("/system/config/hostname")));

    EXPECT_TRUE(p.snapshot("/system/config/hostname").empty());
    RepeatedPtrField<Update> list;
    p.fill(&list, "/system/config/hostname");
    EXPECT_EQ(list.size(), 0);
}

// Deleting an absent leaf is silently accepted (§3.4.6): remove() is idempotent.
TEST(SystemConfigWrite, ApplyBatchDeleteAbsentLeafSucceeds) {
    SystemConfigProvider p;
    EXPECT_TRUE(p.applyBatch(WriteBatch{}.remove("/system/config/never-set")));
}

// A multi-leaf write to the atomic NTP record lands as one transaction: every
// changed leaf is visible together, stamped with the shared collection time.
TEST(SystemConfigWrite, ApplyBatchWritesNtpRecordCoherently) {
    SystemConfigProvider p;
    const std::string ntp = "/system/ntp/servers/server[address=10.0.0.1]/config";
    gnmi::TypedValue ver;   ver.set_uint_val(5);
    gnmi::TypedValue assoc; assoc.set_string_val("PEER");
    EXPECT_TRUE(p.applyBatch(WriteBatch{}
        .set(ntp + "/version", ver, 7000)
        .set(ntp + "/association-type", assoc, 7000)));

    Snapshot snap = p.snapshot(ntp);
    EXPECT_EQ(snap.at(ntp + "/version").val.uint_val(), 5u);
    EXPECT_EQ(snap.at(ntp + "/association-type").val.string_val(), "PEER");
    EXPECT_EQ(snap.at(ntp + "/version").collectedNs, 7000);
    EXPECT_EQ(snap.at(ntp + "/association-type").collectedNs, 7000);
}

// We own a broad /system prefix, so only config-true paths may be written; a
// hypothetical read-only /system/.../state leaf must be refused.
TEST(SystemConfigWrite, NonConfigPathIsNotWritable) {
    SystemConfigProvider p;
    EXPECT_TRUE(p.writable("/system/ntp/servers/server[address=10.0.0.1]/config/port"));
    EXPECT_FALSE(p.writable("/system/state/current-datetime"));
}

// The schema is stable, independent of current data: deleting a config leaf and
// setting it again works, and the revived leaf still carries LeafType::Config —
// the type comes from the schema, never from whether a value happens to exist.
TEST(SystemConfigWrite, ResetAfterDeleteKeepsConfigType) {
    SystemConfigProvider p;
    const std::string path = "/system/config/hostname";
    EXPECT_TRUE(p.applyBatch(WriteBatch{}.remove(path)));
    EXPECT_TRUE(p.snapshot(path).empty());

    EXPECT_TRUE(p.applyBatch(WriteBatch{}.set(path, std::string("revived"), 9)));
    Snapshot snap = p.snapshot(path);
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap.at(path).val.string_val(), "revived");
    EXPECT_EQ(snap.at(path).type, LeafType::Config);
}

// ---------------------------------------------------------------------------
// Atomic containers. NTP server records are atomic (the whole .../config record
// is one atomic notification, spec §2.1.1); the flat /system/config scalars are
// not. atomicPrefix() reports the owning container, or nullopt.
// ---------------------------------------------------------------------------

TEST(SystemConfigAtomic, NtpLeafReportsItsServerConfigContainer) {
    SystemConfigProvider p;
    const std::string container =
        "/system/ntp/servers/server[address=10.0.0.1]/config";
    auto ap = p.atomicPrefix(container + "/port");
    ASSERT_TRUE(ap.has_value());
    EXPECT_EQ(*ap, container);
}

TEST(SystemConfigAtomic, NtpConfigContainerItselfIsAtomic) {
    SystemConfigProvider p;
    const std::string container =
        "/system/ntp/servers/server[address=10.0.0.1]/config";
    auto ap = p.atomicPrefix(container);
    ASSERT_TRUE(ap.has_value());
    EXPECT_EQ(*ap, container);
}

TEST(SystemConfigAtomic, FlatConfigScalarsAreNotAtomic) {
    SystemConfigProvider p;
    EXPECT_FALSE(p.atomicPrefix("/system/config/hostname").has_value());
}

TEST(SystemConfigAtomic, SeededNtpRecordIsServedAsAtomicLeaves) {
    SystemConfigProvider p;
    Snapshot snap =
        p.snapshot("/system/ntp/servers/server[address=10.0.0.1]/config");
    // address, port, version, iburst, association-type
    EXPECT_EQ(snap.size(), 5u);
    EXPECT_EQ(snap.at("/system/ntp/servers/server[address=10.0.0.1]/config/port")
                  .val.uint_val(), 123u);
}
