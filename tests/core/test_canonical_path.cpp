#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "canonical_path.hpp"
#include "leaf_id.hpp"

using namespace gnmid::core;

namespace {

std::string canon(std::string_view raw) { return canonicalize(raw).str(); }

}  // namespace

// --- Canonicalisation rules (D16) -----------------------------------------

TEST(CanonicalPath, StripsTrailingSlashExceptRoot) {
    EXPECT_EQ(canon("/a/b/c/"), "/a/b/c");
    EXPECT_EQ(canon("/a/b/c"), "/a/b/c");
    EXPECT_EQ(canon("/"), "/");
    EXPECT_EQ(canon(""), "/");        // empty path is root
    EXPECT_EQ(canon("//a//b/"), "/a/b");  // repeated separators collapse
}

TEST(CanonicalPath, StripsPredicateQuotes) {
    EXPECT_EQ(canon("/a/b[name=\"eth0\"]/c"), "/a/b[name=eth0]/c");
    EXPECT_EQ(canon("/a/b[name=eth0]/c"), "/a/b[name=eth0]/c");
    // Quotes only matter inside predicates; depth tracking leaves '/' literal there.
    EXPECT_EQ(canon("/a/b[path=\"/x/y\"]/c"), "/a/b[path=/x/y]/c");
}

TEST(CanonicalPath, SortsPredicateKeys) {
    EXPECT_EQ(canon("/a/b[k2=v2][k1=v1]/c"), "/a/b[k1=v1][k2=v2]/c");
    EXPECT_EQ(canon("/a/b[k1=v1][k2=v2]/c"), "/a/b[k1=v1][k2=v2]/c");
}

// Scenario 7 — one node, many spellings, one canonical key.
TEST(CanonicalPath, Scenario7AllSpellingsCanonicaliseEqual) {
    const CanonicalPath base = canonicalize(
        "/components/component[class=POWER_SUPPLY][name=psu0]/state/temperature");
    const CanonicalPath reordered = canonicalize(
        "/components/component[name=\"psu0\"][class=POWER_SUPPLY]/state/temperature");
    EXPECT_EQ(base, reordered);
    EXPECT_EQ(base.str(),
              "/components/component[class=POWER_SUPPLY][name=psu0]/state/temperature");
}

// --- Escape-aware predicate values ----------------------------------------

TEST(CanonicalPath, EscapedValueContainingBracket) {
    // value "a]b": quoted form and backslash-escaped form are the same node.
    const std::string quoted = canon("/a/b[name=\"a]b\"]/c");
    const std::string escaped = canon("/a/b[name=a\\]b]/c");
    EXPECT_EQ(quoted, escaped);
    // ']' is re-escaped in the canonical form so bracket scanning stays sound.
    EXPECT_EQ(quoted, "/a/b[name=a\\]b]/c");
}

TEST(CanonicalPath, EscapedValueContainingQuote) {
    // value a"b: quoted-with-escaped-quote and bare form are the same node.
    EXPECT_EQ(canon("/a/b[name=\"a\\\"b\"]"), canon("/a/b[name=a\"b]"));
    EXPECT_EQ(canon("/a/b[name=a\"b]"), "/a/b[name=a\"b]");  // '"' needs no escaping unquoted
}

TEST(CanonicalPath, EscapeAwareElementSplitAndAncestor) {
    // A '/' and ']' inside an escaped/quoted value must not be read as structure.
    const CanonicalPath p = canonicalize("/a/b[k=\"x/y]z\"]/c");
    EXPECT_EQ(p.str(), "/a/b[k=x/y\\]z]/c");
    const std::vector<std::string_view> anc = ancestorPrefixes(p);
    ASSERT_EQ(anc.size(), 2u);
    EXPECT_EQ(anc[0], "/a/b[k=x/y\\]z]");  // longest first; predicate kept intact
    EXPECT_EQ(anc[1], "/a");
}

// --- Element-aligned isUnderPrefix (D16) ----------------------------------

TEST(IsUnderPrefix, CoversNodeAndProperDescendants) {
    EXPECT_TRUE(isUnderPrefix(canonicalize("/a/b"), canonicalize("/a/b")));
    EXPECT_TRUE(isUnderPrefix(canonicalize("/a/b"), canonicalize("/a/b/c")));
}

