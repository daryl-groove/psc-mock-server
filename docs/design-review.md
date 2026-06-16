# Core Data Model — Design Review

Reviewer pass over the settled design (`docs/core-data-model-design.md`) and its
current implementation (`include/`, `src/`, `tests/`), checked against the gNMI
specification (`docs/spec/gnmi-specification.md`, v0.11.0).

Scope: the core data layer only — `LeafEntry`, `NotificationGroup`,
`LeafRegistry`. The protocol/provider layers are out of scope except where the
core must expose enough for them.

State at review time: all 6 checklist phases marked `[x]`; 5 test suites build
and pass (`meson test` → 5/5 OK, includes the concurrency tests).

---

## Review Criterion / Decision Lens

The maturity goals are: **easy to understand, low redundancy, low coupling, easy to
use, easy to extend, easy to test.** These rest on one premise — **do not design for
design's sake.** Generality the real situation does not call for is itself a defect,
not maturity; decisions are judged against the *actual* trajectory, not abstract
reusability.

The actual situation here is a gNMI **mock** for an ORv3 PSC card that is **intended
as the foundation for a future real product.** Decisions therefore weigh the
production trajectory — but only where deferring is not free.

The test that reconciles the two — **one-way vs two-way doors:**

- **One-way door** — foundational, expensive to change later: the identity / data
  model, the concurrency model, the cross-layer event substrate. Design these for the
  real trajectory **now**, because retrofitting ripples through everything.
- **Two-way door** — additive, localized, hideable behind a stable API. Build the
  minimal version now, keep the seam, and **do not pre-build.** Pre-building a
  two-way door *is* designing for design's sake.

Severity reads against this lens: 🔴 is usually a one-way door or a correctness
defect; 🟡 is usually a two-way-door reminder. Each finding's fix is sized by which
door it is.

