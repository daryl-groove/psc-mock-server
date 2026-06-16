# Core Data Model Design: Leaf, Group, Registry

## Overview

This document captures the settled design decisions for the core data layer
(`LeafEntry`, `NotificationGroup`, `LeafRegistry`) of the gNMI mock server
refactor.

This layer is intentionally **decoupled from providers and from the gRPC
transport**. It does depend on gNMI's *value* encoding (`gnmi::TypedValue`),
because the value a leaf holds is the gNMI-typed value — but it knows nothing
about providers, subscriptions, or the gRPC service. It can be built,
unit-tested, and validated independently before being integrated with any
provider or protocol handler.

> **Revision note (2026-06-15).** This document was substantially revised after
> an Opus design-review pass (see `docs/design-review.md`). The concurrency
> model changed from a two-phase "schema frozen at runtime" assumption to a
> single-phase reader/writer-locked store, because the target device (an ORv3
> PSC card exposing hot-pluggable PSU/BBU sub-devices) **adds and removes leaves
> at runtime**. The handle model, the write path, and type encapsulation changed
> accordingly. Revised/added decisions are marked **[revised]** / **[new]**.

---

## Background and Motivation

The original server stores leaves in per-provider `LeafStore` instances with no
shared registry. Five structural problems drove the redesign:

1. **Atomic group awareness scattered into providers.** The `atomicPrefix()`
   virtual on each provider was the sole source of group-boundary information.
   Every provider re-implemented atomic logic; there was no central place to
   enforce constraints or inspect group membership across providers.

2. **No cross-provider groups.** Because each `LeafStore` is per-provider,
   forming a notification group that spans two providers was impossible — even
   when the data semantically belongs together.

3. **ON\_CHANGE blind to unsubscribed atomic group members.** The existing
   `subscribe.cpp` ON\_CHANGE loop snapshots only the client's subscribed paths.
   If an *unsubscribed* leaf in the same atomic group changes, the snapshot
   misses it and no notification fires — violating the intent of atomic grouping
   (spec §3.5.2.5).

4. **No enforcement of the prefix non-overlapping constraint.** Nothing prevented
   two providers from declaring overlapping or identical atomic prefixes, which
   makes clients see spurious leaf deletions (spec §2.1.1 implicit-delete rule).

5. **Manual group membership.** Developers had to manually call `addLeaf()`.
   Forgetting it silently produced wrong atomic behaviour with no error.

The new design fixes all five with a single global `LeafRegistry`, explicit
`NotificationGroup` objects with enforced non-overlapping prefixes,
auto-assignment at `registerLeaf()` time, and a single reader/writer-locked
concurrency model.

---

## Development Strategy

**This is a greenfield design, not a refactor of the existing codebase.** The
existing `src/` is reference material only — to understand what behaviours must
be reproduced — not a base to extend or patch.

Build sequence:

1. **Core layer** — implement `LeafEntry`, `NotificationGroup`, `LeafRegistry`
   in isolation, no dependency on existing server code.
2. **Unit tests** — validate all core behaviours (registration, membership,
   prefix constraint, snapshot/diff, dynamic attach/detach, locking) with no
   provider or gRPC involved.
3. **Integration** — only after the core is stable, wire it into the server by
   replacing the current data layer. The provider interface and protocol
   handlers are designed separately on top of the validated core.

Do **not** start from `LeafStore` / `IDataProvider` / `DataProviderRegistry` and
extend them.

---

## Design Goals

1. Clean separation of concerns — the core knows nothing about providers or gRPC.
2. Independently testable — no provider, no gRPC needed.
3. Extensible — future providers add leaves/groups, and **hot-pluggable devices
   attach/detach whole subtrees at runtime**, without touching the core.
4. **Invariants enforced by mechanism, not by prose.** Where a rule matters
   (you must hold the lock to write; a handle must never dangle; the
   leaf↔group back-pointer must stay consistent), the *type system / API*
   enforces it — not a comment a caller can ignore. This principle drove most of
   the 2026-06-15 revisions (D11, D6, D1).

---

## Core Objects

### LeafId — the public handle (**[new]**, D1)

Callers (providers) never hold raw `LeafEntry*`. `registerLeaf()` returns a
`LeafId`: an opaque value handle. Internally it holds the leaf's
`shared_ptr<const CanonicalPath>` — the **same** handle the registry keys its map
on (L=B) — so resolving it is a direct map lookup with no string copy and no extra
data structure. It is a distinct type — not a bare pointer or string — for type
safety, and the representation can still change later (e.g. an interned integer)
without touching any call site.

```cpp
// leaf_id.hpp
#include "canonical_path.hpp"           // CanonicalPath (shared with the registry)
class LeafId {
public:
    LeafId() = default;                 // empty / "no leaf"
    bool valid() const noexcept { return path_ != nullptr; }
    // Equality + hashing so callers can key their own maps on a LeafId. Under the
    // L=B single-materialisation, one live leaf == one shared handle, so pointer
    // identity coincides with path identity.
    bool operator==(const LeafId&) const = default;

private:
    friend class LeafRegistry;          // only the registry mints a LeafId
    explicit LeafId(std::shared_ptr<const CanonicalPath> p) : path_(std::move(p)) {}
    const std::shared_ptr<const CanonicalPath>& path() const noexcept { return path_; }
    std::shared_ptr<const CanonicalPath> path_;  // SAME handle the registry keys on (L=B); opaque to callers
};
```

**Why a handle instead of a raw pointer.** With a *dynamic* schema (D2), a leaf
can be erased at runtime when its device is unplugged. A raw `LeafEntry*` cached
by a provider would then dangle (UB). A `LeafId` cannot dangle: resolving a stale
id simply fails cleanly (the write/read reports "not found"). This makes
handle-safety a *mechanism*, not a "don't keep pointers across a detach" comment
(Goal 4).

### LeafEntry — encapsulated, registry-owned (**[revised]**, D11)

Represents a single gNMI leaf node. Previously a public aggregate; now an
**encapsulated, non-copyable** class. The registry is its sole owner and the
only writer; outside code reads via const accessors and never holds a mutable
reference.

| Field          | Type                            | Meaning                                          |
|----------------|---------------------------------|--------------------------------------------------|
| `path_`        | `shared_ptr<const CanonicalPath>` | Canonical path materialised **once** & shared (map key, `LeafId`, change events) — zero-copy, refcount-safe (L=B / D16) |
| `value_`       | `shared_ptr<const gnmi::TypedValue>` | `nullptr` = declared but not yet set; COW versioned value (D17) |
| `type_`        | `optional<LeafType>`            | `nullopt` = defer to group/default               |
| `collectedNs_` | `int64_t`                       | Collection timestamp (ns since epoch)            |
| `group_`       | `NotificationGroup*`            | Owning group; `nullptr` if ungrouped (non-owning)|

```cpp
// leaf_entry.hpp
#include "canonical_path.hpp"           // CanonicalPath — stored as a shared handle
struct NotificationGroup;               // fwd decl — only a pointer is stored

class LeafEntry {
public:
    // Read-only views. effectiveType() resolves own type > group preferredType
    // > Operational (D8); defined out-of-line where NotificationGroup is complete.
    const CanonicalPath&                   path() const noexcept { return *path_; }
    // A shared handle to an immutable value version (nullptr = unset); a writer
    // swaps in a NEW version, so this handle never mutates underfoot (D17).
    std::shared_ptr<const gnmi::TypedValue> value() const noexcept { return value_; }
    int64_t                                collectedNs() const noexcept { return collectedNs_; }
    uint64_t                               changeSeq() const noexcept { return changeSeq_; }
    LeafType                               effectiveType() const noexcept;
    const NotificationGroup*               group() const noexcept { return group_; }

    LeafEntry(const LeafEntry&)            = delete;   // single-owner: never copied
    LeafEntry& operator=(const LeafEntry&) = delete;

private:
    friend class LeafRegistry;            // registry constructs, writes, and links
    LeafEntry(std::shared_ptr<const CanonicalPath> p, std::optional<LeafType> t,
              std::shared_ptr<const gnmi::TypedValue> v)
        : path_(std::move(p)), value_(std::move(v)), type_(t) {}

    // path_ is the SAME shared handle used as the registry map key, inside LeafId,
    // and in change events — materialised once, shared zero-copy, refcount-safe (L=B).
    std::shared_ptr<const CanonicalPath>    path_;
    std::shared_ptr<const gnmi::TypedValue> value_;       // COW versioned value, nullptr = unset (D17)
    std::optional<LeafType>                 type_;
    int64_t                                 collectedNs_ = 0;
    uint64_t                                changeSeq_   = 0;     // bumped on a real value change (D14)
    NotificationGroup*                      group_       = nullptr;
};
```

