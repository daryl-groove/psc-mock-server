#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "canonical_path.hpp"
#include "gnmi.pb.h"
#include "leaf_entry.hpp"
#include "leaf_id.hpp"
#include "leaf_type.hpp"
#include "notification_group.hpp"

namespace gnmid::core {

// One leaf's value as captured by a read (D7/D17). effectiveType is resolved at
// snapshot time so the protocol layer need not re-query the live registry. `value`
// is a shared handle to an immutable version (nullptr = unset) — a refcount bump,
// not a protobuf deep copy.
struct LeafValueSnapshot {
    std::shared_ptr<const gnmi::TypedValue> value;
    int64_t                                 collectedNs   = 0;
    uint64_t                                changeSeq     = 0;  // change-detection token (D14)
    LeafType                                effectiveType = LeafType::Operational;
};

// Path-keyed (canonical string) snapshot; owned by the caller, no pointer escapes.
using LeafSnapshot = std::map<std::string, LeafValueSnapshot>;

// A group as the protocol layer needs it: identity, atomic flag, and the FULL
// member list (sorted, value-copied) for atomic monitored-set expansion (D13).
struct GroupView {
    std::string              name;
    std::string              prefix;
    bool                     atomic = false;
    std::optional<LeafType>  preferredType;
    std::vector<std::string> memberPaths;  // sorted canonical strings
};

// One-shot result of subscription setup (D13): the §2.4.2-expanded leaf set plus
// the distinct groups owning >=1 member under the query, taken under a SINGLE
// shared lock so the two are mutually consistent even under concurrent
// attach/detach. GroupView.memberPaths is the FULL member list (Scenario 2).
struct SubscriptionView {
    LeafSnapshot           leaves;
    std::vector<GroupView> groups;
};

// Declarative description of one hot-pluggable device branch (D12). attachSubtree
// adds the whole thing under one exclusive lock; detachSubtree removes it by prefix.
struct SubtreeSpec {
    struct GroupSpec {
        std::string             name;
        std::string             prefix;
        bool                    atomic = false;
        std::optional<LeafType> preferredType;
    };
    struct LeafSpec {
        std::string                     path;
        std::optional<LeafType>         type;
        std::optional<gnmi::TypedValue> initialValue;
    };
    std::vector<GroupSpec> groups;
    std::vector<LeafSpec>  leaves;
};

class LeafRegistry;  // fwd — ValueWriter holds a back-reference

// The sole value-write path (D6). A move-only RAII object that holds the registry's
// exclusive lock for its whole lifetime; set() is the only way to mutate a leaf's
// value, so "you must hold the lock to write" is a compile-time fact. Many set()
// calls under one writer is the atomic-coherence boundary (D6/D14, Scenario 6).
class [[nodiscard("store the writer or the lock is immediately released")]] ValueWriter {
public:
    // Sets the leaf's value + collection timestamp. Value-gated: only a real change
    // installs a NEW immutable version (D17) and bumps changeSeq (D14); re-pushing an
    // unchanged value is a no-op. Returns false if the id is stale (leaf detached) —
    // a clean miss, never UB.
    bool set(const LeafId& id, gnmi::TypedValue value, int64_t collectedNs);

    ~ValueWriter()                             = default;  // releases the lock
    ValueWriter(ValueWriter&&)                 = default;  // movable: returned by value
    ValueWriter& operator=(ValueWriter&&)      = default;
    ValueWriter(const ValueWriter&)            = delete;   // two owners would double-release
    ValueWriter& operator=(const ValueWriter&) = delete;

private:
    friend class LeafRegistry;
    explicit ValueWriter(LeafRegistry& reg, std::unique_lock<std::shared_mutex> lk)
        : reg_(&reg), lock_(std::move(lk)) {}

    LeafRegistry*                       reg_;
    std::unique_lock<std::shared_mutex> lock_;
};

// Central, sole owner of every LeafEntry and NotificationGroup in the process
// (D1). A single shared_mutex guards BOTH structure and values (D2): reads take a
// shared lock, all mutation (structural and value writes) takes the exclusive
// lock. No internal pointer ever escapes — reads return value copies plus shared
// value handles.
class LeafRegistry {
public:
    LeafRegistry() = default;

    // Non-copyable, non-movable: leaves and groups hold raw pointers into this
    // object's maps; moving the registry would dangle every back-pointer.
    LeafRegistry(const LeafRegistry&)            = delete;
    LeafRegistry& operator=(const LeafRegistry&) = delete;

    // --- Structural mutation (exclusive lock; throws std::invalid_argument on
    //     misconfiguration — duplicate name/path, overlapping prefix) ---

    // Registers a group, enforcing the D5 non-overlapping-prefix constraint.
    void registerGroup(const std::string& name, const std::string& prefix, bool atomic,
                       std::optional<LeafType> preferredType = std::nullopt);

    // Registers a leaf, canonicalizes its path, auto-assigns it to the matching
    // group if any (D3), and returns its opaque handle.
    LeafId registerLeaf(const std::string& path, std::optional<LeafType> type = std::nullopt,
                        std::optional<gnmi::TypedValue> initialValue = std::nullopt);

    // Adds a whole device branch under one exclusive lock (D12) — a concurrent
    // reader sees none of it or all of it, never a partial branch. Returns the
    // leaves' handles. A malformed spec (duplicate/overlap) throws.
    std::vector<LeafId> attachSubtree(const SubtreeSpec& spec);