> **Status update (2026-06-15).** After this review, the design was revised in
> `core-data-model-design.md`. Findings are now tracked with a status:
> ✅ **resolved** (decided & written into the design, implementation pending),
> 🟩 **decided** (chosen, but not yet written into the design doc),
> 🔜 **next** (the active decision), ⬜ **open** (queued). The *implementation*
> still reflects the pre-revision design — these statuses track the **design
> doc**, which is the source of truth the code will be brought to. Summary:
>
> | Finding | Status | Resolved by (design decision) |
> |---|---|---|
> | A — runtime schema mutability vs two-phase | ✅ resolved | D2 revised → single rwlock; D12 subtree ops |
> | B — write path unenforced / D2↔D6 contradiction | ✅ resolved | D6 revised → `ValueWriter` sole write path |
> | H — copyable aggregates break back-pointer invariant | ✅ resolved | D11 encapsulated, non-copyable types |
> | E — doc↔code drift on auto-assign | ✅ resolved | D3 mechanism corrected in doc |
> | F — "decoupled from gNMI" overstated | ✅ resolved | Overview wording softened |
> | (A-residue) two-lock setup TOCTOU | ✅ resolved | D13 merged `collectForSubscription` |
> | C — change-detection contract unstated | ✅ resolved | D14 (`changeSeq` token, key-set diff, delete granularity, timestamp collapse) |
> | G — per-leaf ns → per-Notification timestamp | ✅ resolved | D14 timestamp-collapse (non-atomic own / atomic max) — ⚠ a non-atomic group on a SAMPLE/whole-set send is not in this enumeration (Scenario 5 uses sample time); fold into D14 |
> | (new) non-atomic group ON_CHANGE semantics | ✅ resolved | D15 group/bundling guidance + Scenarios 5–6 |
> | D — path normalization incomplete | ✅ resolved | D16 canonical-string `normalizePath` (key order, trailing slash, quotes) |
> | I — flat key drops `origin` | ✅ resolved | D16 O2 — single-origin core, origin defaulted/validated at boundary — ⚠ residue **N** (reject code). Origin-in-key (O1/O2) is **deferrable**: core stays string-key + canonicalize-at-boundary, so keep origin out of core and decide O1/O2 at the protocol/provider boundary; N rides with it |
> | K — `collectLeaves` hot-path deep copy | ✅ resolved | D17 COW value (`shared_ptr<const TypedValue>`): snapshot copies a handle, not a protobuf |
> | J — test coverage gaps | ⬜ open | after impl; add boundary tests: watermark race (P), push-payload lifetime (L), origin reject code (N), atomic partial subscription (S-P3-c), not-yet-existent arming (S-P4-b), wildcard (Q, if scoped in) |
> | L — push payload lifetime: `const CanonicalPath*` + `std::span` dangle across threads / after unlock | 🟩 decided → **B** | one-way door (event substrate). `path` → `shared_ptr<const CanonicalPath>` (mirrors D17 value), batch → `shared_ptr<const vector<LeafChange>>`: zero-copy + refcount-safe. core D6 / protocol P1 / P3-Fork1 doc edit pending |
> | Q — wildcard paths unhandled; D16 element-aligned match excludes `[name=*]` | 🔴 open | §3.3.1 L1068, §3.4.6 L1443. One-way-ish (matching, not storage); scope in/out by whether real clients use wildcards |
> | M — TARGET_DEFINED lifecycle "Initial: —" drops mandatory initial sync + sync_response | ✅ resolved | §3.5.1.5.2 L1751-1752 / §3.5.2.3 L1875-1880; per-mode table row now reads "send all + `sync_response`" (protocol-layer-design.md) |
> | N — origin rejected with InvalidArgument; spec points to UNIMPLEMENTED | ✅ resolved | §3.3.4 L1152 / §3.5.2.4 L1900: unknown-but-valid origin → `UNIMPLEMENTED` (malformed path stays `INVALID_ARGUMENT`); written into protocol-layer-design.md seam note + RPC status table |
> | P — watermark advance "current global high-water" underspecified → possible silent miss | ✅ resolved | §3.5.1.5.2; P3 Fork4 tightened to "max-of-sent / high-water captured under the snapshot lock" (protocol-layer-design.md) |
> | O — SHOULD read as MUST; several "spec-forced" items are spec-permitted choices | ✅ resolved | §3.5.1.5.2 L1790-1792 (SHOULD reject) and §3.5.2.5 L1907 SHOULD / L1925 MAY now labelled as spec-permitted choices, not "spec-forced" (protocol-layer-design.md P3/P5) |
> | R — Notification Builder omits `target` echo (MUST) | ✅ resolved | §2.2.2.1 L414-424; "`prefix.target` MUST be echoed" added to the Notification-prefix section (protocol-layer-design.md) |
> | S — atomic single-member delete / delete-prefix rule uncovered | ✅ resolved | §2.1.1 L281-285; delete-aligns-to-atomic-container-prefix rule added to P3 spec-forced list (protocol-layer-design.md) |
> | T — Resolver missing use_models / UNIMPLEMENTED-path / YANG-default mechanism | 🟡 open | §2.6 L819-826 (MUST) / §3.5.2.4 / §3.5.2.3 L1870-1873 |
>
> **Second pass (2026-06-15 — protocol layer + criterion re-alignment).** After
> `protocol-layer-design.md` (P1–P5) was written, a second cold-read pass found
> findings **L–T** above. Most are protocol-layer issues outside the original
> core-only scope; a few (N, S) critique the revisions themselves (O2, D14); **none
> reopen A–K.** Re-examined under the door lens for a **staged build (core first,
> protocol later)**: the only decision that blocks the core implementation is its
> **storage representation** — **L** (decided → B: path as `shared_ptr<const
> CanonicalPath>`, shared zero-copy into the later event seam). Everything else is
> deferrable — **D16 O1/O2** (origin stays out of the core; string-key +
> canonicalize-at-boundary makes it a boundary decision, drives N), **Q** (wildcard,
> protocol-stage matching), and the standing two-way doors (D17 change-log, **D2**
> lock granularity, **P4** structural index, **P5** per-leaf interval, **P2** T2/T3).
> Severities use the 🔴/🟠/🟡 scale from Findings.

The narrative below is the **original review** (pre-revision), kept for context.
Read the table above for current status.

---

## Verdict

The greenfield design is **mature in structure and clearly worth keeping**. It
resolves all five problems that motivated the rewrite (scattered atomic logic,
no cross-provider groups, ON_CHANGE blind to unsubscribed atomic members, no
prefix-overlap enforcement, manual membership). The two load-bearing spec
interpretations — §2.4.2 container expansion and §3.5.2.5 atomic-scoped-to-
subscription — are implemented correctly and covered by an integration test.

