# ON_CHANGE Delivery & Source Binding — Design Discussion

> **Status: EXPLORATORY / not yet decided.** This is a forward-looking design
> discussion, deliberately kept separate from `DESIGN.md` (which records *what is
> implemented*). The changes contemplated here (push delivery, a write-side
> source seam, batch/commit semantics, `duplicates` accounting) are expected to
> be a **large change** and are not committed to. This document preserves the
> reasoning, the doubts, the alternatives considered, and the provisional
> verdicts — so a future implementer inherits the *why*, not just the *what*.
>
> **Update:** the **batch/commit write-coherence seam has since shipped** as
> `WriteBatch` + `LeafStore::commit` (see §3.3) — that part is no longer
> hypothetical. Push delivery and the `duplicates`/`writeSeq` accounting remain
> future work. Phase 3 ON_CHANGE as shipped is poll-based and correct (see
> `DESIGN.md` → "Phase 3 — ON_CHANGE").

## Why this document exists

Three things became clear while reviewing the shipped ON_CHANGE implementation:

1. The detection mechanism (poll vs push) is an **implementation choice the spec
   does not mandate** — so it is ours to decide on engineering grounds.
2. The *meaning* of "a value changed" for analog/float data is **not a gNMI
   concern** — it belongs to the data source, and that realization dictates
   where the seam between gNMI and the data source must sit.
3. The real data source will not be the simulator. The likely shape is a service
   (e.g. a D-Bus bridge) that polls hardware and publishes values, which the gNMI
   service then consumes. We do not need to build that now, but we must design
   the **gNMI-side interface** so it binds cleanly to *any* such source — D-Bus
   or otherwise.

All three converge on one place: **the write-side boundary** (how values enter
the store). That is the thing worth designing carefully now; the read side
(`snapshot()` / `diff()` / emit) barely moves.

---

## 1. Detection mechanism: poll vs push

### 1.1 How it works today (poll, server-side)

STREAM ON_CHANGE detection is entirely **target-side**. The client opens the
subscription once and never polls (polling is the POLL mode). Inside
`handleStream()`'s loop, every ≤200 ms the target:

```
cur  = registry_.snapshot(query)     // subscribe.cpp
diff = LeafStore::diff(prev, cur)     // value comparison only
if diff non-empty: stream->Write(Notification)   // push to client
prev = cur
```

The writer (`LeafStore::set()` / `remove()`) and the subscriber are **decoupled**;
the only shared state is the store's map under a `shared_mutex`. The writer does
**not** notify anyone — the subscriber *discovers* changes by polling and
diffing. "Trigger" is therefore not an event; it is "the next poll observed a
difference."

### 1.2 What the spec actually mandates

Source: `spec/reference/rpc/gnmi/gnmi-specification.md`.

§3.5.1.5.2 (STREAM / ON_CHANGE):
> data updates are **only sent when the value of the data item changes**. ... the
> target **MUST** first generate updates for all paths ... Following this initial
> set, updated values **SHOULD** only be transmitted when their value changes. ...
> heartbeat ... **MUST** be re-sent once per interval.

The hard rules are all **client-observable contract**, not mechanism:
- initial snapshot **MUST** be sent;
- heartbeat **MUST** be sent;
- "only on change" is a **SHOULD** (not MUST).

The spec is **silent** on: poll vs push, any detection interval, and any maximum
latency between a change and its emission (there is no "MUST emit within X ms").

Moreover, the `Update.duplicates` field (§2.1) **explicitly blesses** discarding
intermediate values:
> If a client is unable to keep up with the server, **coalescion can occur on a
> per update basis such that the server can discard previous values** ... and
> return only the latest. ... the server **SHOULD increment a count**
> (`duplicates`).

That is exactly what poll+diff does (two changes within one 200 ms window → only
the latest is emitted). **Conclusion: our poll is spec-legal.** See §4 for the
honesty obligation this creates (`duplicates`).

> **Note / known gap:** we do not currently set `duplicates` when coalescing.
> Spec is SHOULD, and in the current mock the write rate is far below the drain
> rate so no coalescing actually occurs — but the data model should be ready
> (see §4).

### 1.3 Efficiency comparison

