# psc-core Implementation Checklist

> **Rewritten 2026-06-15** for the post-revision core design. Supersedes the
> pre-revision checklist (old design: public aggregates, two-phase locking,
> raw-pointer returns, quotes-only normalize). **All boxes reset** — the current
> `include/`/`src/` is the OLD design and is being brought to the new one (see the
> drift inventory in `docs/design-review.md`).

Design reference: `docs/core-data-model-design.md` (D1–D17).

Locked decisions **not yet folded into that design doc** (the gap to reconcile
right after this checklist):
- **L = B** — `path` stored as `shared_ptr<const CanonicalPath>`, shared
  zero-copy (mirrors D17's value handle); the later change-event batch becomes a
  `shared_ptr<const vector<LeafChange>>`. Also removes the current key/`path_`
  duplication.

Pending decisions (**Phase 0**): namespace/naming; `canonicalize` placement.

Scope: **core only** (leaf / group / registry). The protocol layer (ILeafSink
event seam, wildcard matching, P1–P5) is a later stage — see **Deferred**.

Build: `meson setup build && cd build && ninja`
Test:  `cd build && meson test -v`
Rule: each phase must have passing unit tests before the next.

---

## Phase 0 — Naming & utils decisions (LOCKED)

- [x] 0.1 **Namespace + project/lib name** — **DECIDED: `gnmid`** (gNMI server;
        gNMI-rooted, does **not** shadow proto `gnmi::`, product-neutral like
        `sshd`/`httpd`). This core layer is **`gnmid::core`**. Apply repo-wide:
        namespace, meson `project()` (`gnmid`) / `static_library()` / dep names,
        and replace every `psc` in comments.
- [x] 0.2 **Path-utils placement** — **DECIDED: extract** `CanonicalPath` +
        `canonicalize()` + prefix/ancestor predicates into `canonical_path.hpp/.cpp`,
        with a free `canonicalize()` factory as `CanonicalPath`'s friend (invariant:
        only it can build a key). Removes the `isAncestorOrEqual`/`isUnderPrefix`
        duplication; `canonicalize` is **no longer** a `LeafRegistry` method.

---

## Phase 1 — Foundational value types