The weaknesses cluster around **one root cause**: the core's most important
invariants are documented in prose but **not enforced by the type system or
API** — plus a few places where the flat-string-path key model leaks the
structured-`Path` semantics gNMI actually defines.

### Scorecard

| Dimension              | Maturity | Open items |
|------------------------|----------|-----------|
| Structure / layering / readability | High | D2↔D6 wording contradiction (B); doc↔code drift (E) |
| Testability            | High   | A few boundary cases untested (J) |
| Invariant safety       | Medium | value-write, structural mutation, and struct copy all bypass invariants (A, B, H) |
| Change-detection contract | Medium | `collectedNs`-as-token unstated; delete & same-ns cases undecided (C, K) |
| Path / spec equivalence | Medium | key-order, trailing slash, origin, atomic-timestamp collapse (D, G, I) |

---

## Findings

Severity: 🔴 design-level, decide before integration · 🟠 contract gap, decide
soon · 🟡 cleanup / robustness.

### 🔴 A — D2's "schema immutable at runtime" assumption vs gNMI's dynamic nature

The entire locking model (separate shared locks for `collectLeaves` /
`getGroupsForPrefix`, lock-free diff between two snapshots) rests on `leaves_`
and `groups_` being structurally immutable during the runtime phase. But
`registerLeaf` (`src/leaf_registry.cpp`) and `unregisterLeaf` take **no lock at
all**, and real gNMI servers add/remove leaves at runtime (Set creating a list
entry, an interface going up/down). A runtime structural mutation racing a
`collectLeaves` is a `std::map` data race + iterator invalidation (UB).

The class cannot guard its single most important invariant. Decide one:
- **(a) schema is fully static** → add a `freeze()` latch; structural mutation
  after freeze throws. Turn the convention into a mechanism.
- **(b) schema mutates at runtime** → structural inserts/erases must take the
  exclusive lock, and D2's "structure immutable, lock-free" premise needs a
  rewrite.

### 🔴 B — "Who manages synchronization" is self-contradictory; the write path is unenforced

D2 says "thread safety is **managed inside the registry**"; D6 says "**upper
layer** manages synchronization." `registerLeaf` hands back a raw `LeafEntry*`
(D6) so a provider can do `entry->value = ...` and bypass `writeLock()`
entirely, racing the shared lock in `collectLeaves` with zero diagnostic.
`WriteLockGuard`'s `[[nodiscard]]`/move-only design is careful but **optional** —
`[[nodiscard]]` catches "acquired then dropped," not "never acquired." Consider
funnelling writes through a registry API (e.g. `setValue(LeafEntry*, value, ns)`
that locks and bumps the timestamp internally) so the correct path is the only
easy path.

### 🔴 H — The three types are freely-copyable aggregates that hold bidirectional pointers

`LeafEntry::group` ↔ `NotificationGroup::members` is a two-way back-pointer, but
both are plain copyable structs. Copying a `LeafEntry` yields a copy whose
`group` points at a group that does not list it in `members`; copying a
`NotificationGroup` yields members whose `group` still points at the original.
The "bidirectional consistency" invariant silently breaks under copy/move and
the type does nothing to stop it. The aggregate/designated-init decision (good
for tests) exposes an invariant that should be encapsulated. At minimum delete
copy on the two types, or document the constraint loudly.

### 🟠 C — Change-detection contract is unstated

The integration test detects ON_CHANGE via `snap.collectedNs !=
before.collectedNs` (`tests/test_integration_scenario.cpp`), but "`collectedNs`
is the change token, and every value write MUST bump it" is **never stated in
the design** — it lives only inside a test. Risks: (1) a writer updates `value`
but forgets to bump `collectedNs` → missed change; (2) two updates within the
same clock tick → undetectable; (3) the documented diff only scans keys present
in `after`, so **deletions** (in `before`, absent from `after`) are not caught,
and how a delete becomes a gNMI `delete` notification is undecided. Nail the
"change = compare what" contract in the design.

### 🟠 G — No defined collapse from per-leaf `collectedNs` to per-Notification timestamp

A gNMI `Notification` carries a single notification-level `timestamp`; its
`update` entries have none. The core stores one `collectedNs` per leaf. When an
atomic group re-sends several subscribed members in one Notification, their
`collectedNs` differ — which one is THE timestamp? Spec §2.1.1 frames an atomic
notification as the complete state "at a given timestamp," so this matters.
Decide the rule (e.g. `max(collectedNs)` over included members, or a group-level
last-changed stamp) — the per-leaf model does not obviously produce it.