| Axis | Poll (current) | Push (event-driven) |
|------|----------------|---------------------|
| Idle cost | O(subscribers × leaves) every 200 ms, **even when nothing changes** | **~0** — no work without an event |
| Latency (change→emit) | up to 200 ms | near-immediate |
| Scaling in idle subscribers | poor (constant background work) | good |
| Scaling in change rate | independent of changes (wasteful) | proportional to changes |

For ON_CHANGE, **push is the more natural and more efficient model**, especially
for rarely-changing event data (alarms): poll spins diffing unchanged state;
push does nothing until the event.

### 1.4 The nuance that matters: where do changes originate?

Our data source is itself a **periodic poller** (the simulator's 1 s tick; a real
PSC register poller; a D-Bus bridge polling hardware). The writer therefore has a
natural "I just refreshed a batch of values" moment at the **end of each tick** —
the ideal point to fire a notification. Push does **not** require hardware
interrupts; it only requires the writer to signal at its tick boundary.

This also means the subscriber's extra 200 ms poll is **redundant work on top of
a source that already has a well-defined update cadence**. The detection
granularity is already bounded by the source's tick.

**Provisional verdict:** push is the better *end-state* for ON_CHANGE
(efficiency + latency), using the writer's tick/commit as the signal source.
**But** at the current scale (1 provider, a handful of leaves, few subscribers)
the 200 ms poll's waste is negligible. Push's value materializes only when:
many subscribers, many leaves, low change rate (idle waste), or latency-sensitive
events (alarms). **For now, keep poll; record push as a future optimization.**

### 1.5 If/when we go push: complexity & risk

Two very different implementations:

**A. Minimal hybrid (CV + version) — recommended push form. Moderate complexity,
low regression risk, not a big refactor.**

- Add to `LeafStore`: a `condition_variable_any` + a monotonic `version_`
  bumped on every `set()`/`remove()` **under the write lock**; plus a
  `waitForChange(lastSeenVersion, timeoutCap)`.
- `handleStream()` ON_CHANGE: replace `sleep_for(200 ms)` with
  `store.waitForChange(...)`; on wake, still do `snapshot()` + `diff()`.
- **`diff()`, `emitDiff`, `emitSnapshot`, per-subscriber `prev`,
  `resolveStreamMode` are all unchanged** → existing unit tests stay valid →
  low regression risk.
- Heartbeat is preserved via the wait's timeout cap (cap = time to next
  heartbeat).

