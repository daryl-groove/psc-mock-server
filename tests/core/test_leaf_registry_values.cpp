#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "leaf_registry.hpp"

using namespace gnmid::core;

namespace {

gnmi::TypedValue strv(const std::string& s) {
    gnmi::TypedValue v;
    v.set_string_val(s);
    return v;
}

std::vector<std::string> sortedPaths(const LeafSnapshot& snap) {
    std::vector<std::string> paths;
    for (const auto& [path, _] : snap) paths.push_back(path);
    std::sort(paths.begin(), paths.end());
    return paths;
}

// The D14 poll-diff over the union of before/after snapshot keys (test-side; the
// push event seam is deferred — core exposes only the changeSeq token).
struct Diff {
    std::vector<std::string> added;    // in after, not before  -> wire update
    std::vector<std::string> removed;  // in before, not after  -> wire delete
    std::vector<std::string> changed;  // in both, changeSeq differs -> wire update
};
Diff diff(const LeafSnapshot& before, const LeafSnapshot& after) {
    Diff d;
    for (const auto& [k, v] : after) {
        auto it = before.find(k);
        if (it == before.end())                       d.added.push_back(k);
        else if (it->second.changeSeq != v.changeSeq) d.changed.push_back(k);
    }
    for (const auto& [k, v] : before) {
        if (!after.count(k)) d.removed.push_back(k);
    }
    return d;
}

}  // namespace

// --- collectLeaves subtree boundary (Phase 4.2, §2.4.2) -------------------

TEST(Values, CollectLeavesReturnsOnlyLeavesUnderPrefix) {
    LeafRegistry reg;
    reg.registerLeaf("/a/b/c/d1");
    reg.registerLeaf("/a/b/c/d2");
    reg.registerLeaf("/a/b/cx");  // string-prefix of /a/b/c but NOT a subtree member
    reg.registerLeaf("/a/x/y");

    EXPECT_EQ(sortedPaths(reg.collectLeaves("/a/b/c")),
              (std::vector<std::string>{"/a/b/c/d1", "/a/b/c/d2"}));
}

TEST(Values, CollectLeavesIncludesExactPrefixLeaf) {
    LeafRegistry reg;
    reg.registerLeaf("/a/b/c/g1");
    reg.registerLeaf("/a/b/c/g2");
    EXPECT_EQ(sortedPaths(reg.collectLeaves("/a/b/c/g1")),
              (std::vector<std::string>{"/a/b/c/g1"}));
}

TEST(Values, CollectLeavesAtRootReturnsEverything) {
    LeafRegistry reg;
    reg.registerLeaf("/a/b");
    reg.registerLeaf("/c/d");
    EXPECT_EQ(sortedPaths(reg.collectLeaves("/")), (std::vector<std::string>{"/a/b", "/c/d"}));
}

TEST(Values, CollectLeavesResolvesEffectiveType) {
    LeafRegistry reg;
    reg.registerGroup("/a/b", /*atomic=*/false, LeafType::State);
    reg.registerLeaf("/a/b/inherits");
    reg.registerLeaf("/a/b/own", LeafType::Config);
    reg.registerLeaf("/a/lonely");

    LeafSnapshot snap = reg.collectLeaves("/a");
    EXPECT_EQ(snap.at("/a/b/inherits").effectiveType, LeafType::State);
    EXPECT_EQ(snap.at("/a/b/own").effectiveType, LeafType::Config);
    EXPECT_EQ(snap.at("/a/lonely").effectiveType, LeafType::Operational);
}

// --- Value-gated changeSeq (Phase 4.1, D14) -------------------------------

TEST(Values, SetInstallsValueAndBumpsChangeSeq) {
    LeafRegistry reg;
    LeafId id = reg.registerLeaf("/a/x");
    EXPECT_FALSE(reg.getLeaf("/a/x")->value);      // unset
    EXPECT_EQ(reg.getLeaf("/a/x")->changeSeq, 0u);

    {
        auto w = reg.writeValues();
        EXPECT_TRUE(w.set(id, strv("hello"), 100));
    }
    auto snap = reg.getLeaf("/a/x");
    ASSERT_TRUE(snap->value);
    EXPECT_EQ(snap->value->string_val(), "hello");
    EXPECT_EQ(snap->collectedNs, 100);
    EXPECT_GT(snap->changeSeq, 0u);
}