### 🟠 D — Path normalization is incomplete (D10 claims canonical authority, handles only quotes)

`normalizePath` (`src/leaf_registry.cpp`) only strips predicate double-quotes.
The structured-`Path` node equality gNMI defines also covers:
- **key order**: `/a/b[k1=v1][k2=v2]` and `/a/b[k2=v2][k1=v1]` are the *same*
  node but distinct strings → two map keys → two leaves; a client reordering
  keys misses.
- **trailing slash**: a group prefix `/a/b/` makes `isUnderPrefix` reject
  `/a/b/c` (next char is `c`, not `/`) → auto-assign silently fails — exactly the
  "silent mismatch" D10 claims to eliminate.

Either extend normalization (canonicalise key order, strip trailing slash) or
write an ADR stating explicitly what is and isn't canonicalised.

### 🟡 I — The flat-string key drops gNMI Path's `origin` dimension

A gNMI `Path` has an `origin` (e.g. `openconfig`). The string-path key has
nowhere to put it, so same-path/different-origin nodes collide. Fine for a
single-origin mock, but record the assumption alongside D/key-order.

### 🟡 E — Doc ↔ code drift on auto-assign

The design (and the `groupsByPrefix_` comment in `leaf_registry.hpp`) still
describes the single `upper_bound` auto-assign step, but `findOwningGroup`
correctly walks `/` boundaries longest-first — the code is *more* correct than
the doc (the checklist 3.3 note explains why `upper_bound` misses a dashed
sibling like `/a/b < /a/b-x < /a/b/c`). Update the doc to match.

### 🟡 F — "Decoupled from gNMI" is overstated

The core `#include`s `gnmi.pb.h` and stores `gnmi::TypedValue` directly in
`LeafEntry`/`LeafValueSnapshot`. It is decoupled from providers and the gRPC
transport, but not from gNMI's value encoding. Abstracting the value type would
be over-engineering for a mock; just soften the wording.

### 🟡 J — Test coverage gaps (suite is green, but boundaries are open)

- No `registerGroup`-level test that `/a/b` and `/a/bc` groups may coexist
  (string-prefix but not path-ancestor); only the `addLeaf` level covers it.
- `normalizePath` untested on nested/escaped quotes (`[name="a\"b"]`).
- `collectLeaves` untested on an empty registry / root `/` (real for a root Get).

### 🟡 K — `collectLeaves` deep-copies every value on the poll hot path

`collectLeaves` copies each `value` (`optional<TypedValue>`) into the snapshot,
yet the actual ON_CHANGE diff (per C) compares only `collectedNs`. Two full
snapshots per subscription per interval copy a lot of `TypedValue` to compare
timestamps. Acceptable for a mock; the obvious optimisation (lightweight
`collectedNs`-only diff, fetch values on hit) should be designed together with C.

---

## Mapping to the design decisions

| Decision | Assessment | Finding |
|----------|-----------|---------|
| D1 Registry owns all entries; `std::map` pointer stability | Correct, well documented | — |
| D2 Two-phase thread safety | Premise unenforced; clashes with dynamic schema | A, B |
| D3 Auto leaf→group assignment | Correct; impl better than doc | E |
| D4 Group name as unique key | Fine | — |
| D5 Non-overlapping prefix | Correctly enforced at `registerGroup` | J (coverage) |
| D6 Value update via stable pointer | Bypasses lock; contradicts D2 | B |
| D7 Snapshot via value copy | Correct; copy cost on hot path | K |
| D8 Type lazy-init via group | Correct, tested | — |
| D9 Ungrouped leaves | Correct, tested | — |
| D10 Path normalization at boundary | Only quotes handled | D, I |

---

## Suggested priority

1. **A / B / H** — turn the core invariants into mechanisms (freeze latch,
   registry-owned write API, non-copyable types). Same root cause.
2. **C** — state the change-detection contract; cover delete + same-ns.
3. **D / G / I** — one ADR on path/timestamp/origin equivalence and what the
   mock assumes.
4. **E / J / K** — doc alignment, boundary tests, hot-path note.

The static-vs-dynamic-schema question in A is a product decision that shapes how
A/B/H are implemented; settle it before acting on item 1.