`value_` is a `shared_ptr<const TypedValue>` (D17) whose `nullptr` state means
"declared but unset" — a valid state for a config leaf that exists in the schema
but has no value until a client Sets it. Holding the value behind a shared handle
to an *immutable* version is what makes `collectLeaves` cheap (it copies a pointer,
not a protobuf) and snapshots self-consistent (see D7/D17). `type_` is `optional`
so `nullopt` unambiguously means "resolve via group or default" rather than
overloading a sentinel enum value.

`path_` is likewise a `shared_ptr<const CanonicalPath>` (L=B): the canonical path
is materialised once and that single handle is shared by the registry map key,
this entry, its `LeafId`, snapshots, and change events — a refcount bump rather
than a string copy, and refcount-safe so it cannot dangle after a detach (D16/D17).

Because the type is non-copyable and non-movable, the registry stores it in a
node-based `std::map` and constructs it in place with `try_emplace` (no copy or
move is ever required; map nodes are pointer-stable — D1).

### NotificationGroup — encapsulated, registry-owned (**[revised]**, D11)

A notification bundle: the set of leaves reported together in one gNMI
`Notification`. Owned by the registry; `members_` are non-owning back-pointers
into the registry's leaf map. Also non-copyable.

| Field            | Type                          | Meaning                                          |
|------------------|-------------------------------|--------------------------------------------------|
| `name_`          | `string`                      | Unique key (map key)                             |
| `prefix_`        | `string` (normalized)         | gNMI notification prefix for this group          |
| `atomic_`        | `bool`                        | Whether the notification carries `atomic=true`   |
| `preferredType_` | `optional<LeafType>`          | Lazy-init default type; `nullopt` = no preference|
| `members_`       | `unordered_set<LeafEntry*>`   | Non-owning pointers to registry-owned entries    |

```cpp
// notification_group.hpp
#include "leaf_entry.hpp"               // full definition (unordered_set<LeafEntry*>)

class NotificationGroup {
public:
    const std::string&             name() const noexcept { return name_; }
    const std::string&             prefix() const noexcept { return prefix_; }
    bool                           atomic() const noexcept { return atomic_; }
    std::optional<LeafType>        preferredType() const noexcept { return preferredType_; }
    const std::unordered_set<LeafEntry*>& members() const noexcept { return members_; }

    NotificationGroup(const NotificationGroup&)            = delete;
    NotificationGroup& operator=(const NotificationGroup&) = delete;

private:
    friend class LeafRegistry;          // registry creates, links, and tears down
    NotificationGroup(std::string n, std::string p, bool a, std::optional<LeafType> t)
        : name_(std::move(n)), prefix_(std::move(p)), atomic_(a), preferredType_(t) {}

    // Link/unlink a leaf. Validates path is under prefix_; maintains the
    // bidirectional invariant (leaf.group_ <-> members_). Registry-only.
    bool linkLeaf(LeafEntry* e);
    void unlinkLeaf(LeafEntry* e);

    std::string                    name_;
    std::string                    prefix_;
    bool                           atomic_ = false;
    std::optional<LeafType>        preferredType_;
    std::unordered_set<LeafEntry*> members_;
};

// Defined here because group->preferredType_ requires the complete definition.
inline LeafType LeafEntry::effectiveType() const noexcept {
    if (type_)                            return *type_;
    if (group_ && group_->preferredType()) return *group_->preferredType();
    return LeafType::Operational;
}
```

`linkLeaf` / `unlinkLeaf` are **private and registry-only** (formerly public
`addLeaf` / `removeLeaf`). The bidirectional back-pointer is an invariant the
registry owns end-to-end; exposing manual linking would let a caller create a
half-linked state. The registry maintains both sides under its lock.

`unordered_set` is chosen over `vector`: group sizes are small (< ~20),
removal is rare, so iteration-order cost is negligible, while membership test
and `unlinkLeaf` are O(1). **Iteration order is non-deterministic** — tests that
assert member lists must sort first.

### LeafRegistry — the central store

Sole owner of every `LeafEntry` and `NotificationGroup` in the process. A single
`std::shared_mutex` guards **both structure and values** (D2). All mutation goes
through registry methods; nothing outside can mutate a leaf or the topology.

#### Internal storage

```cpp
// Primary leaf store — node-based std::map for pointer stability (D1).
// NotificationGroup::members_ holds raw pointers into this map; std::map
// guarantees existing node pointers survive subsequent inserts. unordered_map
// is unusable: a rehash would relocate values and dangle every back-pointer.
// The KEY is the shared CanonicalPath handle (L=B / D16): the same handle is the
// map key, LeafEntry::path_, the LeafId, and the change-event payload — materialised
// once, shared zero-copy. DerefLess orders by the pointed-to CanonicalPath and is
// transparent, so a freshly canonicalize()d CanonicalPath looks up without
// allocating a shared_ptr.
std::map<std::shared_ptr<const CanonicalPath>, LeafEntry, DerefLess>  leaves_;

// Primary group store — same pointer-stability requirement.
std::map<std::string, NotificationGroup>  groups_;          // keyed by name

// Secondary prefix index for auto-assign and the D5 overlap check. Non-owning;
// kept in sync with groups_ at (un)registerGroup / (de)tachSubtree time. Keyed by
// CanonicalPath (mirrors NotificationGroup::prefix_, D16) with a transparent
// comparator so an ancestorPrefixes() string_view slice probes it directly.
std::map<CanonicalPath, NotificationGroup*, PrefixLess>  groupsByPrefix_;  // keyed by prefix

uint64_t                  globalSeq_ = 0;  // registry-global monotonic change token (D14)
mutable std::shared_mutex mutex_;
```

#### View / snapshot types — value copies, no pointer escapes

To stay consistent with the LeafId decision, **no internal pointer ever escapes
the registry**. Reads take the shared lock and return scalar copies plus a shared
handle to the value's *immutable* version (D17) — never a pointer or reference
into a live, mutable `LeafEntry`.

```cpp
// One leaf's value, as captured by collectLeaves(). effectiveType is resolved
// at snapshot time so the protocol layer need not re-query the live registry.
struct LeafValueSnapshot {
    std::shared_ptr<const gnmi::TypedValue> value;        // shared handle to an immutable version; nullptr = unset (D17)
    int64_t                         collectedNs   = 0;    // wire timestamp source (D14)
    uint64_t                        changeSeq     = 0;    // change-detection token; diff compares this (D14)
    LeafType                        effectiveType = LeafType::Operational;
};
using LeafSnapshot = std::map<std::string, LeafValueSnapshot>;   // keyed by path

// A group as the protocol layer needs it to build notifications: identity,
// atomic flag, and the FULL member list (all members, including those outside
// the current query — needed for atomic monitored-set expansion, Scenario 2).
struct GroupView {
    std::string              name;
    std::string              prefix;
    bool                     atomic = false;
    std::optional<LeafType>  preferredType;
    std::vector<std::string> memberPaths;     // sorted; value-copy
};

// One-shot result of subscription setup (D15): the §2.4.2-expanded leaf set
// plus the distinct groups owning >=1 member under the query, taken under a
// SINGLE shared lock so the two are mutually consistent even under concurrent
// attach/detach.
struct SubscriptionView {
    LeafSnapshot            leaves;
    std::vector<GroupView>  groups;
};
```

#### Key methods

| Method                                              | Returns             | Phase / notes                                          |
|-----------------------------------------------------|---------------------|--------------------------------------------------------|
| `registerGroup(name, prefix, atomic, preferredType?)`| `void`             | Exclusive lock; enforces D5; throws on dup/overlap     |
| `registerLeaf(path, type?, initialValue?)`          | `LeafId`            | Exclusive lock; canonicalizes; auto-assigns to group (D3) |
| `unregisterLeaf(path)`                              | `void`              | Exclusive lock; detaches from group first (D1 erase order)|
| `unregisterGroup(name)`                             | `void`              | Exclusive lock; members survive as ungrouped (D9)      |
| `attachSubtree(SubtreeSpec)`                        | `vector<LeafId>`    | Exclusive lock; atomically adds a whole device branch (D14)|
| `detachSubtree(prefix)`                             | `void`              | Exclusive lock; atomically removes a whole branch (D14)|
| `getLeaf(LeafId / path)`                            | `optional<LeafValueSnapshot>` | Shared lock; value-copy, no pointer escapes  |
| `getGroup(name)`                                    | `optional<GroupView>`| Shared lock; value-copy                               |
| `collectLeaves(prefix)`                             | `LeafSnapshot`      | Shared lock; ON\_CHANGE poll hot path (D7)             |
| `collectForSubscription(query)`                     | `SubscriptionView`  | Shared lock; one-shot subscription setup (D15)         |
| `writeValues()`                                     | `ValueWriter`       | Exclusive lock; the ONLY value-write path (D6)         |

**Erase order invariant (D1).** `unregisterLeaf()` removes the leaf from its
group's `members_` *before* erasing it from `leaves_`. Reversing it leaves a
dangling `LeafEntry*` in the group set (UB). The bidirectional pointer makes
this O(1): the leaf already knows its group.