TEST(Values, RepushedIdenticalValueIsNoOp) {
    LeafRegistry reg;
    LeafId id = reg.registerLeaf("/a/x");
    { auto w = reg.writeValues(); w.set(id, strv("a"), 1); }
    const uint64_t afterFirst = reg.getLeaf("/a/x")->changeSeq;

    { auto w = reg.writeValues(); EXPECT_TRUE(w.set(id, strv("a"), 2)); }  // identical -> no-op
    EXPECT_EQ(reg.getLeaf("/a/x")->changeSeq, afterFirst);
    EXPECT_EQ(reg.getLeaf("/a/x")->collectedNs, 1);  // timestamp not advanced either

    { auto w = reg.writeValues(); w.set(id, strv("b"), 3); }  // real change
    EXPECT_GT(reg.getLeaf("/a/x")->changeSeq, afterFirst);
}

TEST(Values, ChangeSeqIsGloballyMonotonic) {
    LeafRegistry reg;
    LeafId a = reg.registerLeaf("/a");
    LeafId b = reg.registerLeaf("/b");
    { auto w = reg.writeValues(); w.set(a, strv("1"), 1); }
    const uint64_t seqA = reg.getLeaf("/a")->changeSeq;
    { auto w = reg.writeValues(); w.set(b, strv("1"), 1); }
    const uint64_t seqB = reg.getLeaf("/b")->changeSeq;
    { auto w = reg.writeValues(); w.set(a, strv("2"), 2); }
    const uint64_t seqA2 = reg.getLeaf("/a")->changeSeq;

    EXPECT_LT(seqA, seqB);
    EXPECT_LT(seqB, seqA2);
}

TEST(Values, InvalidIdReturnsFalse) {
    LeafRegistry reg;
    LeafId invalid;
    auto w = reg.writeValues();
    EXPECT_FALSE(w.set(invalid, strv("x"), 1));
}

// --- COW point-in-time (Phase 4.4, D17) -----------------------------------

TEST(Values, EarlierSnapshotKeepsOldVersionAfterLaterWrite) {
    LeafRegistry reg;
    LeafId id = reg.registerLeaf("/a/x");
    { auto w = reg.writeValues(); w.set(id, strv("v1"), 1); }

    auto snap1 = reg.getLeaf("/a/x");          // captures the v1 version handle
    { auto w = reg.writeValues(); w.set(id, strv("v2"), 2); }
    auto snap2 = reg.getLeaf("/a/x");

    EXPECT_EQ(snap1->value->string_val(), "v1");  // old version still valid
    EXPECT_EQ(snap2->value->string_val(), "v2");
}

// --- D14 poll-diff contract (Phase 4.3) -----------------------------------

TEST(Values, PollDiffDetectsAddedAndChanged) {
    LeafRegistry reg;
    LeafId a = reg.registerLeaf("/a");
    { auto w = reg.writeValues(); w.set(a, strv("1"), 1); }

    LeafSnapshot before = reg.collectLeaves("/");

    { auto w = reg.writeValues(); w.set(a, strv("2"), 2); }  // change a
    LeafId b = reg.registerLeaf("/b");                       // add b
    { auto w = reg.writeValues(); w.set(b, strv("9"), 3); }

    LeafSnapshot after = reg.collectLeaves("/");
    Diff d = diff(before, after);
    EXPECT_EQ(d.added, (std::vector<std::string>{"/b"}));
    EXPECT_EQ(d.changed, (std::vector<std::string>{"/a"}));
    EXPECT_TRUE(d.removed.empty());
}

TEST(Values, PollDiffNoChangeIsEmpty) {
    LeafRegistry reg;
    LeafId a = reg.registerLeaf("/a");
    { auto w = reg.writeValues(); w.set(a, strv("1"), 1); }

    LeafSnapshot before = reg.collectLeaves("/");
    { auto w = reg.writeValues(); w.set(a, strv("1"), 9); }  // identical re-push
    LeafSnapshot after = reg.collectLeaves("/");

    Diff d = diff(before, after);
    EXPECT_TRUE(d.added.empty() && d.removed.empty() && d.changed.empty());
}