TEST(IsUnderPrefix, RejectsStringPrefixThatIsNotElementBoundary) {
    EXPECT_FALSE(isUnderPrefix(canonicalize("/a/b"), canonicalize("/a/bc")));
}

TEST(IsUnderPrefix, BareListDoesNotCoverKeyedEntry) {
    // /a/b (the bare list) and /a/b[name=x] (a list entry) are different nodes.
    EXPECT_FALSE(isUnderPrefix(canonicalize("/a/b"), canonicalize("/a/b[name=x]/c")));
    EXPECT_TRUE(isUnderPrefix(canonicalize("/a/b[name=x]"),
                              canonicalize("/a/b[name=x]/c")));
}

TEST(IsUnderPrefix, RootCoversEverything) {
    EXPECT_TRUE(isUnderPrefix(canonicalize("/"), canonicalize("/a/b")));
    EXPECT_TRUE(isUnderPrefix(canonicalize("/"), canonicalize("/")));
}

// --- Longest-ancestor walk (D3) -------------------------------------------

TEST(AncestorPrefixes, LongestFirstExcludingRoot) {
    const CanonicalPath p = canonicalize("/a/b/c");
    const std::vector<std::string_view> anc = ancestorPrefixes(p);
    ASSERT_EQ(anc.size(), 2u);
    EXPECT_EQ(anc[0], "/a/b");
    EXPECT_EQ(anc[1], "/a");
}

TEST(AncestorPrefixes, SlashInsidePredicateIsNotABoundary) {
    const CanonicalPath p = canonicalize("/a/b[name=x/y]/c");
    const std::vector<std::string_view> anc = ancestorPrefixes(p);
    ASSERT_EQ(anc.size(), 2u);
    EXPECT_EQ(anc[0], "/a/b[name=x/y]");
    EXPECT_EQ(anc[1], "/a");
}

// --- Malformed predicates throw -------------------------------------------

TEST(CanonicalPath, MalformedPredicatesThrow) {
    EXPECT_THROW(canonicalize("/a/b[name]"), std::invalid_argument);      // no '='
    EXPECT_THROW(canonicalize("/a/b[name=eth0"), std::invalid_argument);  // no ']'
    EXPECT_THROW(canonicalize("/a/b[k=1][k=2]"), std::invalid_argument);  // repeated key
}

// --- Routing / list fan-out (ownsPath / selects) --------------------------

TEST(Selects, OwnsPathTreatsKeyBracketAsBoundary) {
    EXPECT_TRUE(ownsPath("/components/component",
                         "/components/component[name=PSC-0]"));            // '[' boundary
    EXPECT_TRUE(ownsPath("/components/component",
                         "/components/component[name=PSC-0]/state/name")); // and deeper
    EXPECT_FALSE(ownsPath("/components/component", "/components/componentX"));
}

TEST(Selects, KeyOmittedQueryFansOutToKeyedEntries) {
    // No key in the query -> selects keyed entries of the same shape, even when the
    // query descends past the list element (where ownsPath alone would miss).
    EXPECT_TRUE(selects("/components/component",
                        "/components/component[name=PSC-0]/state/name"));
    EXPECT_TRUE(selects("/components/component/state",
                        "/components/component[name=PSC-0]/state/name"));
}

TEST(Selects, KeyedQueryDoesNotFanOut) {
    // A query carrying a key matches only its own element-aligned subtree.
    EXPECT_TRUE(selects("/components/component[name=PSC-0]",
                        "/components/component[name=PSC-0]/state/name"));
    EXPECT_FALSE(selects("/components/component[name=PSC-0]",
                         "/components/component[name=PSC-1]/state/name"));
}

// --- LeafId smoke (Phase 1.3) ---------------------------------------------

TEST(LeafId, DefaultIsInvalidAndEqual) {
    LeafId a;
    LeafId b;
    EXPECT_FALSE(a.valid());
    EXPECT_EQ(a, b);
    EXPECT_EQ(std::hash<LeafId>{}(a), std::hash<LeafId>{}(b));
}