```
1. entry.group_->unlinkLeaf(&entry)   // O(1) — remove the back-reference first
2. leaves_.erase(it)                   // now safe to destroy the entry
```

`unregisterGroup()` does **not** delete member leaves — it only clears their
`group_` to `nullptr`, leaving them ungrouped independent units (D9). The leaf
data and value remain valid.

---

## LeafType Enum

```cpp
enum class LeafType {
    Config,       // writable config leaf (gNMI Set target)
    State,        // read-only operational state reported by device
    Operational,  // default — read-only telemetry (sensors, counters, etc.)
};
```

`Operational` is the default resolved by `effectiveType()` when neither leaf nor
group specifies a type. `Config` / `State` mirror the YANG `config true/false`
annotation and OpenConfig conventions.

---

## Header Organisation

```
include/
├── leaf_type.hpp           — LeafType enum (no other deps)
├── leaf_id.hpp             — LeafId opaque handle
├── leaf_entry.hpp          — LeafEntry (encapsulated); fwd-declares NotificationGroup
├── notification_group.hpp  — NotificationGroup; includes leaf_entry.hpp
└── leaf_registry.hpp       — LeafRegistry, ValueWriter, view types; includes the above
```

Forward-declaration breaks the circular reference: `leaf_entry.hpp` only
forward-declares `NotificationGroup` (it stores a pointer); `effectiveType()` is
defined inline in `notification_group.hpp` where the full definition is
available. Callers include only the headers for the types they name.

---

## Concurrency Model (D2 **[revised]**)

> **This replaces the former two-phase model.** The old design assumed schema
> structure was immutable after a single-threaded registration phase, so it
> needed no lock for structural reads. That assumption is false for this device:
> PSU/BBU sub-devices are hot-pluggable, so leaves and groups are added and
> removed at *runtime, at unpredictable times*.

The registry is a **single reader/writer-locked store**. One rule:

- **Reads** (`collectLeaves`, `collectForSubscription`, `getLeaf`, `getGroup`)
  take a **shared lock**.
- **All mutations** — structural (`register*` / `unregister*` /
  `attach/detachSubtree`) *and* value writes (via `ValueWriter`) — take the
  **exclusive lock**.

There is no "this is safe because we are in phase X" reasoning to remember. This
is both simpler to understand and correct under runtime structural change.

**Structural mutation is stop-the-world, and that is fine.** Device
attach/detach is coarse (a whole branch) and infrequent (a physical insert /
removal). Taking the exclusive lock briefly for it does not affect the
steady-state value-update or poll throughput, so the model needs no finer-grained
structural locking.

### Value writes via `ValueWriter` (D6 **[revised]**, the sole write path)

`writeValues()` returns a `ValueWriter` — a move-only RAII object that holds the
exclusive lock and is the **only** way to mutate a leaf's value. It is not a
database transaction (no rollback/commit); it is "an exclusive-locked scope
through which values are written", hence the name.

```cpp
class [[nodiscard("store the writer or the lock is immediately released")]]
ValueWriter {
public:
    // Sets the leaf's value + collection timestamp. Compares the new value to the
    // current version; ONLY on a real change does it install a NEW immutable
    // version (value_ = make_shared<const TypedValue>(move(value))) and bump
    // changeSeq (D14/D17) — re-pushing an unchanged value is a no-op, so static
    // sensors never spuriously fire ON_CHANGE. Installing a new version (rather
    // than mutating in place) is what keeps earlier snapshots valid (D17).
    // Records actually-changed leaves in this writer's change-set for the push
    // seam. Returns false if the id is stale (leaf detached) — clean miss, never UB.
    bool set(const LeafId& id, gnmi::TypedValue value, int64_t collectedNs);

    ~ValueWriter()                             = default;   // releases the lock
    ValueWriter(ValueWriter&&)                 = default;   // movable: returned by value
    ValueWriter& operator=(ValueWriter&&)      = default;
    ValueWriter(const ValueWriter&)            = delete;    // two owners would double-release
    ValueWriter& operator=(const ValueWriter&) = delete;

private:
    friend class LeafRegistry;
    explicit ValueWriter(LeafRegistry& reg, std::unique_lock<std::shared_mutex> lk)
        : reg_(&reg), lock_(std::move(lk)) {}
    LeafRegistry*                       reg_;   // set() resolves the id and writes
    std::unique_lock<std::shared_mutex> lock_;
};
```

```cpp
// A provider pushing one device's sensor readings — one lock, many leaves:
{
    auto w = registry.writeValues();
    for (const auto& [id, reading] : myReadings)
        w.set(id, reading.value, reading.ts);
}   // lock released here
```

Rationale:
- **Sole write path.** `LeafEntry::value_` is private with `LeafRegistry` its
  friend; the only public way to change it is `ValueWriter::set`, which can only
  exist while the exclusive lock is held. "You must hold the lock to write" is
  now a *compile-time fact*, not a comment. (`[[nodiscard]]` additionally catches
  "acquired the writer then dropped it".) This closes the former gap where a
  provider could write through a raw pointer and race a reader.
- **Batching preserved.** Many `set()` calls run under one lock — the ergonomic
  win the old "write through a stable pointer" rule was reaching for, without the
  bypass.
- **Stale-id safety.** `set()` on a detached leaf returns `false`; no UB.

> **Change detection & atomic coherence (D14, D15).** `set()` decides "changed"
> by value comparison and bumps the leaf's monotonic `changeSeq` only on a real
> change — the registry enforcing ON\_CHANGE semantics centrally. The
> **ValueWriter scope is also the atomic-coherence boundary**: the exclusive lock
> is held for the whole scope, so a reader (`collectLeaves`, shared lock) can
> never observe a half-updated atomic group — therefore all members of an atomic
> group MUST be updated within one `writeValues()` scope (Scenario 6).
>
> **Push seam (future).** The writer accumulates the leaves it actually changed;
> on scope exit it releases the lock, then hands that change-set to an optional
> registry-registered listener (the protocol layer / `ILeafSink`). Poll-diff
> (today) and event push (later) consume the same primitive — `set()`'s
> value-gated `changeSeq` bump is exactly the event a push model fires. The core
> stays ignorant of subscriptions; it only emits "these leaves changed".
>
> The listener seam must cover **both** value writes (here) **and** structural
> mutations: `attach`/`detachSubtree` **and single `unregisterLeaf`/
> `unregisterGroup`** produce wire-visible adds/deletes that do not pass through
> `ValueWriter`, yet an ON\_CHANGE client subscribed to the branch must receive
> them. So in push mode the registry dispatches change events from value-write
> commits *and* from every structural op. (Poll-diff catches both for free via the
> key-set diff (D14) — this distinction only matters once push is built.)
>
> **Seam shape (finalized in protocol-layer-design.md P3 — `ILeafSink`).** The
> registry holds an optional `ILeafSink*` and dispatches **one unified
> `onChange(const ChangeBatch&)`** (no-op when unset). `ChangeBatch =
> {changed, added, removedPrefixes}` maps the four sources cleanly: a `ValueWriter`
> commit fills `changed`; `attachSubtree`/`registerLeaf` fill `added`;
> `detachSubtree`/`unregisterLeaf`/`unregisterGroup` fill `removedPrefixes` (a
> single unregister is a one-entry prefix — R2 coverage by construction). The
> `changed`/`added` records are **enriched** (R1): each `LeafChange` carries
> `{LeafId, shared_ptr<const CanonicalPath>, changeSeq, collectedNs,
> shared_ptr<const TypedValue>}` captured at commit, so the consumer never re-locks
> the core — **both** handles (path D16/L=B, value D17) are zero-copy and
> **refcount-safe**: they outlive the post-unlock, cross-thread dispatch even if the
> leaf is detached meanwhile, restoring the D1 no-dangle guarantee (finding L). The
> batch is delivered as an **owned** `shared_ptr<const ChangeBatch>` (the batch owns
> its record vectors), not a
> `span` into the `ValueWriter`'s transient buffer, so nothing dangles after the
> writer scope exits. The core still names no subscriptions; it only emits "these
> leaves changed / appeared / vanished".

---

## C++ Standard Baseline

Targets **C++20**. Features relied on:

1. **`[[nodiscard("reason")]]` with message** — on `ValueWriter` to explain *why*
   discarding the writer is wrong.
2. **`std::map::try_emplace` in-place construction** — lets the registry hold
   non-copyable, non-movable `LeafEntry` / `NotificationGroup` directly as map
   values (no copy/move ever needed), preserving node pointer-stability (D1).
3. **Defaulted `operator==`** — on `LeafId` so callers can key their own maps on
   a handle.