- [x] 1.1 `leaf_type.hpp` — **keep** as-is (only namespace rename). *[carry-over]*
- [x] 1.2 `canonical_path.hpp/.cpp` — **NEW** (D16, utils)
        - `CanonicalPath`: wraps the canonical string; private ctor;
          `operator<` / `==` / hash
        - `canonicalize(string_view) → CanonicalPath`: strip trailing slash,
          strip predicate quotes (escape-aware), sort `[key=val]` predicates by key
        - predicates on the canonical form: `isUnderPrefix` / ancestor-or-equal,
          longest-ancestor walk, bracket-depth scan (dedup'd from the old two copies)
        - **single-origin invariant**: core takes origin-less canonical strings
          (O2). O1/O2 stays a boundary decision — keep `origin` OUT of here.
- [x] 1.3 `leaf_id.hpp` — **NEW** (D1)
        - opaque handle holding `shared_ptr<const CanonicalPath>` (rides L=B);
          `valid()`, defaulted `==`, hash; only the registry mints one
- [x] 1.4 `tests/test_canonical_path.cpp` — **NEW**
        - quotes / trailing-slash / key-order all canonicalise equal (Scenario 7)
        - element-aligned `isUnderPrefix`: `/a/b` covers `/a/b/c`, not `/a/bc`,
          not bare-list `/a/b` vs keyed `/a/b[...]`
        - escaped predicate value containing `]` / `"`

---

## Phase 2 — Core entities (encapsulated)

- [x] 2.1 `leaf_entry.hpp` — **REWRITE** (D11, D17, L=B, D14)
        - encapsulated class, non-copyable; registry-friend private ctor
        - fields: `shared_ptr<const CanonicalPath> path_`;
          `shared_ptr<const gnmi::TypedValue> value_` (nullptr = unset);
          `optional<LeafType> type_`; `int64 collectedNs_`;
          `uint64 changeSeq_`; `NotificationGroup* group_`
        - const accessors; `effectiveType()` (D8)
- [x] 2.2 `notification_group.hpp/.cpp` — **REWRITE** (D11)
        - encapsulated, non-copyable; `linkLeaf` / `unlinkLeaf` **private,
          registry-only** (no public addLeaf/removeLeaf)
        - under-prefix check uses the canonical_path predicate (no local copy)
- [x] 2.3 `tests/test_notification_group.cpp` — rewrite to new API
        - link/unlink only via registry; effectiveType priority; sort before
          asserting member order

---

## Phase 3 — Registry structure + concurrency

- [x] 3.1 `leaf_registry.hpp/.cpp` skeleton — **REWRITE** (D1, D2)
        - storage: `std::map<shared_ptr<const CanonicalPath>, LeafEntry, DerefLess>`
          (node-stable, shared-path key); `std::map<string, NotificationGroup>`;
          `groupsByPrefix_`
        - **single `shared_mutex`**: reads shared, **all** mutation exclusive (D2)
          — replaces the two-phase model
- [x] 3.2 `registerGroup` / `registerLeaf` — exclusive lock; canonicalize at the
        boundary; enforce D5 overlap; D3 auto-assign via the canonical_path
        longest-ancestor walk; **return `LeafId`** (no raw pointer)
- [x] 3.3 `getLeaf` / `getGroup` — shared lock; return
        `optional<LeafValueSnapshot>` / `optional<GroupView>` (value copies, no
        pointer escape)
- [x] 3.4 `tests/test_leaf_registry_registration.cpp` — rewrite
        - auto-assign; ungrouped leaf; D5 overlap throws; canonical equivalence;
          `/a/b` & `/a/bc` groups coexist (finding J)

---

## Phase 4 — Values & change detection

- [x] 4.1 `ValueWriter` — **NEW** (D6) the sole write path
        - `writeValues()` → move-only RAII writer holding the exclusive lock
        - `set(LeafId, value, collectedNs)`: value-gated — install a new COW
          version + bump `changeSeq` **only on a real change** (D14, D17);
          stale id → returns `false` (no UB)
- [x] 4.2 `collectLeaves(prefix)` — shared lock; `LeafValueSnapshot{shared_ptr value,
        collectedNs, changeSeq, effectiveType}` (D7/D17); §2.4.2 subtree boundary
- [x] 4.3 change-detection contract (D14) — document + test the changeSeq
        semantics and snapshot key-set diff (added / removed / changed).
        *(The push event seam is deferred — poll-diff only here.)*
- [x] 4.4 `tests/test_leaf_registry_values.cpp` — **NEW**
        - set bumps changeSeq only on real change; re-push same value = no-op
        - unset leaf skipped; an earlier snapshot still holds the old version
          after a later write (COW point-in-time)
        - one `writeValues` scope = atomic-group coherence (Scenario 6)
        - concurrency: shared/exclusive don't deadlock; two writers serialize

---

## Phase 5 — Subscription setup & structural ops

- [x] 5.1 `collectForSubscription(query)` — **NEW** (D13) single shared lock →
        `SubscriptionView{leaves, groups}` (`GroupView.memberPaths` = full member
        list, for atomic monitored-set expansion)
- [x] 5.2 `attachSubtree(SubtreeSpec)` / `detachSubtree(prefix)` — **NEW** (D12)
        exclusive lock; whole branch atomic (no partial); returns `LeafId`s /
        removes the branch (leaves + groups under it)
- [x] 5.3 `unregisterLeaf` / `unregisterGroup` — exclusive lock; D1 erase order
        (unlink before erase); D9 survivors become ungrouped
- [x] 5.4 `tests/test_leaf_registry_structural.cpp` — **NEW** (+ old deletion cases)
        - attach/detach seen all-or-nothing; stale `LeafId` set → false;
          collectForSubscription leaf/group consistency; unregister order + D9

---

## Phase 6 — Integration scenarios (new API)

- [x] 6.1 `tests/test_integration_scenario.cpp` — rewrite to the new API
        - Scenario 1 — §2.4.2 container expansion
        - Scenario 2 — §3.5.2.5 monitored set (atomic pulls in unsubscribed member)
        - Scenario 4 — hot-plug attach/detach branch
        - Scenario 6 — atomic coherence via one `writeValues` scope
        - Scenario 7 — canonical path equivalence

---

## Phase 7 — Build & cleanup

- [x] 7.1 `meson.build` — add new sources (`canonical_path.cpp`); register new
        test executables; **remove** `-Wno-missing-field-initializers` (no
        aggregate init after D11); apply the Phase-0 naming
- [x] 7.2 fix stale doc↔code comments (e.g. `groupsByPrefix_` still says
        "upper_bound"; `leaf_registry.hpp` header comment still says "two-phase")

---

## Deferred — protocol stage (NOT in this checklist)

- ILeafSink / ChangeBatch / LeafChange event seam (L implementation) + push
  routing (P1, P2)
- Notification builder (P3); monitored-set re-expansion (P4); TARGET_DEFINED (P5)
- wildcard path matching (Q); origin O1/O2 + reject code (N); watermark (P);
  target echo (R); atomic delete-prefix (S); Resolver MUSTs (M, O, T)

---

## Status Legend
- `[ ]` not started   `[~]` in progress   `[x]` done & tests passing
- *carry-over* = survives from the old impl with only the namespace change