    // Removes a whole branch atomically (D12): every leaf under prefix (unlinked
    // from its group first, D1 erase order) and every group whose prefix is under
    // it. Held LeafIds for the branch go stale (a later set() returns false).
    void detachSubtree(const std::string& prefix);

    // Removes one leaf; detaches it from its group first (D1 erase order). No-op if
    // absent.
    void unregisterLeaf(const std::string& path);

    // Removes one group; its members survive as ungrouped independent units (D9).
    void unregisterGroup(const std::string& name);

    // --- Reads (shared lock; value copies, no pointer escapes) ---

    std::optional<LeafValueSnapshot> getLeaf(const std::string& path) const;
    std::optional<LeafValueSnapshot> getLeaf(const LeafId& id) const;
    std::optional<GroupView>         getGroup(const std::string& name) const;

    // --- Advisory pre-checks for the D5 prefix constraint (shared lock, read-only) ---
    //
    // These let a caller / tooling / test SEE a prefix conflict early and clearly,
    // instead of discovering it only when registerGroup throws. They are ADVISORY:
    // the authoritative, race-free guard remains registerGroup itself, which
    // re-checks under the exclusive lock at insert time. A wouldConflict() answer can
    // go stale before a later registerGroup (another thread may register in between),
    // so it informs — it does not replace the throw.

    // All registered group prefixes, sorted (canonical strings).
    std::vector<std::string> registeredPrefixes() const;

    // If registering `prefix` would violate D5 (one prefix being a path-ancestor-or-
    // equal of the other), returns the existing prefix it would clash with; nullopt if
    // free. Sibling prefixes (e.g. /a/b and /a/bc) do not conflict.
    std::optional<std::string> wouldConflict(const std::string& prefix) const;

    // Snapshots every leaf under prefix (§2.4.2 subtree boundary), shared lock,
    // returned by value (D7). The poll/SAMPLE hot path; values are shared COW
    // handles (D17), so this copies pointers, not protobufs.
    LeafSnapshot collectLeaves(const std::string& prefix) const;

    // One-shot subscription setup (D13): the expanded leaf set AND the owning groups
    // under one shared lock, so the two cannot disagree under a concurrent detach.
    SubscriptionView collectForSubscription(const std::string& query) const;

    // Acquires the exclusive lock and returns the sole value-write handle (D6).
    ValueWriter writeValues();

private:
    friend class ValueWriter;

    // The actual value write, performed while the caller's ValueWriter holds the
    // exclusive lock (so it does NOT re-lock). Value-gated changeSeq bump (D14/D17).
    bool setValueLocked(const LeafId& id, gnmi::TypedValue value, int64_t collectedNs);

    // Register one group/leaf assuming the exclusive lock is already held — the
    // shared core of register*/attachSubtree, so a whole branch is one lock (D12).
    void   registerGroupLocked(const std::string& name, const std::string& prefix, bool atomic,
                               std::optional<LeafType> preferredType);
    LeafId registerLeafLocked(const std::string& path, std::optional<LeafType> type,
                              std::optional<gnmi::TypedValue> initialValue);

    // Deref-comparator for the shared-path key, with transparent lookup from a
    // freshly canonicalize()d CanonicalPath (no shared_ptr allocation to look up).
    struct DerefLess {
        using is_transparent = void;
        bool operator()(const std::shared_ptr<const CanonicalPath>& a,
                        const std::shared_ptr<const CanonicalPath>& b) const noexcept {
            return *a < *b;
        }
        bool operator()(const std::shared_ptr<const CanonicalPath>& a,
                        const CanonicalPath& b) const noexcept { return *a < b; }
        bool operator()(const CanonicalPath& a,
                        const std::shared_ptr<const CanonicalPath>& b) const noexcept { return a < *b; }
    };

    // Transparent comparator so the prefix index can be probed by a string_view
    // ancestor slice (from ancestorPrefixes) without building a CanonicalPath.
    struct PrefixLess {
        using is_transparent = void;
        bool operator()(const CanonicalPath& a, const CanonicalPath& b) const noexcept { return a < b; }
        bool operator()(const CanonicalPath& a, std::string_view b) const noexcept { return a.str() < b; }
        bool operator()(std::string_view a, const CanonicalPath& b) const noexcept { return a < b.str(); }
    };

    // Returns the single group whose prefix is a path-ancestor of `path`, or
    // nullptr. Probes ancestor prefixes longest-first (D3); D5 guarantees at most
    // one match. Caller holds the lock.
    NotificationGroup* findOwningGroup(const CanonicalPath& path);

    // Returns the existing group prefix that overlaps `prefix` under D5 (one being a
    // path-ancestor-or-equal of the other), or nullptr if none. Caller holds a lock.
    // Shared by registerGroup's enforcement and wouldConflict's advisory check.
    const CanonicalPath* overlappingPrefix(const CanonicalPath& prefix) const;

    LeafValueSnapshot snapshotOf(const LeafEntry& leaf) const;
    GroupView         viewOf(const NotificationGroup& group) const;

    // Primary leaf store — node-based std::map for pointer stability (D1), keyed on
    // the shared CanonicalPath handle (L=B): the map key IS the leaf's path handle.
    std::map<std::shared_ptr<const CanonicalPath>, LeafEntry, DerefLess> leaves_;
    std::map<std::string, NotificationGroup>                          groups_;          // by name
    std::map<CanonicalPath, NotificationGroup*, PrefixLess>           groupsByPrefix_;  // by prefix

    uint64_t                  globalSeq_ = 0;  // registry-global monotonic change token (D14)
    mutable std::shared_mutex mutex_;
};

}  // namespace gnmid::core