> Note: the former design used aggregate + designated initializers for
> `LeafEntry`. That was dropped (D11): the core types are now encapsulated to
> protect their invariants. Designated initializers remain fine for the *view*
> structs (`LeafValueSnapshot`, `GroupView`), which are plain data.

Evaluated and not adopted: `std::atomic_ref` (`TypedValue` is not trivially
copyable), `std::jthread`/coroutines (protocol-layer concern), Concepts (no
templated API benefits yet).

---

## Design Decisions

### D1 — Registry owns all entries; the public handle is `LeafId` **[revised]**

The registry is the single owner of all `LeafEntry` / `NotificationGroup`
objects, stored in node-based `std::map`s for pointer stability (existing node
pointers survive subsequent inserts; only erase of that node invalidates it).

Callers receive a **`LeafId`** (opaque handle), not a raw pointer. Raw
`LeafEntry*` live only inside the registry and inside `NotificationGroup`'s
member set, where the registry maintains them. See "LeafId" above for why a
handle is required once the schema is dynamic (D2).

### D2 — Single reader/writer-locked store **[revised]**

(Full description in "Concurrency Model" above.) One `shared_mutex`: reads take
shared, all mutations (structural and value) take exclusive. Structural mutation
is stop-the-world and infrequent. This replaces the former two-phase model,
which is unsound for a device whose schema changes at runtime.

### D3 — Auto leaf-to-group assignment

`registerLeaf()` auto-assigns the leaf to the matching group by finding the
group whose prefix is a path-ancestor of the leaf path. D5 guarantees at most one
matches.

**Mechanism (corrected):** probe each path-ancestor prefix of the leaf,
**longest first**, against `groupsByPrefix_`, returning the first hit. (This
replaces an earlier single `upper_bound` step, which misses the true ancestor
when a sibling prefix containing a `-`/`.` segment sorts between the ancestor and
the leaf — e.g. `/a/b < /a/b-x < /a/b/c`. Same O(log G) class × path depth.)
Bracket depth is tracked so `/` inside `[...]` predicate values is not treated as
a path separator.

**Groups must be registered before their leaves.** A leaf registered first finds
no group and stays ungrouped even if the group is added later (no retroactive
assignment). `attachSubtree` (D14) handles a device branch as one unit so this
ordering is automatic within a branch.

### D4 — Group name as unique key

Groups are identified by a unique `name`, used as a map key for O(log G) lookup.

### D5 — Group prefix non-overlapping constraint *(enforced at registerGroup / attachSubtree)*

No two group prefixes may be such that one is a path-ancestor-or-equal of the
other.

**Why (atomic):** spec §2.1.1 — an `atomic=true` notification for prefix P
declares the *complete state* of P; leaves absent from it are implicitly deleted
client-side. If two groups nest under the same prefix and either is atomic, the
atomic notification silently wipes the other group's leaves from the client.

**Why (auto-assign):** even with neither atomic, overlapping prefixes would give
`registerLeaf()` auto-assign (D3) two equally-valid candidates. Non-overlap keeps
D3 unambiguous independently of the atomic rule.

Sibling prefixes are allowed: `/a/b/c/d1` and `/a/b/c/d2` do not overlap;
`/a/b` and `/a/bc` do not overlap (string prefix but not a path ancestor).

**Discoverability (advisory).** Enforcement is fail-fast: `registerGroup` throws
`std::invalid_argument` naming both the offending and the existing prefix, and the
message states *why* nesting is illegal (the §2.1.1 complete-state reason) so a
developer who trips it understands the rule without reading this doc. Two read-only
helpers let a caller / tooling / test see a conflict *ahead* of time:
`registeredPrefixes()` (all current prefixes, sorted) and `wouldConflict(prefix)`
(returns the existing prefix it would clash with, or `nullopt`). These are
**advisory only** — the authoritative, race-free guard stays `registerGroup`, which
re-checks under the exclusive lock at insert time, so a `wouldConflict()` answer can
go stale before a later `registerGroup` under concurrency. As the device topology
grows, declaring it as a single schema/manifest validated as a whole (seeded by the
`SubtreeSpec` of D12) is the scalable way to surface conflicts at load/build time
rather than via scattered runtime registration; these helpers are the minimal step
toward that without building the manifest layer yet.

### D6 — Value updates via `ValueWriter` only **[revised]**

(Full description in "Concurrency Model".) Value writes go through
`ValueWriter::set(LeafId, value, ts)`, which holds the exclusive lock and bumps
the change token. `LeafEntry::value_` is otherwise unreachable. Replaces the
former "write directly through a stable raw pointer" rule, which allowed
unsynchronized writes and required a separate "upper layer manages
synchronization" caveat. Now the registry both owns and *enforces* the write
contract.

### D7 — Snapshot via shared value handle (**[revised]**, D17)

`collectLeaves(prefix)` takes the shared lock, copies each matching leaf's
`{value, collectedNs, changeSeq, effectiveType}` into a `LeafSnapshot`, releases
the lock, and returns by value. The caller diffs two independent snapshots
(before/after a poll interval) to detect ON\_CHANGE; no lock is held after return.
The `value` copied into the snapshot is a `shared_ptr` to an *immutable version*,
not a deep protobuf copy — see **D17** for why that is both cheap and safe.

Subtree boundary is enforced via an ancestor-or-equal check so `/a/b/c` excludes
`/a/b/cx` (spec §2.4.2 "node and all its children", not "string prefix").

### D8 — Type lazy initialization via group

Effective `LeafType` priority: (1) leaf's own `type_`; (2) group's
`preferredType_`; (3) `Operational`. A group thus acts as a type initializer for
its members, reducing boilerplate when all members share a schema type.

### D9 — Ungrouped leaves

A leaf with `group_ == nullptr` is an independent notification unit. ON\_CHANGE
for it emits a single-leaf, non-atomic notification; no complete-state semantics.
`unregisterGroup()` turns former members into ungrouped leaves.

### D10 — Path normalization at the registry boundary

`registerLeaf`, `registerGroup`, `getLeaf`, `collectLeaves`,
`collectForSubscription`, and the subtree ops normalize path strings internally
before storage or lookup; callers never pre-normalize.

**Superseded by D16.** The boundary normaliser is the free function
`canonicalize(std::string_view) → CanonicalPath` (in `canonical_path`, **not** a
`LeafRegistry` method), the sole factory for a `CanonicalPath` key — so an
un-normalized key is impossible to construct. It strips predicate double-quotes
(`/a/b[name="eth0"]/c → /a/b[name=eth0]/c`, bracket-depth tracked; quotes outside
`[...]` preserved), strips a trailing slash, and sorts a node's `[key=val]`
predicates — making string equality coincide with gNMI structured-`Path` node
equality for the subset the mock supports. Full rules and the `origin` handling are
in **D16** (findings D/I).

### D11 — Encapsulated, non-copyable core types **[new]**

`LeafEntry` and `NotificationGroup` are encapsulated classes (private fields,
const accessors), `= delete`-d copy, with `LeafRegistry` as friend. The
leaf↔group back-pointer is a bidirectional invariant the registry owns; exposing
public mutable fields or copy would let it break silently (a copied leaf points
at a group that does not list it). Linking is registry-only (`linkLeaf` /
`unlinkLeaf`). The registry stores them in `std::map` via `try_emplace` (no
copy/move needed). This is the concrete application of Goal 4.

### D12 — Subtree attach/detach primitives **[new]**

Hot-pluggable devices come and go as **whole branches** (a PSU = one subtree
under one prefix). The structural API matches that granularity:

```cpp
struct SubtreeSpec {                     // declarative description of one device branch
    struct GroupSpec { std::string name, prefix; bool atomic;
                       std::optional<LeafType> preferredType; };
    struct LeafSpec  { std::string path; std::optional<LeafType> type;
                       std::optional<gnmi::TypedValue> initialValue; };
    std::vector<GroupSpec> groups;
    std::vector<LeafSpec>  leaves;
};

std::vector<LeafId> attachSubtree(const SubtreeSpec& spec);  // exclusive lock
void                detachSubtree(const std::string& prefix);// exclusive lock
```

- **Atomicity.** The whole branch is added/removed under one exclusive lock, so a
  reader never observes a half-attached or half-detached device (e.g. 3 of 50
  leaves present). Internally `detachSubtree` still erases each leaf one by one
  (the unavoidable storage action) — it just does so as one indivisible,
  registry-driven operation rather than N separate provider calls.
- **Low coupling.** A provider names only its branch prefix; it does not track
  the full `LeafId` set of its device. `detachSubtree(prefix)` scans and removes
  the subtree (leaves + groups whose prefix is under it).
- **Presence stays in the provider layer.** A provider watching
  `/.../psu[id=3]/presence` go false calls `detachSubtree("/.../psu[id=3]")`. The
  core has no notion of "presence"; it only offers atomic attach/detach.

### D13 — Merged subscription-setup query **[new]**

