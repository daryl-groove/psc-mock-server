// Phase 1 push-seam unit test (P1 / D6). Pins the ILeafSink / ChangeBatch / LeafChange
// contract directly against the registry: enriched value-change payload, structural
// add/remove batching, the unregisterGroup carve-out (Fork 4), zero-overhead when no
// sink is attached, and the L=B lifetime guarantee (the dispatched batch outlives the
// writer scope AND a subsequent detach).

#include <memory>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "leaf_registry.hpp"
#include "leaf_sink.hpp"

using namespace gnmid::core;

namespace {

gnmi::TypedValue strv(const std::string& s) {
    gnmi::TypedValue v;
    v.set_string_val(s);
    return v;
}

// Records every batch it is handed, keeping the shared_ptr alive (lifetime test).
class RecordingSink : public ILeafSink {
public:
    void onChange(std::shared_ptr<const ChangeBatch> batch) noexcept override {
        batches.push_back(std::move(batch));
    }
    std::vector<std::shared_ptr<const ChangeBatch>> batches;
};

}  // namespace

TEST(LeafSink, ValueWriteProducesEnrichedChanged) {
    LeafRegistry reg;
    RecordingSink sink;
    reg.setSink(&sink);
    LeafId id = reg.registerLeaf("/a/b");  // batch[0] = added

    {
        auto w = reg.writeValues();
        w.set(id, strv("x"), 42);
    }  // batch[1] = changed, dispatched on writer destruction

    ASSERT_EQ(sink.batches.size(), 2u);
    const ChangeBatch& b = *sink.batches.back();
    ASSERT_EQ(b.changed.size(), 1u);
    EXPECT_TRUE(b.added.empty());
    EXPECT_TRUE(b.removedPrefixes.empty());

    const LeafChange& c = b.changed[0];
    EXPECT_EQ(c.path->str(), "/a/b");
    EXPECT_EQ(c.id, id);
    ASSERT_TRUE(c.value);
    EXPECT_EQ(c.value->string_val(), "x");
    EXPECT_EQ(c.collectedNs, 42);
    EXPECT_GT(c.changeSeq, 0u);
}

TEST(LeafSink, UnchangedValueRecordsNothing) {
    LeafRegistry reg;
    RecordingSink sink;
    LeafId id = reg.registerLeaf("/a/b", std::nullopt, strv("x"));
    reg.setSink(&sink);  // attach AFTER the register, so only writes show up

    {
        auto w = reg.writeValues();
        EXPECT_TRUE(w.set(id, strv("x"), 9));  // identical → value-gated no-op
    }
    EXPECT_TRUE(sink.batches.empty());  // empty batch is suppressed, not dispatched

    {
        auto w = reg.writeValues();
        w.set(id, strv("y"), 10);  // real change
    }
    ASSERT_EQ(sink.batches.size(), 1u);
    EXPECT_EQ(sink.batches[0]->changed.size(), 1u);
}

TEST(LeafSink, NoSinkIsNoOp) {
    LeafRegistry reg;  // no sink attached → the poll/test path
    LeafId id = reg.registerLeaf("/a/b");
    {
        auto w = reg.writeValues();
        EXPECT_TRUE(w.set(id, strv("x"), 1));  // works, no dispatch, no crash
    }
    auto snap = reg.getLeaf(id);
    ASSERT_TRUE(snap);
    ASSERT_TRUE(snap->value);
    EXPECT_EQ(snap->value->string_val(), "x");
}

TEST(LeafSink, RegisterLeafAddsIncludingUnset) {
    LeafRegistry reg;
    RecordingSink sink;
    reg.setSink(&sink);
    reg.registerLeaf("/a/unset");                         // unset → value nullptr
    reg.registerLeaf("/a/set", std::nullopt, strv("v"));  // with a value

    ASSERT_EQ(sink.batches.size(), 2u);

    ASSERT_EQ(sink.batches[0]->added.size(), 1u);
    EXPECT_EQ(sink.batches[0]->added[0].path->str(), "/a/unset");
    EXPECT_FALSE(sink.batches[0]->added[0].value);  // faithfully unset (Fork 3b)

    ASSERT_EQ(sink.batches[1]->added.size(), 1u);
    EXPECT_EQ(sink.batches[1]->added[0].path->str(), "/a/set");
    ASSERT_TRUE(sink.batches[1]->added[0].value);
    EXPECT_EQ(sink.batches[1]->added[0].value->string_val(), "v");
}

TEST(LeafSink, AttachSubtreeBatchesAllAddsOnce) {
    LeafRegistry reg;
    RecordingSink sink;
    reg.setSink(&sink);

    SubtreeSpec spec;
    spec.leaves.push_back({.path = "/dev/x", .type = std::nullopt, .initialValue = strv("1")});
    spec.leaves.push_back({.path = "/dev/y", .type = std::nullopt, .initialValue = strv("2")});
    reg.attachSubtree(spec);

    ASSERT_EQ(sink.batches.size(), 1u);  // ONE batch for the whole branch
    EXPECT_EQ(sink.batches[0]->added.size(), 2u);
}

TEST(LeafSink, DetachAndUnregisterLeafProduceRemovedPrefixes) {
    LeafRegistry reg;
    RecordingSink sink;
    reg.registerLeaf("/a/b");
    reg.registerLeaf("/c/d");
    reg.setSink(&sink);

    reg.unregisterLeaf("/a/b");
    ASSERT_EQ(sink.batches.size(), 1u);
    ASSERT_EQ(sink.batches[0]->removedPrefixes.size(), 1u);
    EXPECT_EQ(sink.batches[0]->removedPrefixes[0].str(), "/a/b");
    EXPECT_TRUE(sink.batches[0]->added.empty());
    EXPECT_TRUE(sink.batches[0]->changed.empty());

    reg.detachSubtree("/c");  // branch-level: ONE prefix, not per-leaf
    ASSERT_EQ(sink.batches.size(), 2u);
    ASSERT_EQ(sink.batches[1]->removedPrefixes.size(), 1u);
    EXPECT_EQ(sink.batches[1]->removedPrefixes[0].str(), "/c");
}

TEST(LeafSink, UnregisterGroupEmitsNoEvent) {
    LeafRegistry reg;
    RecordingSink sink;
    reg.registerGroup("/g", false);
    reg.registerLeaf("/g/a");
    reg.setSink(&sink);

    reg.unregisterGroup("/g");  // Fork 4 carve-out: ungroup != delete → no event
    EXPECT_TRUE(sink.batches.empty());
    EXPECT_TRUE(reg.getLeaf("/g/a").has_value());  // leaf survives as ungrouped
}

TEST(LeafSink, BatchOutlivesWriterAndDetach) {
    // L=B lifetime: the dispatched batch's path/value handles stay valid after the
    // writer scope exits AND after the leaf is removed from the registry.
    LeafRegistry reg;
    RecordingSink sink;
    reg.setSink(&sink);
    LeafId id = reg.registerLeaf("/a/b");
    {
        auto w = reg.writeValues();
        w.set(id, strv("x"), 1);
    }

    std::shared_ptr<const ChangeBatch> held = sink.batches.back();
    reg.unregisterLeaf("/a/b");  // leaf gone from the registry

    ASSERT_FALSE(held->changed.empty());
    EXPECT_EQ(held->changed[0].path->str(), "/a/b");
    ASSERT_TRUE(held->changed[0].value);
    EXPECT_EQ(held->changed[0].value->string_val(), "x");
}