Risks / things to get right:
1. **Multi-store wait (the main design cost):** a query may span multiple
   providers/stores, but one CV can only be waited on once. Cleanest fix: a
   **single shared change signal** (registry-level "change bus" that any
   provider's `set`/`remove` notifies), and the subscriber waits on that one
   signal. Waiting on several CVs is the awkward path to avoid.
2. **Lost/spurious wakeups:** bump `version_` under the *same lock* as the data
   write; check the version predicate on wake.
3. **Cancellation/shutdown:** a blocking wait must still notice
   `context->IsCancelled()`. Simplest: cap the timed wait (e.g. ≤1 s) so
   cancellation stays responsive without wiring cancellation into the CV.
4. Precedent: the simulator already uses `cv.wait_for` for its own tick — the
   pattern is not foreign to the codebase.

**B. Full observer/callback registry — high complexity, higher risk.** Pushing
"exactly what changed" to each subscriber without diffing requires subscriber
registration/deregistration, per-query filtering *at notify time*, lock ordering
on callbacks (must not call back while holding the write lock), and backpressure.

> **Revised after reading the reference (see Appendix A):** this "Option B" is in
> fact exactly what openconfig/gnmi does (path `match`er + per-client `coalesce`
> queue + change-detect-at-write). The dismissal above held for a naive C++ port;
> but gRPC's thread-per-RPC absorbs the "goroutine per client", and change-at-write
> removes the diff *and* yields `duplicates` for free. Appendix A re-frames B as the
> **canonical target** if push is pursued. The minimal hybrid (A) becomes merely a
> faster intermediate step.

---

## 2. What counts as a "change"? (float precision / deadband)

### 2.1 Current behavior

`LeafStore`'s `valueEquals` compares `a.SerializeAsString() == b.SerializeAsString()`
— **exact** IEEE-754 equality for doubles. So 25.001 → 25.002 currently counts as
a change.

### 2.2 The doubt, and why it resolves outside gNMI

"Is a tiny float wiggle a real change or noise?" is **sensor domain knowledge**,
not a gNMI concern. Putting a deadband into `diff()` would force the gNMI layer to
know per-leaf thresholds (temperature 0.5 °C, voltage 0.1 V…), polluting a layer
that should be domain-agnostic.

Correct layering:

| Layer | Owns quantization / deadband |
|-------|------------------------------|
| Hardware | ADC physical resolution |
| **Source adapter** (D-Bus bridge / provider's writer thread) | quantize to the granularity ON_CHANGE should react to, **before** `store.set()` |
| gNMI `diff()` | **exact equality** over already-quantized stored values |

The simulator already demonstrates this: it performs a **grid-quantized** random
walk (steps of one `w.step` unit), so stored values are discrete and exact
comparison is noise-free.

### 2.3 The hard constraint that makes this the *only* sensible layering

gNMI provides **no ON_CHANGE deadband knob**. (`suppress_redundant` is SAMPLE-only,
and is exact-change suppression, not a deadband.) Therefore:

> **ON_CHANGE granularity == the source's storage quantization.** To make
> ON_CHANGE react more coarsely, quantize more coarsely at the source adapter.
> You cannot ask gNMI for a deadband, because the protocol has no field for it.

A considered sub-doubt: *what if the same source feeds a SAMPLE subscriber wanting
full resolution and an ON_CHANGE subscriber wanting a deadband?* Since there is no
per-subscription deadband in the protocol, you cannot satisfy both from one stored
value via gNMI policy. The resolution stored is the resolution everyone sees. If
that conflict ever becomes real, it must be solved at the source (e.g. publish two
representations), not in gNMI.

**Verdict: gNMI `diff()` stays exact-equality; deadband is a source-adapter
responsibility.**

### 2.4 Same value, new sample time → not a change (already correct)

`diff()` compares **value only, never the timestamp** (by design — see the
`diff()` comment in `leaf_store.cpp`). A re-sample that yields the same value is
**not** emitted. This is exactly right: ON_CHANGE tracks "the value changed", not
"we sampled again". No change needed.

### 2.5 Do sensors belong in ON_CHANGE at all?

Instinct ("probably not") is largely right, and the spec backs the distinction.
§3.5.1.5.2 (TARGET_DEFINED):
> if the path ... refers to ... leaves which are **event driven** ... then
> `ON_CHANGE` may be created, whereas if other data represents **counter values,
> a `SAMPLE`** subscription may be created.

- Continuous analog quantities (temperature, voltage, current) are **SAMPLE**-natured
  — which is why `PscPowerSensorProvider::preferredMode()` returns SAMPLE.
- Event/state quantities (alarms, admin-state, link up/down, enum states) are
  **ON_CHANGE**-natured.

Precise statement (not "sensors can't use ON_CHANGE"): **SAMPLE is the natural
default for sensors; ON_CHANGE on a sensor is only meaningful when the value is
quantized into discrete steps** (then it emits only on step crossings).
TARGET_DEFINED lets the provider decide per leaf — the spec-blessed way to express
this.

---

## 3. The write-side seam / source binding (the convergence point)

Focusing on gNMI design: **the gNMI side must never know whether data comes from
D-Bus, a hardware poller, or a test.** That decoupling boundary **already exists**
— it is `LeafStore`'s write API (`set()` / `remove()`). Whatever the source, it
pushes into that sink; the gNMI side reads via `snapshot()` / `collect()`.

Two real decisions shape this seam:

### 3.1 Decision (a): driver inside the provider vs external writer

This directly decides whether the deferred "store accessor" is ever needed.

- **Driver inside the provider (recommended):** the source adapter lives in the
  provider's writer thread (today's simulator `jthread`; tomorrow a thread that
  polls D-Bus and calls `store_.set()`). `store_` stays **private** — no accessor
  needed. Swapping data sources = swapping the driver, *not* exposing the store.
- **External writer:** only needed if a separate object (a shared D-Bus service
  pushing into many providers, or a test thread) must write the store. *That* is
  the only case requiring the deferred `LeafStore& store()` accessor.

**Verdict: keep the driver inside the provider; keep `store_` private.** Source
differences become "the provider runs a different driver."

### 3.2 Decision (b): per-leaf `set()` vs batch `commit()`

- A D-Bus poll returns a **batch** of values. Per-leaf `set()` × N reads
  semantically as "N independent events". A batch (`set...` then `commit()`)
  expresses "these N leaves share one tick and one collection time T".
- Batch `commit()` unlocks two things at once:
  - **(i) Honest bundling** (spec §3.5.2.1 — only leaves sharing timestamp T
    should be bundled into one Notification);
  - **(ii) In a push design, one notify per commit** instead of N.

### 3.3 The seam as built — `WriteBatch` + `LeafStore::commit` (IMPLEMENTED)

> **Status: BUILT.** The write-coherence half of atomic shipped as a `WriteBatch`
> value object committed atomically, *not* the stateful `ILeafSink` sketched
> below. This section records what landed and why it deviates from the original
> sketch.

```cpp
// data_provider.hpp — store-agnostic write model. A driver (gNMI Set, simulator,
// config seed) assembles a batch; the store applies it under one lock.
class WriteBatch {
    WriteBatch& set(const std::string& xpath, /* typed | TypedValue */,
                    int64_t collectedNs);   // fluent, chainable
    WriteBatch& remove(const std::string& xpath);
    const std::vector<WriteOp>& ops() const;
};
// LeafStore::commit(const WriteBatch&) takes ONE unique_lock for the whole batch.
// IDataProvider::applyBatch(const WriteBatch&) is the provider write side;
// DataProviderRegistry::commit() partitions a batch by owning prefix.
```

**Why `WriteBatch` and not the stateful `ILeafSink` (set/remove/buffer/commit):**
the original sketch had the sink accumulate pending ops between `set()` calls and
flush on `commit()`. But `LeafStore` has **two concurrent writers** — the
simulator's `jthread` and the gNMI Set RPC thread — so a single shared mutable
pending buffer is a data race that would need a per-writer transaction handle to
make safe. An explicit `WriteBatch` value object (the RocksDB/LevelDB idiom) side-
steps that entirely: each writer builds its own batch, hands the immutable result
to `commit()`. No shared write-side state, thread-safety is trivial.

This also resolves the open shape questions: value typing **mirrors the typed
overloads** (with a `TypedValue` overload for the Set path); `commit()` is
**explicit** (the batch boundary is the transaction); and `commit()` is the
single place a future push signal (§1.5) would be raised — every writer, sensor
tick included, already funnels through it.

---

## 4. Coalescing / `duplicates`

Two implications for our direction:

1. **It legalizes poll.** The spec permits discarding intermediate values within
   a window and returning only the latest — exactly poll+diff behavior (§1.2).
2. **Honesty requires counting what was dropped.** `duplicates` is "how many
   updates were coalesced because the client couldn't keep up". To report it, the
   store must keep a **monotonic write sequence per leaf**; at emit the subscriber
   computes `duplicates = (cur.writeSeq − prev.writeSeq) − 1` (intermediate values
   discarded).

Practical stance:
- In the current mock, write rate (1 s/tick) ≪ drain rate → no coalescing →
  `duplicates` always 0. **No counter needed yet.**
- But **reserve it in the data model now**: add a `writeSeq` to `Leaf` (cheap) so
  enabling `duplicates` later does not change the snapshot structure.
- **Push + batch `commit()` makes `duplicates` easier than poll** — you increment
  at the explicit "merge into the pending entry" point, rather than reconstructing
  it from sequence deltas.

> Doubt acknowledged: is `duplicates` over-engineering for a mock? For *now*, yes
> to implement; **no** to ignore in the data model. Reserving `writeSeq` keeps the
> option cheap.

---

## 5. Convergence: it all lands on the write-side boundary

- **Deadband** → quantize at the source adapter.
- **Batch `commit()`** → honest bundling + push notify point.
- **`duplicates`** → per-leaf `writeSeq` recorded by the writer.
- **Push** → the writer's `commit()` is the signal source.

Every one of these is a property of **how values enter the store**, i.e. the
`ILeafSink` / source-binding boundary. The read side (`snapshot()` / `diff()` /
`emitDiff` / `emitSnapshot`) stays essentially unchanged. So the design work to do
*first* is the shape of that write-side seam; the delivery-mechanism switch
(poll→push) is a second, separable step that reuses the same `commit()` signal.

---

## 6. Open decisions (to settle before any implementation)

1. **Switch to push now, or stay poll?** — **Decision: stay poll.** It is
   spec-legal (§1.2) and adequate at current scale (1 provider, ~7 leaves, few
   subscribers); ON_CHANGE on sensors is marginal anyway (§2.5). Building the
   router/queue now would design APIs against imagined needs — the same
   speculation `DESIGN.md` §A avoids for the StoreBackedProvider base. See §6.1
   for the trigger that flips this.
2. ~~**`ILeafSink` shape**~~ — **RESOLVED / BUILT** as `WriteBatch` +
   `LeafStore::commit` (§3.3): typed overloads + a `TypedValue` overload, explicit
   `commit()`, and `commit()` is the future push-signal point. Every writer (gNMI
   Set, simulator tick, config seed) now funnels through it under one lock.
3. **Push form** — if/when: minimal hybrid (CV + version, keep diff), *not* an
   observer/callback registry.
4. **Multi-store wake** — a single registry-level change signal vs per-store CVs
   (recommended: shared signal).
5. **`writeSeq` / `duplicates`** — reserve `writeSeq` in `Leaf` now; implement the
   counter only when coalescing can actually occur.

### 6.1 The next provider — a deterministic, independent ON_CHANGE source

To make ON_CHANGE non-speculative we need a **second provider that is genuinely
ON_CHANGE-natured**, ideally one that also exercises the untested `remove()` →
`Notification.delete` path and gives a **deterministic** trigger (the random-walk
sensor cannot).

**Rejected — presence / alarms on `/components/component/...`.** An obvious idea is
a "PSU presence / hot-swap" or "alarm" provider modelling component appearance and
removal. It is rejected because of **lifecycle coupling**: a PSU under
`/components/component[name=PSC-x]` shares its instance with the existing sensor
provider, so removing it must cascade-remove that component's sensor leaves — which
live in a *different* provider's store. Two providers coordinating one entity's
lifecycle breaks the "one provider owns one namespace" model. Deeper truth:
presence/lifecycle is a property of the entity that *owns* the sensors, so it
belongs *inside* the sensor provider, not beside it. Splitting it is artificial and
future-confusing.

**Chosen — a system-level configuration provider** (`SystemConfigProvider` over
`/system/config/...`: e.g. `hostname`, `login-banner`, `motd-banner`,
`/system/clock/config/timezone-name`; openconfig-system). Note: **system-level**,
not component-level config — component config (`/components/component/.../config`)
would re-entangle with the `/components` tree we are trying to stay disjoint from.

Why it fits every requirement:
- **Disjoint namespace, no shared lifecycle** (`/system/config` vs
  `/components/component`) → zero cascade, zero cross-provider coordination.
- **`config true` (writable)** → `Set` → store is **semantically correct** (unlike
  sensors, which are read-only `state` — the original reason `Set`→store was
  deferred).
- **ON_CHANGE-natured** — config changes only when configured; naturally event-like.
- **Deterministic trigger, finally.** Driven by the real gNMI `Set` RPC, which has
  both `update` and `delete`:
  - `Set update /system/config/hostname` → ON_CHANGE subscriber sees an Update;
  - `Set delete /system/config/login-banner` → store `remove()` → subscriber sees a
    `Notification.delete`.
  Both deterministic, both via a real RPC, both semantically correct. This is the
  manual ON_CHANGE trigger we lacked **and** it covers the delete path — so presence
  is unnecessary even for delete coverage.

**Architecture it unlocks — a symmetric write side.** `Set` is currently a no-op;
wiring it makes routing symmetric:

```
read:   get/subscribe → registry.fill()/snapshot() → provider.fill()/snapshot() → store.collect()/snapshot()
write:  set           → registry.set()/delete()    → provider.set()/remove()    → store.set()/remove()
```

`IDataProvider` gains an **optional write side** (`set()`/`remove()`) implemented
only by writable providers (config), refused by read-only ones (sensors); the `Set`
RPC routes by prefix through the registry. **No public `store()` accessor is needed**
— writes route the same way reads do. This is the canonical gNMI-target shape.

**Sequencing:** build `SystemConfigProvider` + the registry write fan-out on the
*existing* poll-based ON_CHANGE (already works, including delete). It also makes the
**second store-backed provider**, so `DESIGN.md` §A (`StoreBackedProvider` base) can
emerge from real duplication. `atomic` framing stays a separate later step (§7).
Switching the ON_CHANGE bucket to reference-style push (Appendix A) remains gated on
real scale / latency, independent of this provider.

## 7. Deferred / explicitly out of scope right now

- Push delivery (this whole document is the rationale for a *future* change).
- ~~The `ILeafSink` / batch-commit seam~~ — **BUILT** as `WriteBatch` +
  `LeafStore::commit` (§3.3). The write side is now coherent (one lock per
  transaction); push delivery on top of `commit()` remains the deferred part.
- `LeafStore& store()` accessor — **not** added; writes route through the registry
  (§6.1), reads already do, so no public accessor is needed.
- `Set` → store injection — **no longer a deferred orphan**: it is the reason
  `SystemConfigProvider` exists (§6.1), and is semantically correct there because
  config is writable. The earlier objection (sensors are read-only `state`; the
  simulator would overwrite) only applied to wiring `Set` into the *sensor* store.
- `atomic` framing (`Notification.prefix` + `atomic=true` bundle; spec §2.1.1) — a
  subscribe-emit contract change (a provider declares an atomic group), **not** a
  free provider flag. Deferred as its own step; best demonstrated later on a coupled
  config record.
- `suppress_redundant` (SAMPLE-only, P3) — independent of this discussion.

## 8. Relationship to current code

Nothing in this document is implemented. The shipped ON_CHANGE (poll, exact-equality
diff, simulator writer) remains correct and spec-legal. When/if this direction is
pursued, the expected touch points are:

- `LeafStore` — `ILeafSink`, `commit()`, optional `version_` (push) and `writeSeq`
  (`duplicates`).
- provider — driver swap (simulator → D-Bus/hardware poller), store stays private.
- `subscribe.cpp` `handleStream()` — one line: `sleep_for` → `waitForChange` (push).
- read side (`snapshot`/`diff`/`emit*`) — unchanged.

See `DESIGN.md` → "Phase 3 — ON_CHANGE" and §C for the as-built state this would
evolve from.

---

## Appendix A — Reference implementation digest (openconfig/gnmi, Go)

Source: `spec/gnmi/{subscribe,cache,coalesce,match,ctree}`. This is the canonical
Google implementation; it is assumed faithful to the gNMI spec, so it carries
weight for deciding our direction. Digesting it **largely vindicates push** — and
shows the canonical structure is essentially the "full observer/coalesce" model
this doc earlier labelled Option B.

### A.1 Architecture (confirmed by reading the code)

It is **pure push, change-detected-at-write, with a per-client coalescing queue**:

```
collector → cache.GnmiUpdate(notification)                         [cache/cache.go:368]
   └─ per leaf: target.gnmiUpdate(n)                               [cache/cache.go:499]
        - compares old vs new: value.Equal(old, new)               [cache/cache.go:572]
        - if eventDriven && unchanged  → return nil (SUPPRESS, no push)
        - else update ctree leaf in place, return the leaf
   └─ for each non-nil leaf: cache.client(leaf)  == Server.Update  [cache.SetClient, cache.go:190]
        └─ match.Update(path) → for each client whose query matches:[match/match.go:103]
             matchClient.Update(leaf) → queue.Insert(leaf)         [subscribe.go:378]
                                          (coalesce by identity, count dups)

per client: one goroutine sendStreamingResults                     [subscribe.go:476]
   └─ queue.Next(ctx) → (leaf, dupCount)   [blocks until insert/close, coalesce.go]
   └─ MakeSubscribeResponse(leaf.Value(), dup)  [sets Update.duplicates] [subscribe.go:555]
   └─ stream.Send(notification)
   └─ if a Send blocks past timeout → close the subscription       [subscribe.go:494]
```

### A.2 Component-by-component

- **Change detection lives at the WRITE** (`cache.go:570-575`). Event-driven
  emulation (on by default; `DisableEventDrivenEmulation()` opts out) compares the
  new value to the stored one and **suppresses unchanged non-atomic updates**
  (returns `nil`, so nothing is pushed). There is **no subscriber-side snapshot
  diff and no per-subscriber `prev` state** — the "diff" is a single value compare
  at insert time. (Atomic notifications bypass suppression — they always push.)
- **`match` is a path-trie subscription router** (`match/match.go`): `AddQuery`
  registers a client's paths; `Update(n, path)` invokes `client.Update` for every
  matching query. This is the "who wants this path" registry; the writer fans out
  only to interested clients.
- **`coalesce.Queue` is per-client, identity-keyed, dup-counting** (`coalesce.go`):
  `Insert` coalesces repeated items (incrementing a per-item count) instead of
  growing the queue; `Next` blocks and returns `(item, dupCount)`. Because the
  queue holds the **leaf pointer** (whose value the cache updated in place), a
  coalesced consumer reads the **latest** value and learns how many were dropped —
  this *is* the spec's `duplicates`. It is set on the first Update of the response
  (`subscribe.go:555+`).
- **One goroutine per subscription** drains its queue and Sends (`subscribe.go:476`).
  Slow clients are bounded: if a Send can't complete within a timeout, the
  subscription is closed (`subscription timed out while sending`). Coalescing
  absorbs bursts; the timeout kills hopeless consumers.
- **Initial walk:** `processSubscription` (`subscribe.go:397`) queries the cache
  once for matching leaves, inserts them, then a `syncMarker` → `sync_response`.
  `updates_only` inserts the sync marker without the initial walk.

### A.3 Two divergences from our server that matter

1. **The reference does NOT implement SAMPLE.** Grepping `subscribe/` and `cache/`
   for `sample_interval` / `heartbeat` / `suppress_redundant` returns **nothing**.
   The mode switch handles only `ONCE`, `POLL`, `STREAM` (`subscribe.go:299`), and
   `STREAM` = push every cache change. The openconfig model assumes the
   collector/upstream decides sampling cadence and feeds the cache; the cache just
   streams changes. **Consequence for us:** we cannot wholesale adopt it — our
   SAMPLE timer path (sample_interval, and the future heartbeat/suppress_redundant)
   has *no reference here* and must stay our own. A reference-aligned design keeps
   the SAMPLE-by-timer bucket and swaps only the ON_CHANGE bucket to push.

2. **Change-at-write vs our change-at-read.** We currently poll `snapshot()` +
   `diff()` per subscriber. The reference compares old-vs-new once at `set()` time
   and pushes only the changed leaf. Theirs needs no per-subscriber diff state but
   needs the matcher + per-client queue; ours needs no matcher/queue but pays a
   periodic diff per subscriber.

### A.4 What this means for our C++ direction

The reference is "Option B" (full observer + coalesce) — which this doc earlier
called high-complexity/not-recommended. Two things change that calculus:

- **gRPC C++ already gives one thread per RPC.** `handleStream` *is* the per-client
  goroutine. So "goroutine draining a queue" maps to "`handleStream` blocks on its
  own coalescing queue" — no thread pool needed, the structure already fits.
- **Change-at-write removes the diff** and yields `duplicates` for free, which the
  minimal-hybrid (CV + version + keep diff) does not.

Re-framed options for the ON_CHANGE bucket:

| | Machinery to build | Diff? | `duplicates`? | Canonical? |
|--|--|--|--|--|
| **Poll (today)** | none | per-subscriber, periodic | manual (writeSeq) | no |
| **Minimal hybrid** | CV + `version_` in store; `waitForChange` | keep diff | not natural | no |
| **Reference-style** | change-detect in `set()`; a `match`-style router; per-client coalescing queue | **none** | **built-in** | **yes** |

**Revised recommendation:** if push is pursued, aim for the **reference-style**
target (change-at-write + path router + per-client coalescing queue), because it is
canonical, deletes the diff, and gives `duplicates` natively — and the C++ cost is
mitigated by gRPC's thread-per-RPC. The minimal hybrid is now only attractive as an
intermediate step if we want to ship push fast without building the router/queue.
**Keep the SAMPLE-by-timer bucket regardless** — the reference does not cover it.
Still no reason to do any of this until scale/latency/alarms demand it.

Mapping of reference pieces → C++ (when/if built):
- `cache.gnmiUpdate` suppress-unchanged → `LeafStore::set()` compares old vs new,
  enqueues only on change (and on add/remove). The store becomes the change source.
- `match.Match` → a registry-level path→subscriber router (our `DataProviderRegistry`
  gains a subscriber-matching side, or a sibling component).
- `coalesce.Queue` → a small thread-safe coalescing queue (mutex + condvar +
  identity-keyed dup map); `Next()` blocks like the Go version.
- `sendStreamingResults` → `handleStream`'s ON_CHANGE bucket becomes
  `item = queue.next(); send(item, dup)` instead of poll+diff.
- per-leaf `writeSeq` (§4) becomes unnecessary — coalescing counts dups directly.