Subscription setup needs both the §2.4.2-expanded leaf set and the distinct
groups owning members under the query. Under a dynamic schema, issuing those as
two separate shared-locked calls risks a detach between them, yielding a leaf set
and group set that disagree. `collectForSubscription(query)` returns both in one
`SubscriptionView` under a single shared lock. It is a one-shot, non-hot-path
call, so merging is free and removes the inconsistency window. (The per-interval
poll continues to use the lighter `collectLeaves`.)

### D14 — Change-detection contract **[new]**

How the protocol layer learns *what changed*, finalized:

- **Change token = a registry-global monotonic `uint64 changeSeq`.**
  `ValueWriter::set` compares the new value to the leaf's current value (protobuf
  equality) and, **only on a real change**, sets `leaf.changeSeq_ = ++globalSeq_`
  (and updates `value`/`collectedNs`). Re-pushing an unchanged value is a no-op —
  a provider may blindly push every sample and static leaves never spuriously
  fire ON\_CHANGE. The *registry*, not the provider, decides what counts as a
  change (Goal 4).
- **Why a separate seq, not `collectedNs`.** A global monotonic counter is
  collision-free (two changes in the same clock tick get distinct seqs, unlike a
  timestamp) and gives a total order of changes — the seam a future push/
  changelog model needs (D6). `collectedNs` keeps its own job: the wire timestamp.
- **Poll diff** compares `changeSeq` over the **union** of before/after snapshot
  keys:

  | case | meaning | wire |
  |---|---|---|
  | key in after, not before | leaf added | `update` |
  | key in before, not after | leaf removed | `delete` |
  | key in both, `changeSeq` differs | value changed | `update` |
  | key in both, `changeSeq` equal | unchanged | — |

- **Unset leaves never go on the wire.** A `value == nullopt` leaf (declared,
  never Set) is skipped in initial sync and in updates; its first real value
  lands in the "changed" branch and is sent then.
- **Delete granularity.** A removed subtree (`detachSubtree`) MAY be reported as
  a single branch/container-level `delete` (spec §3.5.2.3), not one delete per
  leaf — ideal for unplugging a device branch (Scenario 4).
- **Timestamp collapse (spec §3.5.2.3 / §2.1.1).** The single per-`Notification`
  timestamp is: for an independent/non-atomic update, the changed leaf's own
  `collectedNs` (one Notification per leaf); for an atomic-group re-send,
  `max(collectedNs)` over the included (subscribed) members — "state as of the
  most recent member change". (Settles former review finding G.)
- **Also powers SAMPLE `suppress_redundant`.** The same `changeSeq` comparison
  lets a SAMPLE subscription skip leaves unchanged since the last sample
  (§3.5.1.5.2) — one mechanism serves ON\_CHANGE diffing and SAMPLE suppression
  alike.
- **Accepted imprecision.** Under polling, a value that churns A→B→A within one
  interval advances `changeSeq` twice, so the diff emits one redundant (but
  correct-valued, harmless) update. A future push model removes even that.

### D15 — Group semantics & bundling guidance **[new]**

A `NotificationGroup` is load-bearing **only when `atomic == true`**. This
decision records the three-way classification so leaves are modelled correctly,
grounded in spec §3.5.2.1 (bundling) and §3.5.1.5.2 (ON\_CHANGE/SAMPLE):

| Data | Model as | Why |
|---|---|---|
| Leaves atomically applied together via `Set` (e.g. an NTP config record) | **atomic group** (`atomic=true`) | atomic boundary: complete-state + implicit-delete (§2.1.1); ON\_CHANGE re-sends all *subscribed* members on any member change (§3.5.2.5) |
| Leaves **meaningfully conjoined and co-collectable** — they can share one timestamp: static-after-boot properties, component inventory (part/model/serial), or a set of sensors sampled together and reported as a unit | non-atomic group (`atomic=false`) | a *bundling* statement: the server's explicit, **subscription-independent** declaration that these are one bundle, emitted together in one Notification with a shared timestamp **when the whole set is sent** (ON\_CHANGE initial sync, SAMPLE, ONCE/POLL) |
| Leaves whose **distinct precise timestamps are meaningful** (hardware-timestamped counters, high-rate events) | **ungrouped** leaves (D9) | §3.5.2.1 MUST NOT obscure their individual timestamps by bundling; each reports independently with its own timestamp |

Consequences, so the boundary is unambiguous:

- **A non-atomic group has NO effect on steady-state ON\_CHANGE.** When one
  member changes, only that member is reported (per-leaf), exactly as if it were
  ungrouped. The group's bundling manifests *only* when the whole set is sent
  together (Scenario 5). It does **not** force siblings to re-send — that is an
  atomic-only behaviour.
- **Grouping is therefore a SAMPLE / whole-set decision, not an ON\_CHANGE one.**
  Declare a non-atomic group when the server should emit those leaves as one
  timestamped Notification on a sample/initial/once send — this is real,
  subscription-independent server-side knowledge (it works even if the client
  subscribed to the members individually rather than to a common container). The
  only hard limit is the §3.5.2.1 one: do not bundle leaves whose individual
  precise timestamps are meaningful.
- **`preferredType` (D8) is orthogonal** to atomicity and stays available on
  either group kind (or set per-leaf); it is a type-defaulting convenience, not a
  notification concern.
- **Bundling here is *not* `allow_aggregation`.** A group emits its members as
  **separate `Update`s within one `Notification`** sharing a timestamp — that is
  *bundling* (§3.5.2.1), which the spec permits freely and whose use cases (atomic
  `Set`, static-after-boot, inventory) are exactly this classification. It is
  **distinct from** aggregation (`allow_aggregation`, §3.5.1.2: many schema
  elements collapsed into a single `Update`'s `val` blob, client-gated, default
  off). So group bundling MUST NOT be gated on `allow_aggregation`. (Detailed in
  protocol-layer-design.md P3.)

This is why D5 (non-overlap) and the monitored-set expansion (Scenario 2) only
ever matter for atomic groups: they are the only groups with wire semantics.

### D16 — Path equivalence: the `CanonicalPath` identity type **[new]**

**Problem (findings D/I).** A leaf's identity is its path string. But one gNMI
node has many string spellings, and the original `normalizePath` (quotes only)
made distinct spellings collide into distinct map keys — a client that addressed
the same node differently would silently miss it, and a group prefix could fail
to capture its own leaves. The gNMI spec is explicit about what makes two paths
the *same node*:

- **Key order is irrelevant.** A `PathElem`'s keys are a `map<string, string>`
  (§2.2.2, L340–341), so `/a/b[k1=v1][k2=v2]` and `/a/b[k2=v2][k1=v1]` are the
  same node, different strings.
- **Node identity is the tuple `<origin, path>`** (§2.7, L852–854); `origin`
  unspecified defaults to `openconfig` (§2.7.1, L880–881).
- **Root `/` is the zero-element path** (§2.2.2, L400); a trailing slash is not a
  distinct node.

**Decision — the key is a `CanonicalPath` value type whose only constructor is
the canonicaliser.** The map key is not a raw `std::string` but a small
move/copy-cheap value type that *wraps* the canonical string and **can only be
obtained by running normalization** — there is no public constructor that takes
an arbitrary string. The canonicaliser makes string equality coincide with gNMI
structured-`Path` node equality (for the subset the mock supports):

```cpp
// canonical_path.hpp — owns path identity; no dependency on the registry.
class CanonicalPath {
    std::string s_;                                  // already-canonical form
    explicit CanonicalPath(std::string normalized) : s_(std::move(normalized)) {}
    friend CanonicalPath canonicalize(std::string_view raw);   // the sole builder
public:
    const std::string& str() const noexcept { return s_; }
    bool operator<(const CanonicalPath&) const noexcept;   // delegates to s_
    bool operator==(const CanonicalPath&) const noexcept;
    // ... hash; no public ctor from a bare string ...
};

// the SOLE factory — a FREE function in canonical_path (not a LeafRegistry method),
// so path identity is independently testable/reusable; every inbound path string
// goes through here:
CanonicalPath canonicalize(std::string_view raw);
```

**Storage (L=B).** The canonical path is materialised **once** as a
`shared_ptr<const CanonicalPath>` and shared everywhere: the registry keys
`std::map<std::shared_ptr<const CanonicalPath>, …>` on it (a deref comparator with
transparent lookup from a freshly `canonicalize()`d raw string), and that same
handle is held by `LeafEntry::path_`, `LeafId`, snapshots, and change events. This
removes the old key/`path_` string duplication and makes path-passing a refcount
bump, mirroring the D17 value handle. Every boundary method (`registerLeaf`,
`registerGroup`, `getLeaf`, `collectForSubscription`, the subtree ops …) takes a
raw string, calls `canonicalize()` once, and works with the resulting handle. **It
is impossible to put an un-normalized key into the registry** — the type system,
not caller discipline, enforces it.