// --- Atomic-group coherence via one writeValues scope (Scenario 6) --------

TEST(Values, OneWriteScopeAppliesAllAtomicMembers) {
    LeafRegistry reg;
    reg.registerGroup("/system/ntp", /*atomic=*/true);
    LeafId server = reg.registerLeaf("/system/ntp/server");
    LeafId port   = reg.registerLeaf("/system/ntp/port");
    LeafId vrf    = reg.registerLeaf("/system/ntp/vrf");
    {
        auto w = reg.writeValues();
        w.set(server, strv("10.0.0.1"), 1);
        w.set(port, strv("123"), 1);
        w.set(vrf, strv("mgmt"), 1);
    }
    LeafSnapshot snap = reg.collectLeaves("/system/ntp");
    EXPECT_EQ(snap.at("/system/ntp/server").value->string_val(), "10.0.0.1");
    EXPECT_EQ(snap.at("/system/ntp/port").value->string_val(), "123");
    EXPECT_EQ(snap.at("/system/ntp/vrf").value->string_val(), "mgmt");
}

TEST(Values, ConcurrentReaderNeverSeesHalfUpdatedAtomicGroup) {
    LeafRegistry reg;
    reg.registerGroup("/system/ntp", /*atomic=*/true);
    LeafId a = reg.registerLeaf("/system/ntp/a");
    LeafId b = reg.registerLeaf("/system/ntp/b");
    LeafId c = reg.registerLeaf("/system/ntp/c");
    auto writeGen = [&](int gen) {
        const std::string g = "g" + std::to_string(gen);
        auto w = reg.writeValues();
        w.set(a, strv(g), gen);
        w.set(b, strv(g), gen);
        w.set(c, strv(g), gen);
    };
    writeGen(0);  // seed so the reader never sees nullptr

    std::atomic<bool> stop{false};
    auto reader = [&] {
        while (!stop.load()) {
            LeafSnapshot s = reg.collectLeaves("/system/ntp");
            const std::string ga = s.at("/system/ntp/a").value->string_val();
            const std::string gb = s.at("/system/ntp/b").value->string_val();
            const std::string gc = s.at("/system/ntp/c").value->string_val();
            ASSERT_EQ(ga, gb);  // all three move together (one exclusive-locked scope)
            ASSERT_EQ(gb, gc);
        }
    };
    std::thread r1(reader), r2(reader);
    for (int gen = 1; gen <= 20000; ++gen) writeGen(gen);
    stop.store(true);
    r1.join();
    r2.join();
}

// --- Concurrency invariants (Phase 4.4) -----------------------------------

TEST(Values, ConcurrentWritersSerializeWithoutLostUpdates) {
    LeafRegistry reg;
    LeafId id = reg.registerLeaf("/a/counter");

    constexpr int threadCount = 8;
    constexpr int setsPerThread = 2000;
    auto worker = [&](int tid) {
        for (int i = 0; i < setsPerThread; ++i) {
            auto w = reg.writeValues();
            w.set(id, strv("t" + std::to_string(tid) + "_" + std::to_string(i)), i);
        }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < threadCount; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();

    // Every set wrote a unique value -> every set is a real change -> the global
    // token advanced exactly threadCount*setsPerThread times with no lost updates.
    EXPECT_EQ(reg.getLeaf("/a/counter")->changeSeq,
              static_cast<uint64_t>(threadCount) * setsPerThread);
}

TEST(Values, ReadsAndWritesDoNotDeadlock) {
    LeafRegistry reg;
    std::vector<LeafId> ids;
    for (int i = 0; i < 50; ++i) ids.push_back(reg.registerLeaf("/a/leaf" + std::to_string(i)));

    std::atomic<bool> stop{false};
    auto reader = [&] {
        while (!stop.load()) EXPECT_EQ(reg.collectLeaves("/a").size(), 50u);
    };
    auto writer = [&] {
        for (int i = 0; i < 10000; ++i) {
            auto w = reg.writeValues();
            w.set(ids[0], strv("v" + std::to_string(i)), i);
        }
    };
    std::thread r1(reader), r2(reader), wth(writer);
    wth.join();
    stop.store(true);
    r1.join();
    r2.join();
    SUCCEED();  // reaching here without hanging proves no shared/exclusive deadlock
}
