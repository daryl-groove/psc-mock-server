// Push-routing unit test (P1/P2, S2): pins SubscriptionHub's change->stream matching
// (changeTouchesQueries) — the part where routing bugs live — directly against
// ChangeBatch inputs, with no gRPC stream and no threads. The cv/wake plumbing is
// standard and exercised end-to-end by tests/e2e/test_push.py.

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "canonical_path.hpp"
#include "leaf_sink.hpp"
#include "subscription.h"

using gnmid::core::CanonicalPath;
using gnmid::core::canonicalize;
using gnmid::core::ChangeBatch;
using gnmid::core::LeafChange;
using impl::changeTouchesQueries;

namespace {

CanonicalPath cp(const char* s) { return canonicalize(s); }

LeafChange changedLeaf(const char* path) {
  LeafChange c;
  c.path = std::make_shared<const CanonicalPath>(canonicalize(path));
  return c;
}

}  // namespace

TEST(ChangeTouchesQueries, ChangedLeafUnderQueryMatches) {
  std::vector<CanonicalPath> q{cp("/system/config")};
  ChangeBatch b;
  b.changed.push_back(changedLeaf("/system/config/hostname"));
  EXPECT_TRUE(changeTouchesQueries(q, b));
}

TEST(ChangeTouchesQueries, ChangedLeafOutsideQueryNoMatch) {
  std::vector<CanonicalPath> q{cp("/system/config")};
  ChangeBatch b;
  b.changed.push_back(changedLeaf("/components/component[name=PSC-0]/state/temperature"));
  EXPECT_FALSE(changeTouchesQueries(q, b));
}

TEST(ChangeTouchesQueries, ExactQueryLeafMatches) {
  std::vector<CanonicalPath> q{cp("/a/b/c")};
  ChangeBatch b;
  b.changed.push_back(changedLeaf("/a/b/c"));  // ancestor-or-equal
  EXPECT_TRUE(changeTouchesQueries(q, b));
}

TEST(ChangeTouchesQueries, ElementAlignedNotStringPrefix) {
  std::vector<CanonicalPath> q{cp("/a/b")};
  ChangeBatch b;
  b.changed.push_back(changedLeaf("/a/bc"));  // string prefix but NOT element-aligned
  EXPECT_FALSE(changeTouchesQueries(q, b));
}

TEST(ChangeTouchesQueries, MultipleQueriesMatchOnSecond) {
  std::vector<CanonicalPath> q{cp("/components"), cp("/system/config")};
  ChangeBatch b;
  b.changed.push_back(changedLeaf("/system/config/hostname"));
  EXPECT_TRUE(changeTouchesQueries(q, b));
}

TEST(ChangeTouchesQueries, AddedLeafUnderQueryMatches) {
  std::vector<CanonicalPath> q{cp("/components")};
  ChangeBatch b;
  b.added.push_back(changedLeaf("/components/component[name=lc1]/state/temperature"));
  EXPECT_TRUE(changeTouchesQueries(q, b));
}

TEST(ChangeTouchesQueries, RemovedLeafUnderQueryMatches) {
  // The Set-delete case: unregisterLeaf -> removedPrefixes[leaf]; a sub on the
  // container must be woken to emit the delete.
  std::vector<CanonicalPath> q{cp("/system/config")};
  ChangeBatch b;
  b.removedPrefixes.push_back(cp("/system/config/motd-banner"));
  EXPECT_TRUE(changeTouchesQueries(q, b));
}

TEST(ChangeTouchesQueries, RemovedBranchAboveQueryMatches) {
  // A deep query; a branch deleted ABOVE it must still wake the sub (bidirectional).
  std::vector<CanonicalPath> q{cp("/components/component[name=lc1]/state/temperature")};
  ChangeBatch b;
  b.removedPrefixes.push_back(cp("/components/component[name=lc1]"));
  EXPECT_TRUE(changeTouchesQueries(q, b));
}

TEST(ChangeTouchesQueries, UnrelatedRemovedNoMatch) {
  std::vector<CanonicalPath> q{cp("/system/config")};
  ChangeBatch b;
  b.removedPrefixes.push_back(cp("/components/component[name=lc1]"));
  EXPECT_FALSE(changeTouchesQueries(q, b));
}

TEST(ChangeTouchesQueries, EmptyQueriesNeverMatch) {
  std::vector<CanonicalPath> q;  // a pure-SAMPLE stream registers no ON_CHANGE queries
  ChangeBatch b;
  b.changed.push_back(changedLeaf("/system/config/hostname"));
  EXPECT_FALSE(changeTouchesQueries(q, b));
}

// --- P4: bare key-omitted list query matches its keyed entries (the list fan-out,
// shared with Backend::selects) so a per-slot hot-plug wakes a list-level ON_CHANGE
// stream immediately instead of falling back to the ~1s liveness re-diff. ----------

TEST(ChangeTouchesQueries, KeyOmittedListMatchesChangedKeyedLeaf) {
  std::vector<CanonicalPath> q{cp("/components/component")};
  ChangeBatch b;
  b.changed.push_back(changedLeaf("/components/component[name=PSC-0]/state/temperature"));
  EXPECT_TRUE(changeTouchesQueries(q, b));
}

TEST(ChangeTouchesQueries, KeyOmittedListMatchesAddedKeyedLeaf) {
  // Hot-plug insert: the sensor subtree appears under a keyed entry.
  std::vector<CanonicalPath> q{cp("/components/component")};
  ChangeBatch b;
  b.added.push_back(changedLeaf("/components/component[name=PSC-0]/power-supply/state/input-voltage"));
  EXPECT_TRUE(changeTouchesQueries(q, b));
}

TEST(ChangeTouchesQueries, KeyOmittedListMatchesRemovedKeyedBranch) {
  // Hot-plug remove: detachSubtree -> removedPrefixes[keyed branch].
  std::vector<CanonicalPath> q{cp("/components/component")};
  ChangeBatch b;
  b.removedPrefixes.push_back(cp("/components/component[name=PSC-0]/power-supply"));
  EXPECT_TRUE(changeTouchesQueries(q, b));
}

TEST(ChangeTouchesQueries, KeyOmittedQueryDescendingPastListElementMatches) {
  // The query omits the key AND descends past the list element.
  std::vector<CanonicalPath> q{cp("/components/component/state")};
  ChangeBatch b;
  b.changed.push_back(changedLeaf("/components/component[name=PSC-0]/state/name"));
  EXPECT_TRUE(changeTouchesQueries(q, b));
}

TEST(ChangeTouchesQueries, KeyedQueryDoesNotMatchOtherSlot) {
  // A per-slot query carries a key, so the fan-out does NOT apply: a change on a
  // DIFFERENT slot must not wake it.
  std::vector<CanonicalPath> q{cp("/components/component[name=PSC-0]")};
  ChangeBatch b;
  b.changed.push_back(changedLeaf("/components/component[name=PSC-1]/state/temperature"));
  EXPECT_FALSE(changeTouchesQueries(q, b));
}