**Origin (O1/O2) is deferred — keep it out of the core (decided).** The core key
stays strictly origin-less: `canonicalize()` and the core map see only origin-less
canonical strings. Whether node identity later becomes `<origin, path>` (O1) or
stays single-origin (O2) is a **boundary** decision, made when the
protocol/provider layer is built. The core implementation MUST keep `origin` out
of its keys and methods, so that choice stays localised to `canonicalize()` / the
boundary (design-review.md findings I, N).

Canonicalisation rules, applied in order inside `canonicalize()`:

1. **Trailing slash** — strip a single trailing `/` unless the path *is* root
   `/` (length 1). Fixes the silent auto-assign miss where a group prefix `/a/b/`
   rejected `/a/b/c` (finding D).
2. **Predicate quotes** — strip `"` around values inside `[...]` (existing rule),
   honouring `\`-escapes so a value may contain `]` or `"`.
3. **Predicate key ordering** — within each path element, sort its `[key=value]`
   predicates by key name (ASCII). Multi-key list entries become order-
   independent (finding D). A repeated key in one element is a registration error
   (a key map cannot repeat).

Identity is then `CanonicalPath` equality (delegating to the wrapped string), and
**every existing string/`/`-boundary mechanism stays unchanged and becomes
sound**: `isUnderPrefix` (ancestor walk), D5 overlap check, D3 longest-ancestor
auto-assign, and §2.4.2 container expansion all operate on the canonical string
(`CanonicalPath::str()`), whose `/` boundaries now line up with element
boundaries.

**Why this representation (three options weighed).**

- **(A1) Plain `std::string` key.** Lightest. But correctness relies on every
  entry point *remembering* to normalize first — nothing in the type system
  enforces it, so one missed call silently inserts an un-normalized key. The most
  important invariant is left to discipline.
- **(A2) Structured `Path` key** (a parsed `{vector<Elem>, …}` with
  `operator<`/hash). Makes illegal states unrepresentable, but forces *every*
  prefix/ancestor/overlap/expansion operation to be re-implemented on the
  structured form — the same logic, more code — and drags the structured-`Path`
  type into the core, reversing its deliberate decoupling from gNMI's path
  structure (cf. finding F).
- **(A3, chosen) `CanonicalPath` value type.** The wrapped string keeps all the
  `/`-boundary logic to a few lines and never leaks `Path` internals; *and*
  because the only way to obtain one is `canonicalize()`, "is this key
  normalized?" becomes a **compile-time guarantee** rather than a convention.
  This buys A2's "illegal states unrepresentable" at essentially A1's code size —
  no `Path` parser, no gNMI-structure coupling. Extending the canonical form
  later = adding one rule to `canonicalize()`. (Goals: 易理解 / 少冗餘 / 低耦合 /
  易擴充 — and it removes the one footgun A1 left open.)

**Origin (finding I) — single-origin core, origin owned by the boundary
(decided).** Node identity in gNMI is the tuple `<origin, path>`, and `origin`
can name a *different schema tree over the same device* — the classic case being
a `cli` / vendor-native origin used as an escape hatch alongside `openconfig` for
features OpenConfig does not yet model (spec §2.7.3). Two options were weighed:

- **(O1) Fold origin into the key** — canonical key becomes `<origin, path>`,
  empty→`openconfig`. Fully spec-correct on the identity tuple and multi-origin
  ready, but every stored key and every prefix comparison carries an origin
  dimension the mock never varies — redundant surface for an unused capability.
- **(O2, CHOSEN) Single-origin core; the protocol/provider layer owns origin.**
  The core key stays the origin-less canonical path. The boundary layer is the
  sole place origin is handled: it (a) defaults an empty origin to `openconfig`,
  (b) rejects any other (syntactically valid) origin with **`UNIMPLEMENTED`** —
  it names a schema the mock does not implement, per the Get/Subscribe behavior
  tables §3.3.4 L1152 / §3.5.2.4 L1900 (finding **N**); `InvalidArgument` is for a
  *malformed* path — (c) strips origin before calling the core. The core
  documents a **single-origin invariant**.

  > ✅ **Implemented (backlog C1).** The gNMI boundary now does exactly (a)–(c)
  > via `validateOrigin()` (`utils/utils.h`), shared by get/set/subscribe, with
  > `gnmi_to_xpath` no longer embedding origin. (Re-attaching origin onto response
  > paths was deferred — strip-only; see protocol-layer-design.md C1.)

**Decision rationale.** This is a greenfield, telemetry-focused ORv3 PSC mock
serving a single, well-modelled OpenConfig surface (components / sensors / PSU /
BBU) — there is no legacy CLI tree to preserve and no vendor-native model to
expose in parallel, so one `openconfig` origin covers 100% of what it serves.
O2 *fixes* finding I's real defect — origin is no longer silently dropped, it is
explicitly defaulted and validated — while keeping the core key minimal. Origin
is a transport/schema concern, so it belongs in the layer that owns transport,
not smeared across every core map key. **This is a fact-driven choice (the mock
genuinely has one tree), not a shortcut**, and it is not a one-way door: should a
second origin ever be needed, the upgrade is localised to `canonicalize()` and
`CanonicalPath`'s wrapped representation (promote `path` → `<origin, path>`, i.e.
fall back to O1) — nothing else in the core changes, which is exactly why wrapping
the key in the `CanonicalPath` type (A3) pays off here.

**Element-aligned prefix (documented consequence).** Because `isUnderPrefix`
treats the character after the ancestor as a boundary, a following `[`
(predicate) is a non-match: a bare list prefix `/if/i` does **not** capture keyed
entries `/if/i[name=eth0]/...` — and that is correct, the list and a list entry
are different nodes. A group that should own a list entry's leaves must name the
**complete element including its key predicate** (e.g.
`/components/component[name=psu0]`), which is exactly how hot-plug device
branches are declared (D12). Group prefixes are thus element-aligned by rule.

### D17 — Hot-path: copy-on-write value sharing **[new]**

**Problem (finding K).** ON\_CHANGE polling diffs two snapshots per subscription
per interval, but the diff compares only `changeSeq` (a `uint64`, D14). The
original D7 deep-copied every leaf's `TypedValue` into each snapshot — copying
potentially large protobufs just to compare 8-byte counters, then discarding the
copies for the (usually majority) leaves that did not change.

**Decision — store the value as a `shared_ptr<const gnmi::TypedValue>` and treat
each write as installing a *new immutable version* (copy-on-write).**

- `LeafEntry::value_` is `shared_ptr<const TypedValue>` (`nullptr` = unset).
- `ValueWriter::set`, on a *real* change, does
  `value_ = std::make_shared<const TypedValue>(std::move(newValue))` — it never
  mutates the pointed-to object, it swaps in a fresh version (and bumps
  `changeSeq`, D14).
- `collectLeaves` copies that `shared_ptr` into the snapshot — a refcount bump,
  **not** a protobuf deep copy.

**The same model now covers the path (L=B).** `LeafEntry::path_` is likewise a
`shared_ptr<const CanonicalPath>` (D16): the canonical path is materialised once
and that one handle is shared — registry map key, this entry, its `LeafId`,
snapshots, and change events — as a zero-copy, refcount-safe handle. It mirrors
the value handle, removes the old key/`path_` string duplication, and makes the
change-event payload safe across the post-unlock cross-thread dispatch: it cannot
dangle on a concurrent detach, restoring the D1 no-dangle guarantee (design-review
finding L).

**Why this is the best fit (vs. the alternatives weighed).**

- **Cheap snapshots, one lock.** The per-interval snapshot cost drops from "N
  deep `TypedValue` copies" to "N pointer copies"; the diff still compares
  `changeSeq`. No second lock, no extra API.
- **Self-consistent & safe.** A snapshot holds the exact version current at
  snapshot time; a later write swaps the *entry's* pointer to a new version,
  leaving the snapshot's handle pointing at the old, still-valid, immutable one.
  No pointer into a live mutable entry ever escapes — strictly safer than copying
  a value out of a mutable field, and it preserves the point-in-time guarantee
  D7 needs.
- **Composes with push (D6).** A change event can carry the same `shared_ptr` to
  the new version with zero copy, so the COW value is also the right substrate
  for the future push/changelog model.
- **Rejected alternatives.** A *two-pass* primitive (lightweight `changeSeq`
  scan, then a second locked fetch of only-changed values) avoids the deep copy
  too, but adds API surface and re-introduces a two-lock window — against the
  single-lock-consistency spirit of D13. Doing *nothing* leaves the cost on the
  SAMPLE poll path (which push never removes) and on initial sync.

**Cost accepted.** Each value version is one heap allocation behind an atomic
refcount. For a mock's leaf counts this is negligible, and it buys both the
hot-path win and a cleaner safety story. Under the rwlock, readers and writers
never run concurrently, so the atomic refcount is not even load-bearing for
correctness — it is what lets a *released* snapshot outlive a subsequent write.

#### Future direction (NOT built now): the change log

D17 makes each poll interval *cheap* (pointer copies, not deep copies) but still
**O(N)** — every poll visits all N leaves under the subscription to compare
`changeSeq`, even when nothing changed. The next step up is **O(k)**: visit only
the *k* leaves that actually changed this interval. This subsection records that
design so a future maintainer knows it was considered, why it pays off, and when
to reach for it — without paying its cost today.

**What it is.** A registry-owned, append-only **change log** ordered by
`changeSeq`: each `ValueWriter` commit appends one record per changed leaf —
`{changeSeq, path, shared_ptr<const TypedValue> /*or tombstone for a delete*/}`.
A reader asks `collectChangesSince(prefix, sinceSeq)` and gets back only the
records with `seq > sinceSeq` under that prefix, plus the new high-water seq to
remember. The per-interval poll-diff (collect-then-compare two snapshots) is
replaced by "give me what changed since I last looked."

**Why it has future value.**

- **Cost stops scaling with the data, only with the churn.** An idle subscription
  costs O(1) (its `sinceSeq` already equals the global high-water — nothing to
  return); a busy one costs O(k) in the number of actual changes. Today's D17
  poll is O(N) per subscription per interval regardless of churn.
- **It is the natural backbone for push (D6).** A push/ON\_CHANGE-callback model
  *is* "stream the change log to subscribers." The same structure also enables
  things polling can't do well: client **resumption / catch-up** after a dropped
  stream (replay from the client's last seq), change **coalescing**, and a
  first-class "what changed since X" query (audit/debug, Get-with-since).
- **It reuses primitives already chosen.** `changeSeq` (D14) is the total order
  the log is keyed by; the `ValueWriter` change-set (D6) is exactly the per-commit
  record list; D17's COW `shared_ptr` values let a record hold the value version
  with **zero copy**. The current decisions were made to be forward-compatible
  with this — it is not a seed that has to be retrofitted.

**When adopting it is appropriate (i.e. when it stops being over-engineering).**
Reach for it when *at least one* of these is true — until then O(N)-cheap (D17)
is the right amount of engineering:

- **Scale signal:** N grows to the thousands–tens-of-thousands of leaves, *and*
  there are many concurrent subscriptions and/or a high poll rate, so the O(N)
  scan becomes a measured cost (profile, don't guess). A single PSC card mock is
  nowhere near this.
- **Architecture signal:** ON\_CHANGE is actually moved from poll to **push**
  (D6). At that point a durable, ordered change stream stops being optional — the
  log is the thing you push *from* — so building it is justified by the feature,
  not by performance.
- **Functional signal:** a real need appears for **stream resumption**, dropped-
  client **catch-up/replay**, or a **"changes since seq/time X"** query. These are
  awkward to bolt onto snapshot-diffing and natural on a log.

**Refactoring cost later — small, and deliberately so.** The change log is
**additive to the core, localized in the protocol layer**:

- *Core data model is untouched.* `LeafEntry`, the registry's `std::map` storage,
  `LeafId`, `CanonicalPath`, `ValueWriter`, `changeSeq`, and the COW value model
  all stay exactly as they are. The log is a *new index* (`vector`/`map` keyed by
  `changeSeq`) plus a *new read method* (`collectChangesSince`) — it adds, it does
  not rewrite. `collectLeaves` can remain for initial sync / SAMPLE / Get.
- *Protocol layer swaps its diff source.* It already conceptually holds a
  `lastSeq`; it moves from "snapshot, diff, repeat" to "ask for changes since
  `lastSeq`." That is a contained change in one layer.
- *The one genuinely new piece is retention.* An append-only log must be trimmed,
  so it needs a policy: trim records below the slowest live subscriber's
  `sinceSeq`, and force a full resync for any client that has fallen off the back.
  **This retention/compaction policy is the real work** — and precisely the part
  that would be premature complexity to build before a signal above exists.

Because everything except retention is additive and reuses D14/D6/D17, the
migration is incremental (introduce the log alongside, move the hot path over,
keep `collectLeaves` for the cold paths) rather than a big-bang rewrite. That
forward-compatibility is *why* it is safe to defer.

### D18 — Leaf-centric model: interior nodes are implicit prefixes **[new]**

The registry stores exactly two kinds of entity: `LeafEntry` (the value-bearing
leaves) and `NotificationGroup` (notification scopes). **There is no object for an
interior/container node.** A container exists *implicitly*, exactly when ≥1 leaf
lives under its prefix; it is an **addressing scope**, not a stored entity.

- **An empty container is simply absence.** `/components/power` with no leaves under
  it is unknown to the registry — and that is gNMI-correct: a container has no value
  of its own (its content *is* its descendant leaves, §2.4.2), so an empty one has
  nothing to store or report. `collectLeaves("/components/power")` just returns the
  empty set.
- **Containers appear/vanish as a function of leaf presence.** `registerLeaf` /
  `attachSubtree` make a subtree exist; `detachSubtree` / `unregisterLeaf` make it
  vanish. A **stable parent container that devices hot-plug under** (e.g.
  `/components/power`, into which `…/psu[name=PSU3]/vin` grows) needs **no "create"
  step**: it is just the common prefix of whatever is currently under it, and it is
  always a valid `collectLeaves` / subscribe / Get target whether populated or not.
  Growth is therefore "a new leaf appears under an existing (possibly stable) prefix",
  not "materialise a node object" — `registerLeaf` for one leaf, `attachSubtree` for a
  whole branch; neither requires the parent to pre-exist as an object. (Scenario 4
  variant `GrowsUnderExistingContainerThenDetaches`.)
- **A group may pre-declare a scope before its leaves exist.** Registering a
  `NotificationGroup` at a prefix is the way to make a *scope* (e.g. an atomic device
  boundary) exist ahead of its leaves; leaves auto-join (D3) as they appear. This is
  distinct from — and the only stored stand-in for — a "container that exists while
  empty".
- **Subscribing to an empty / not-yet-existent container is a protocol-layer concern,
  not a core-storage one.** §3.5.1.3 (a subscription to a path that has no children
  yet must stay armed and emit when they appear) is handled by the protocol layer
  retaining the subscription query and re-expanding on a later `attachSubtree` (P4) —
  the core does **not** store an empty node to support it.
- **Schema validity is NOT the core's job.** The core is a path-keyed leaf store, not
  a YANG validator. It does not enforce that a node is *either* a leaf *or* an interior
  node: it will happily hold a leaf at `/a/b/c` and another at `/a/b/c/d/e`
  simultaneously, which YANG forbids (a leaf has no children). Which paths are
  containers vs leaves, and rejecting schema-invalid topologies, belongs to the
  provider / schema layer above the core.

This is why the API is shaped around *leaves and prefixes* (`registerLeaf`,
`collectLeaves(prefix)`, `attach/detachSubtree(prefix)`) and never around a
"container" type: modelling interior nodes as implicit prefixes is both simpler and a
faithful match to gNMI's "a path names a node and all its children" semantics.

---

## Constraint Summary

| Constraint                              | Enforced at                                |
|-----------------------------------------|--------------------------------------------|
| Group prefix non-overlapping            | `registerGroup` / `attachSubtree`          |
| Leaf auto-assigned to at most one group | `registerLeaf` (prefix scan, D3)           |
| Group name unique                       | `registerGroup` / `attachSubtree`          |
| Groups registered before their leaves   | caller convention; automatic within a branch (D14) |
| Reads under shared lock                 | registry methods (internal)                |
| All mutation under exclusive lock       | registry methods (internal); value writes via `ValueWriter` |
| Handle never dangles                    | `LeafId` resolves-or-misses (D1/D12)       |
| Leaf↔group back-pointer consistent      | registry-only linking, encapsulated types (D11) |
| "Changed" decided by value, not by write | `ValueWriter::set` value-gates `changeSeq` (D14) |
| Atomic group seen all-or-nothing        | one `writeValues()` scope per atomic update (D6/D14, Scenario 6) |
| Bundling only for shared-timestamp sets | non-atomic group = co-collectable bundle; distinct-precise-timestamp leaves stay ungrouped (D15, §3.5.2.1) |
| One node = one key regardless of spelling | `canonicalize()` → `CanonicalPath`: key order / trailing slash / quotes (D16) |
| Registry key is always normalized        | `CanonicalPath` has no public ctor; only `canonicalize()` builds one (D16 A3) |
| Single origin (`openconfig`)             | protocol/provider boundary defaults + validates; core key origin-less (D16 O2) |
| Snapshot is cheap & point-in-time        | value held as `shared_ptr<const TypedValue>`; writes install a new version, never mutate (D17) |

---

## Relationship to Providers

Providers are a higher-level concern, excluded from this design. A provider:
- Calls `registerGroup` / `registerLeaf`, or `attachSubtree` for a whole device
  branch, to declare its schema; keeps the returned `LeafId`s.
- Pushes value updates via `registry.writeValues()` + `ValueWriter::set(id, …)`.
- On device removal, calls `detachSubtree(prefix)` and drops its `LeafId`s.
- Needs no manual locking and holds no raw pointers — the registry owns both.

One provider typically owns one or more device branches. Groups do not cross
providers.

---

## gNMI Behaviour Scenarios

Concrete wire behaviour grounding the decisions; the primary reference for the
protocol layer built on top.

### Scenario 1 — Container subscription expands to all leaves (spec §2.4.2)

Registry contains:

```
/a/b/c1/d1/e1  → groupA (prefix=/a/b/c1/d1, atomic=true)
/a/b/c1/d1/e2  → groupA
/a/b/c1/d2/e1  → groupB (prefix=/a/b/c1/d2, atomic=false)
/a/b/c1/d2/e2  → groupB
/a/b/c2/f/g1   → groupC (prefix=/a/b/c2/f,  atomic=true)
/a/b/c2/f/g2   → groupC
```

Client subscribes to `/a/b/c1` and `/a/b/c2/f/g1`.
`collectLeaves("/a/b/c1")` returns all four leaves under it (groupA + groupB).
`collectLeaves("/a/b/c2/f/g1")` returns exactly that one leaf.

Spec §2.4.2 MUST: a path refers to the node **and all its children**; never
return only the node when children exist.

### Scenario 2 — ON\_CHANGE with atomic group + partial subscription (spec §3.5.2.5)

Client has ON\_CHANGE for `/a/b/c1` and `/a/b/c2/f/g1`.

**Monitored set** (the protocol layer expands atomic groups so an unsubscribed
member's change still triggers a re-send) — built from
`collectForSubscription`'s `GroupView.memberPaths`:
```
groupA: e1, e2  (atomic → monitor both)
groupB: e1, e2  (non-atomic → only subscribed)
groupC: g1, g2  (atomic → monitor g2 even though the client didn't subscribe it)
```

**Event:** `/a/b/c2/f/g2` changes (NOT subscribed). groupC is atomic, so re-send
all **subscribed** groupC members:
```
Notification: prefix=/a/b/c2/f,  update: g1   (g2 omitted), atomic: true
```
Spec §3.5.2.5: payload **MUST ONLY** contain subscribed leaves. Monitoring all
members is server-internal; the wire payload is filtered to subscribed-only.
(Verified directly against the spec text during the design review.)

**Event:** `/a/b/c1/d2/e2` changes (groupB, non-atomic). Only that leaf is
reported, no atomic flag, no sibling re-send.

### Scenario 3 — Why non-overlapping prefix matters (spec §2.1.1)

Wrong design with nested prefixes:
```
groupX: prefix=/a/b,     atomic=true
groupY: prefix=/a/b/c/d, atomic=false
```
When groupX fires an atomic notification for `/a/b`, the client treats it as the
complete state of everything under `/a/b`; groupY's unchanged leaves, absent from
that notification, are implicitly deleted client-side. D5 prevents this at
registration time.

### Scenario 4 — Hot-pluggable device branch attach/detach **[new]**

A PSU is inserted under `/components/psu[name=PSU3]`:
```cpp
auto ids = registry.attachSubtree(psu3Spec);   // groups + leaves, one exclusive lock
```
All of PSU3's groups and leaves appear atomically — a concurrent
`collectLeaves("/components")` either sees none of PSU3 or all of it, never a
partial branch.

The provider then pushes readings:
```cpp
auto w = registry.writeValues();
for (auto& [id, v] : psu3Readings) w.set(id, v, now);
```

When PSU3 is unplugged (its `presence` leaf goes false), the provider calls:
```cpp
registry.detachSubtree("/components/psu[name=PSU3]");  // whole branch removed atomically
```
Any `LeafId` the provider still holds for PSU3 becomes stale; a later
`ValueWriter::set` on it returns `false` rather than corrupting memory (D1/D6).
On the wire this manifests as a **single branch-level `delete`** for
`/components/psu[name=PSU3]` (spec §3.5.2.3 permits container-level deletes
rather than one per leaf), built by the protocol layer (D14).

### Scenario 5 — A non-atomic group is a SAMPLE/whole-set concern, not an ON\_CHANGE one

Three sensors `/sensors/temp`, `/sensors/volt`, `/sensors/curr` that the server
co-samples and reports as a unit. Declared as a **non-atomic group** (D15).

**Under ON\_CHANGE the group is moot** — each member change is reported alone:
```yaml
Notification: { timestamp: t1, update: [ /sensors/temp = 41 ] }   # temp changed
Notification: { timestamp: t2, update: [ /sensors/volt = 12 ] }   # volt changed
```
A `temp` change does **not** re-send `volt`/`curr` (that is atomic-only). Grouped
or not, ON\_CHANGE output is identical.

**Under SAMPLE the group earns its keep** — the server knows, explicitly and
independently of how the client subscribed (even if it subscribed to the three
members separately), that they are one bundle, so it emits them in a single
Notification with one shared timestamp:
```yaml
Notification:
  timestamp: ts_sample
  update: [ /sensors/temp = 41, /sensors/volt = 12, /sensors/curr = 3 ]
```
This is the server-side "I may bundle these" statement of §3.5.2.1.
`suppress_redundant` still filters per-leaf within the sample (§3.5.1.5.2). The
one hard limit: do **not** group leaves whose individual precise timestamps are
meaningful (hardware-timestamped counters / high-rate events) — bundling would
obscure them (§3.5.2.1).

### Scenario 6 — Atomic config coherence via one `ValueWriter` scope

Atomic group `/system/ntp` (`atomic=true`), members `server`, `port`, `vrf`. A
client ON\_CHANGE-subscribes `/system/ntp`. The provider applies a new NTP config:

```cpp
{
    auto w = registry.writeValues();          // one exclusive-locked scope
    w.set(serverId, addr("10.0.0.1"), now);
    w.set(portId,   intv(123),        now);
    w.set(vrfId,    str("mgmt"),      now);
}   // commit: lock released
```

Because the whole update is one scope, a concurrent `collectLeaves("/system/ntp")`
(shared lock) blocks until commit and so **never observes a half-updated record**
(e.g. new `server` with old `port`). Splitting the three `set()`s across two
`writeValues()` scopes would expose exactly that inconsistent window — hence the
rule: **all members of an atomic group are updated in one scope** (D6/D14).

On the wire (any member changed → atomic re-send of subscribed members):
```yaml
Notification:
  timestamp: max(collectedNs of server, port, vrf)   # D14 timestamp collapse
  prefix: /system/ntp
  atomic: true
  update: [ server=10.0.0.1, port=123, vrf=mgmt ]    # subscribed members only (§3.5.2.5)
```

### Scenario 7 — One node, many spellings, one leaf (D16)

A PSU branch is registered (via `attachSubtree`, D12) with a leaf at the
canonical path

```
/components/component[class=POWER_SUPPLY][name=psu0]/state/temperature
```

Three different clients address what is the *same* gNMI node with different
spellings. After `canonicalize()` (D16) all three produce the same `CanonicalPath`
and hit the same `LeafEntry`:

| Client request path                                                         | What differs            | Canonicalised to |
|-----------------------------------------------------------------------------|-------------------------|------------------|
| `/components/component[name="psu0"][class=POWER_SUPPLY]/state/temperature`  | reversed key order, quotes | same key |
| `/components/component[class=POWER_SUPPLY][name=psu0]/state/`               | trailing slash (subtree) | same prefix, captures the leaf |
| `(origin unset) /components/component[name=psu0][class=POWER_SUPPLY]/state/temperature` | omitted origin | boundary defaults to `openconfig`, then strips it (D16 O2) |

Without D16 the first would be a **missed subscription** (different string → no
map hit) and the second would be a **silent auto-assign / capture failure**
(`/state/` prefix rejects `/state/temperature`). A fourth client sending
`origin: "cli"` is rejected at the boundary with **`UNIMPLEMENTED`** (it names a
schema the mock does not implement, §3.3.4 L1152 / §3.5.2.4 L1900, finding **N**)
— the mock serves a single OpenConfig origin (D16 O2), and the failure is
explicit rather than a silent miss.

---

## Still TBD (next decisions, tracked in design-review.md)

- **Protocol layer** — now designed in its own doc, **`protocol-layer-design.md`**,
  on top of the stable core (`collectLeaves`, `collectForSubscription`,
  `ValueWriter`, `attach/detachSubtree`, the `changeSeq`/change-set seam). Decided
  so far: P1 — ON\_CHANGE is **push-native** via `ILeafSink` (D6 realized). Open
  there: P2 threading, P3 notification builder, P4 monitored-set re-expansion,
  P5 TARGET\_DEFINED policy.
