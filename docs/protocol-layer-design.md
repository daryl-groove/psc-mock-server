# Protocol Layer — Design

The gNMI protocol layer sits on top of the stable core data layer
(`core-data-model-design.md`) and turns gNMI RPCs into reads/writes against the
`LeafRegistry`, and registry state/changes into gNMI `Notification`s on the wire.

**Scope.** Subscribe (ONCE / STREAM / POLL; ON\_CHANGE / SAMPLE / TARGET\_DEFINED),
the notification builder, the steady-state change source, and the core↔protocol
push seam (`ILeafSink`). Get/Set RPCs and the gRPC transport/threading mechanics
are referenced where they shape these but are detailed separately.

This doc is decision-by-decision like the core doc; protocol decisions are
numbered **P1, P2, …** to avoid clashing with the core's `D1…D17`.

---

## What the core already provides (the seams we build on)

The core was shaped so the protocol layer is thin. From
`core-data-model-design.md`:

- **`collectForSubscription(query) → SubscriptionView{leaves, groups}`** (D13) —
  one shared-lock setup query: §2.4.2 container expansion **and** the owning
  groups (with `GroupView.memberPaths`, the full member list needed for atomic
  monitored-set expansion). Mutually consistent (no TOCTOU).
- **`collectLeaves(prefix) → LeafSnapshot`** (D7/D17) — full-value collection for
  initial sync / SAMPLE / POLL / ONCE; values are COW `shared_ptr` handles, cheap
  to copy and zero-copy to forward.
- **`changeSeq`** (D14) — the single comparison that drives both ON\_CHANGE
  change-detection and SAMPLE `suppress_redundant` (§3.5.1.5.2).
- **`ValueWriter` change-set** (D6) + **attach/detach** structural events — the
  event source a push model fires from.
- **`shared_ptr<const TypedValue>` values** (D17) — forwarded into a Notification
  with no protobuf copy.
- **Path identity is `CanonicalPath`, single-origin** (D16) — the protocol layer
  owns `origin`: it defaults empty→`openconfig`, validates it, and strips it
  before calling the core. (Re-attaching it onto response paths is deferred — see
  the note.) An origin that is *syntactically valid but not `openconfig`* names a
  schema the server does not implement, so it is rejected with **`UNIMPLEMENTED`**,
  not `InvalidArgument` — per the Get/Subscribe behavior tables (§3.3.4 L1152 /
  §3.5.2.4 L1900, "syntactically correct but … not implemented → `UNIMPLEMENTED`").
  `InvalidArgument` is reserved for a *malformed* `gnmi::Path`. (Resolves
  design-review finding **N**; see the RPC status-code table below.)
  > ✅ **Implemented (backlog C1).** `validateOrigin()` (`utils/utils.h`), shared by
  > get/set/subscribe (empty/`openconfig`→OK, other→`UNIMPLEMENTED`); `gnmi_to_xpath`
  > no longer embeds origin, so stripping is free at every call site. **Re-attach on
  > responses was deferred** (decided strip-only): the spec mandates only `target`
  > echo (C5), not origin, and empty origin ≡ `openconfig` by client convention — a
  > D16 extension point for when multi-origin is actually needed.

---

## Pipeline and components

```
SubscribeRequest
  └─► (1) Subscription Resolver ── core.collectForSubscription ─► Monitored Set {leaves, groups}
        └─► (2) Initial Sync ── core.collectLeaves ─► (3) Notification Builder ─► send
              └─► sync_response = true                        (skipped if updates_only, §3.5.1.2)
                    └─► (4) Steady-state Driver  per subscription mode:
                           ON_CHANGE ─┐
                           SAMPLE   ──┤── (5) Change Source ─► (3) Notification Builder ─► send
                           POLL/ONCE ─┘
```

1. **Subscription Resolver** — validates origin (D16), expands each requested
   path to a monitored set via `collectForSubscription`. Atomic groups pull in
   their *unsubscribed* members for re-send scope (Scenario 2).
2. **Initial Sync** — ON\_CHANGE "first generate updates for all paths"
   (§3.5.1.5.2), ONCE/POLL "send the set", then a `SubscribeResponse{sync_response=true}`
   (§3.5.1.4). Skipped (only `sync_response` sent) when `updates_only` is set.
3. **Notification Builder** — `{leaf snapshots, group views, reason}` →
   `Notification`(s): atomic members in one Notification (prefix + `atomic` +
   timestamp collapse, D14); non-atomic / ungrouped per-leaf or bundled;
   branch-level `delete` on detach (§3.5.2.3); `gnmi::Path` re-encoded with origin.
4. **Steady-state Driver** — per-mode lifecycle (below).
5. **Change Source** — how steady state learns what changed: **push** for
   ON\_CHANGE (P1), **timer** for SAMPLE.

### Per-mode lifecycle (spec §3.5.1.5)

| Mode | Initial | Steady state | Close |
|---|---|---|---|
| ONCE | send set + `sync_response` | — | RPC closes (§3.5.1.5.1) |
| STREAM / ON\_CHANGE | send all + `sync_response` | push on change; `heartbeat_interval` forces periodic re-send | long-lived |
| STREAM / SAMPLE | send all + `sync_response` | timer per `sample_interval`; `suppress_redundant` skips unchanged via `changeSeq`; heartbeat overrides | long-lived |
| STREAM / TARGET\_DEFINED | send all + `sync_response` | target picks ON\_CHANGE or SAMPLE per leaf (P5) | long-lived |
| POLL | `sync_response` (set sent per Poll) | send full set on each `Poll` msg | long-lived |

---

## Decisions

### P1 — ON\_CHANGE is push-native (decided)

**Steady-state ON\_CHANGE is driven by push, not polling.** On a `ValueWriter`
commit (and on `attach`/`detachSubtree`), the registry dispatches the change-set
through an **`ILeafSink`** listener; the protocol layer routes each changed
`LeafId`/path to the subscriptions whose monitored set covers it, wakes that
subscription's sender, and it builds + writes the Notification. A value change is
therefore reflected on the wire immediately, not at the next poll tick. **SAMPLE
remains timer-driven** (sampling is inherently time-based); it reads current
state on each tick and uses `changeSeq` for `suppress_redundant`.

```cpp
// core→protocol seam (lives at the core boundary, core stays subscription-ignorant).
// ONE unified event carries value changes AND structural adds/removals.
// Payload is enriched (captured at commit) so the sink never re-locks the core.
// [Shape finalized in P3: R1 enriched payload, R2 unified entry point.]
struct LeafChange {                          // one changed/added leaf, captured at commit
    LeafId                                  id;
    std::shared_ptr<const CanonicalPath>    path;        // shared handle (D16/L=B) — zero-copy, refcount-safe
    uint64_t                                changeSeq;   // D14 change token
    int64_t                                 collectedNs; // wire-timestamp source (D14)
    std::shared_ptr<const gnmi::TypedValue> value;       // D17 COW handle — zero-copy
};
struct ChangeBatch {                         // OWNS its records (must outlive the writer scope)
    std::vector<LeafChange>    changed;          // ValueWriter commit (value writes)
    std::vector<LeafChange>    added;            // attachSubtree / registerLeaf
    std::vector<CanonicalPath> removedPrefixes;  // detachSubtree / unregisterLeaf|Group
};
class ILeafSink {
public:
    virtual ~ILeafSink() = default;
    virtual void onChange(const ChangeBatch&) = 0;   // single entry point
};
// LeafRegistry holds an optional ILeafSink*; dispatch is a no-op when unset
// (poll/test paths keep working without a listener). The batch is dispatched as a
// shared_ptr<const ChangeBatch> so fan-out to many Subscriptions is a refcount bump
// and nothing dangles after the ValueWriter scope exits (L=B; see core D6).
```

**Why push-native (vs poll-first).** The end-state is push (core D6 confirmed,
and real-time ON\_CHANGE is an explicit product goal). The core already exposes
the exact seams a push model needs — the `ValueWriter` change-set (D6), the
`changeSeq` order (D14), and zero-copy COW values (D17) — so this is *using*
prepared seams, not new invention. Building push now is also less churn than
shipping a poll-native loop and later reshaping it: retrofitting a wake path onto
a polling loop touches the whole driver, whereas the change source is an isolated
component here.

**Trade-off accepted.** Push requires a **cross-thread wake**: the writer thread
commits, but each subscription's Notifications are written on that subscription's
own stream. So `ILeafSink` must hand the change-set to the affected subscriptions
and wake their senders (a per-subscription queue + condition variable, or
equivalent). That hand-off — and the subscription index that answers "which
subscriptions cover this changed path" — is the substance of **P2 (threading)**,
deferred to the next decision. Structural events (`attach`/`detach`) flow through
the same seam because they are wire-visible adds/deletes that do not pass through
`ValueWriter` (core D6 note).

**Boundary kept clean.** The core never learns about subscriptions; it only emits
"these leaves changed / this subtree appeared/vanished." All subscription
matching, fan-out, and wire encoding stay in the protocol layer.

### P2 — Thread-per-stream, with a driver-agnostic `Subscription` object (decided)

**Each Subscribe RPC (one gRPC stream) is serviced by its own thread; the
per-subscription logic lives in a self-contained `Subscription` object that does
not know *who* drives it.** Today the driver is "one thread per stream"; the
object is shaped so a future shared-scheduler driver is a localized swap.

**What actually drives the thread count.** A single Subscribe stream carries one
`SubscriptionList` with *many* paths/Subscriptions (mixed ON\_CHANGE/SAMPLE), so
"monitoring many leaves across many groups/providers" lives **inside one stream**,
not across many. Thread count therefore tracks **concurrent client streams**, not
leaf/group/provider count. gRPC also requires that writes to one stream be
serialized (one `ServerReaderWriter` is not written from multiple threads at
once), so a *per-stream unit of work exists regardless* — T1 simply makes it a
dedicated thread.

**Realistic load.** The gNMI clients of a device like this are infrastructure
collectors (a telemetry pipeline, a monitor, maybe a controller) — typically
single to low-double-digit concurrent streams. T1 is comfortable to dozens of
streams; a shared pool (T2) only earns its complexity in the hundreds, and async
gRPC (T3) in the thousands. T1 is thus matched to the real load, not a mock
compromise.

```cpp
// Driver-agnostic: T1 calls these from the stream's own thread; a future T2 pool
// would call the same methods from a worker. The object's logic is identical.
class Subscription {
public:
    void start();                       // resolve monitored set + initial sync + sync_response
    void onPushEvent(const ChangeBatch&);   // P1 wake: changed leaves under our monitored set
    void onDeadline(SteadyClock::time_point);// SAMPLE tick / heartbeat fired
    SteadyClock::time_point nextDeadline() const;  // earliest of sample/heartbeat deadlines
private:
    MonitoredSet  set_;                 // leaves + groups (P4 keeps this current under hot-plug)
    StreamWriter  out_;                 // the per-stream serialized writer
    // … per-leaf last-sent changeSeq watermark (P3), mode state …
};
```

In **T1** the thread loop is simply:
`cv.wait_until(nextDeadline())` → woken by a push enqueue *or* a deadline → call
`onPushEvent` / `onDeadline` → write. No shared scheduler, no timer wheel: the OS
scheduler multiplexes, and the "timer" is just a wait-with-timeout.

**Why timers exist at all (independent of T1/T2).** Two behaviours are
time-driven and push cannot cover them: **SAMPLE** must emit once per
`sample_interval` (§3.5.1.5.2), and **`heartbeat_interval`** must re-send an
ON\_CHANGE value periodically even when unchanged. So every subscription has a
"next deadline"; push handles "value changed", the deadline handles "time
elapsed."

**Fan-out seam (P1 ↔ P2).** `ILeafSink::onChange` runs on the writer's
thread after commit. It consults a **subscription index** (monitored-prefix →
subscriptions, guarded by one mutex), enqueues the `shared_ptr<const ChangeBatch>`
into each covering `Subscription` (a refcount bump, not a copy), and signals its
condition variable. The batch carries the changed leaves **with their COW value and
path handles** (D17 / D16 L=B, zero-copy and refcount-safe) so the woken sender need
not re-lock the core to fetch values, and nothing dangles after the writer scope
exits — sidestepping a re-lock, a TOCTOU, and the span-lifetime trap. (Batch shape
and payload finalized in P3 — `LeafChange`/`ChangeBatch`, R1.)

**Trade-off & the kept option.** T1's thread count grows linearly with concurrent
streams; that only bites in the hundreds, which this device is not expected to
reach. Should it ever, the migration is localized **because `Subscription` is
driver-agnostic**: introduce a worker pool + a deadline **priority-queue**
scheduler (a min-heap by `nextDeadline()` — *not* a hashed timer wheel, which is
itself over-engineering below thousands of timers) that calls the same
`onPushEvent`/`onDeadline`/`nextDeadline` methods. The `Subscription` objects, the
notification builder, and the core are untouched. **Switch threshold: concurrent
streams reaching the low hundreds.** This is the pragmatic-best call — T1's
readability at the real load, with T2 kept a localized swap rather than a rewrite.

### P3 — Notification Builder (decided)

The builder turns `{leaf snapshots / change batch, group views, reason}` into
wire `Notification`(s). Four design forks were decided; the rest is spec-forced
construction. Everything here is checked against the gNMI spec — section numbers
are cited because the back-and-forth that settled these surfaced several details
that are easy to get subtly wrong, and the exact spec wording is load-bearing for
the implementation.

#### Spec-forced construction (no design choice — recorded so impl matches the spec)

- **Atomic re-send is scoped to the subscription (§3.5.2.5).** When an atomic
  group member changes, the Notification uses `prefix = group.prefix`,
  `atomic = true`, and the payload contains **only the subscribed members** — not
  the whole container. Spec example: container `a/b` atomic with leaves `c/d`,
  `c/e`, `f/g`; a client subscribed strictly to `a/b/c/d` gets `prefix:a/b`,
  `update:c/d`, `atomic:true` — `c/e`/`f/g` omitted, and the client must **not**
  treat them as deleted ("the atomic cache for `a/b` consists only of the
  subscribed paths"). The single `timestamp` is `max(collectedNs)` over the
  included members (D14 timestamp-collapse, "state as of the most recent member
  change"). **Two precise modalities (§3.5.2.5 L1907 / L1925):** sending the
  scoped atomic update is a `SHOULD` and the *payload-only-subscribed-members* is
  a `MUST`; a target that does **not** support partial-atomic reads `MAY` instead
  reject the subset subscription with `INVALID_ARGUMENT`. We take the `SHOULD`
  path (scoped re-send); the `MAY`-reject alternative is recorded but not chosen.
  (design-review **O**.)
- **Deletes go in the `delete` field and need not be per-leaf (§3.5.2.3).** A
  removed node MUST be appended to the Notification's `delete` field. Deletes
  "are not required to be per-leaf and can be at an intermediate branch" — so a
  `detachSubtree` becomes **one** `delete` path = (detached prefix ∩ monitored
  scope), not one delete per leaf (D14 delete-granularity).
- **Deleting an atomic member deletes the whole container; align the delete
  prefix (§2.1.1 L281-285).** A `delete` of a leaf that was previously sent
  atomically invalidates the *entire* atomic container, so when the builder
  removes anything inside an atomic group it **SHOULD** issue the `delete` with
  the **group's container prefix** (the same prefix the atomic updates carry),
  not a deeper per-leaf path — otherwise the client's atomic cache for that prefix
  behaves unexpectedly. (A delete Notification MAY itself be marked `atomic`; the
  flag does not change delete handling.) (design-review **S**.)
- **Replace = `delete` + `update` together (§3.5.2.3).** To replace an entire
  node's contents, populate `delete` with the node path *and* `update` with the
  new contents in the same Notification.
- **Default-value transition is an `update`, never a delete (§3.5.2.3).** When a
  leaf reverts to its YANG default, the target MUST send an `update` with the
  default value and MUST NOT send only a `delete` for the path or a parent.
- **`sync_response` framing (§3.5.2.3).** After the initial updates for all paths,
  send `SubscribeResponse{sync_response=true}`; STREAM needs it once, POLL once
  per poll cycle. With `updates_only`, `sync_response` is the *first* message and
  the initial dump is skipped. (Already in the per-mode lifecycle table.)
- **Bundling is *not* `allow_aggregation` (§3.5.2.1 vs §3.5.1.2).** Two distinct
  spec concepts, easy to conflate:
  - **Bundling** (§3.5.2.1) = several `Update` messages in **one** `Notification`
    sharing one timestamp. The spec permits it freely as a target choice, and its
    stated use cases — leaves atomically applied via `Set`, static-after-boot
    properties, component inventory (part/model/serial) — are **exactly** the core
    D15 atomic / non-atomic-group classification. This is what the builder does for
    a group; it is **not** gated by `allow_aggregation`.
  - **Aggregation** (`allow_aggregation`, §3.5.1.2) = combining multiple schema
    elements into a **single `Update`'s `val`** blob, requires the client flag
    (default MUST NOT use) *and* schema elements marked eligible.
  The builder emits group members as **separate `Update`s in one `Notification`**
  (bundling), so it MUST NOT be gated on `allow_aggregation`. (§3.5.2.1 also: for
  counter/event data with precise hardware timestamps the target MUST NOT obscure
  them by bundling — core D15's "ungrouped" category.)

#### Fork 1 — ILeafSink event payload is enriched (`LeafChange`), R1 (decided)

The change event carries an owned `vector<LeafChange>` (dispatched as
`shared_ptr<const ChangeBatch>`), each record
`{LeafId, shared_ptr<const CanonicalPath>, changeSeq, collectedNs, shared_ptr<const TypedValue>}`
**captured at commit** (sketch under P1), not a bare `span<const LeafId>`.

- **Why.** The builder never re-locks the core to fetch values — the COW handles
  (value D17, path D16/L=B) are zero-copy and refcount-safe, and the
  `changeSeq`/`collectedNs` come for free from the commit that produced them. This
  removes a second lock acquisition on every push **and** the TOCTOU window a re-read
  would open, and keeps the payload valid across the post-unlock dispatch.
- **Trade-off accepted.** A small struct + the `ValueWriter` assembling it during
  commit, versus a minimal `LeafId`-only seam. Chosen because it lowers coupling
  (the builder is a pure consumer; it never reaches back into the core) and is the
  whole point of making values shareable in D17.

#### Fork 2 — One unified `onChange(ChangeBatch)` entry point, R2 (decided)

The sink has a single method `onChange(const ChangeBatch&)` where
`ChangeBatch = {changed, added, removedPrefixes}` (sketch under P1), instead of
separate `onLeavesChanged`/`onSubtreeAttached`/`onSubtreeDetached` methods.

- **Why.** It is the **same `ChangeBatch` type P2's `Subscription::onPushEvent`
  consumes** — the core emits exactly what the protocol routes, one type across
  the seam, one routing path. A value-only commit simply leaves `added`/
  `removedPrefixes` empty.
- **R2 coverage falls out structurally.** A single `unregisterLeaf`/
  `unregisterGroup` is just a one-entry `removedPrefixes` — the same channel as
  `detachSubtree` — so the former gap (D6 named only subtree ops) is closed *by
  construction*, not by bolting on extra methods. The dispatch sources are
  therefore: `ValueWriter` commit → `changed`; `attachSubtree`/`registerLeaf` →
  `added`; `detachSubtree`/`unregisterLeaf`/`unregisterGroup` → `removedPrefixes`.
- **Trade-off accepted.** A "fatter" event with sometimes-empty spans, versus more
  self-documenting but multiplied method surface that *still* needs new methods for
  the single-unregister case. Chosen for lower coupling and less redundant surface.
- **Core-doc touch.** This revises core **D6** (push seam) to name all four
  dispatch sources and the enriched payload — done there too.

#### Fork 3 — Atomic re-send re-reads the group from the core, B1 (decided)

When one atomic member changes, the unchanged siblings' current values (needed for
the scoped re-send, §3.5.2.5) come from a `collectLeaves(group.prefix)` shared-lock
read, **not** from a per-subscription value cache.

- **Why.** Atomic re-send is **not a hot path** — atomic groups are config records
  (an NTP record, D15), small, and change on config writes, not on telemetry rates
  (the spec itself warns against putting fast-changing leaves in atomic containers,
  §3.5.2.5 best-practice). One cheap shared-lock read on a rare event keeps the
  `Subscription` **stateless about values** (easier to reason about, less redundant
  state to keep coherent).
- **Trade-off accepted.** One extra shared-lock read per atomic event, versus a
  per-leaf last-sent COW cache that is zero-re-lock but must be kept coherent on
  every send. The caching optimization is exactly what the future change-log (D17)
  subsumes if it is ever built — no reason to pre-pay it here.

#### Fork 4 — Single high-water `changeSeq` watermark per subscription, D1 (decided)

Each `Subscription` keeps **one** `lastSentSeq`. To decide what to send it asks,
per leaf, `leaf.changeSeq > lastSentSeq`; after sending it advances `lastSentSeq`
to the **high-water captured under the same snapshot lock that produced the sent
leaves** — i.e. `max(changeSeq)` over the leaves collected in that pass — **not**
a global high-water read separately *after* the send. Reading the global value
after an unlocked send would skip any change committed in the gap between the
snapshot and the advance (it would be `≤` the newly-read global yet `>` what was
actually sent), a silent miss. It is **not** a per-leaf watermark map.
(design-review **P**.)

- **Why one number suffices.** `changeSeq` is **registry-global monotonic** (D14),
  so a leaf that changes after time *t* always gets a seq greater than the global
  high-water at *t*. Therefore "did this leaf change since I last sent?" is exactly
  `leaf.changeSeq > lastSentSeq` — a single remembered number answers it for *every*
  leaf. A per-leaf map gives the identical answer at O(monitored) state for no gain.
- **One mechanism, three jobs.**
  1. **SAMPLE `suppress_redundant`** — see the spec clarifications below.
  2. **ON_CHANGE dedupe** — push events with `seq ≤ lastSentSeq` are dropped.
  3. **Subscribe-time race** — between the initial snapshot (taken at
     `lastSentSeq = global high-water at snapshot time`) and the first push, a
     change that is already in the snapshot (`seq ≤ lastSentSeq`) is skipped when
     its push arrives; one later (`seq > lastSentSeq`) is sent. The watermark
     collapses the race with no extra state.

##### How the watermark interacts with the two orthogonal layers (clarified, spec-aligned)

These two are **independent and stack** — confused at first, so recorded explicitly:

- **Layer 1 — watermark decides *which* leaves/groups need sending.** Applies to
  **all** leaves (ungrouped, non-atomic group, atomic group) uniformly: per-leaf
  `changeSeq > lastSentSeq`.
- **Layer 2 — atomic decides *who rides along*.** Only atomic groups. When Layer 1
  finds **any** member of an atomic group over the line, the send scope is
  *amplified* from that one leaf to **all subscribed members** of the group
  (§3.5.2.5). Independent/non-atomic leaves get no amplification.

So a single high-water serves both: independent leaves are tested leaf-by-leaf;
an atomic group is tested as "is **any** member over the watermark?" → if yes,
re-send all subscribed members.

##### SAMPLE semantics the watermark implements (spec §3.5.1.5.2, verified)

The watermark is **not** a server-side "skip unchanged" tendency — it exists only
to implement client-requested semantics:

- **SAMPLE default = re-send everything every interval.** "The value of the data
  item(s) **MUST be sent once per sample interval**" (L1762-1763). With no
  `suppress_redundant`, the builder does **not** consult the watermark — it sends
  the full monitored set each tick (a plain `collectLeaves`).
- **`suppress_redundant` is a client-set flag, and it is per-leaf.** Only when the
  client sets it `true` does the target "**SHOULD NOT** generate a telemetry update
  unless the value … has changed since the last update," and "updates **MUST only**
  be generated for those **individual leaf nodes** … that have changed" (L1767-1776:
  for `/a/b` with leaves `c`,`d`, if `c` changed but `d` did not, send `c`, MUST NOT
  send `d`). The single high-water implements exactly this per-leaf test
  (`c.changeSeq > lastSentSeq` true, `d.changeSeq > lastSentSeq` false). "Per-leaf"
  is about the *test subject*, not about needing a per-leaf *baseline*.
- **`heartbeat_interval` overrides suppression.** "The target MUST generate one
  telemetry update per heartbeat interval, regardless of whether
  `suppress_redundant` is set to `true`" (L1777-1781). Heartbeat forces a send
  independent of the watermark.

#### Scenarios (so a future reader can reconstruct the behaviour)

- **S-P3-a — independent leaf, SAMPLE + suppress_redundant.** Monitored `/a/c`,
  `/a/d` (ungrouped). `c` changes (`changeSeq` 5→11), `d` unchanged. Next tick with
  `lastSentSeq=8`: send `c` (11>8), skip `d` (its seq ≤ 8); advance `lastSentSeq`
  to global high-water. Matches §3.5.1.5.2 L1771-1776 exactly.
- **S-P3-b — atomic group, any member change.** Atomic `/system/ntp` = {`address`,
  `port`, `stratum`}, client subscribed to all three. `stratum` changes. Layer-1
  sees a member over the watermark; Layer-2 amplifies → one Notification
  `prefix:/system/ntp`, `atomic:true`, updates for all three subscribed members,
  `timestamp = max(collectedNs)`. (Contrast S-P3-a: if those three were *ungrouped*,
  only `stratum` would be sent.)
- **S-P3-c — partial atomic subscription (§3.5.2.5).** Same group, client
  subscribed only to `address`,`port`. `stratum` changes. Re-send is scoped:
  `prefix:/system/ntp`, `atomic:true`, updates for `address`,`port` **only**;
  `stratum` is omitted and the client must not treat it as deleted.
- **S-P3-d — branch detach.** Client subscribed to `/components`. A line card's
  `detachSubtree("/components/component[name=lc1]")` fires → one Notification with a
  single `delete` of that branch path (not per-leaf), §3.5.2.3.

### P4 — Monitored-set re-expansion under hot-plug (decided)

A STREAM subscription's monitored set is not fixed at setup: leaves appear
(`attachSubtree`) and vanish (`detachSubtree`) at runtime (core D12, ORv3 PSU/BBU
hot-plug), and §3.5.1.3 requires that a subscription to a **not-yet-existent** path
stay armed and emit once the path appears. P4 decides how each subscription keeps
its monitored set (and the P2 subscription index) current.

#### Framing: re-expansion splits by mode (the key simplification)

- **SAMPLE / POLL — implicit, free.** Each tick / poll already re-runs
  `collectLeaves(prefix)` over the subscription path, which is a fresh §2.4.2
  expansion of the *current* tree — so newly-attached leaves appear and detached
  ones vanish automatically, no structural wiring. Explicit deletion is **optional**
  for SAMPLE (§3.5.2.3), so a vanished leaf simply stops being reported.
- **ON_CHANGE — event-driven.** Push has no periodic re-scan, so the `ILeafSink`
  structural events are what react: `ChangeBatch.added` brings new leaves in (emit
  `update` — "a new node has been created", §3.5.2.3 MUST), `removedPrefixes` takes
  leaves out (emit branch-level `delete` — **required** for ON_CHANGE, §3.5.2.3).

**So P4 is, in substance, "how ON_CHANGE subscriptions react to structural
events."** SAMPLE/POLL get re-expansion for free from their periodic collection.

#### Spec-forced / falls-out (no design choice)

- **Each subscription retains its subscription *query paths*** (not only the
  materialized leaf set). Forced by §3.5.1.3: a not-yet-existent path has no
  materialized leaf to key on, so only the retained query can be re-matched when a
  future `attachSubtree` fires.
- **An "armed but currently-unmatched" subscription is just the degenerate case**
  of an empty monitored set with a retained query — no special mechanism; the
  re-match on a later attach brings leaves in (S-P4-b).
- **Adds → `update`, removes → one branch `delete`** (§3.5.2.3; branch-level decided
  in P3). For ON_CHANGE the delete is required; for SAMPLE optional.
- **Inherited correct for free** under the re-run strategy below: element-aligned
  prefix matching (D16), and atomic-group re-membership via `GroupView.memberPaths`
  (D13) — a new leaf that lands in an atomic group updates that group's re-send
  scope automatically because the re-expansion re-reads the group view.
- **Watermark (P3, single high-water)** advances to the global high-water after the
  newly-sent leaves go out, so subsequent pushes for them dedupe normally.

#### Fork 1 — ON_CHANGE re-expansion strategy: re-run `collectForSubscription` + diff, B2 (decided)

On a structural event affecting a subscription, **re-run
`collectForSubscription(query)` and diff the new monitored set against the current
one** (D14 key-set diff): keys gained → `update`, keys lost → branch `delete`.
Update the subscription's monitored set and its P2 index entries from the result.

- **Why.** It reuses the exact setup primitive (D13) and the exact poll-diff (D14)
  — **zero new algorithm**. Structural events are coarse and rare (D12, "stop-the-
  world is fine"), so re-expanding an affected subscription is a non-issue, and it
  is off the value hot path entirely. Lower coupling (depends only on
  `collectForSubscription` + diff), easier to extend (any future structural op is
  handled by re-expanding, no new case), less code than bespoke set surgery.
- **Rejected — incremental delta (B1).** Splicing `added`/`removedPrefixes` straight
  into the monitored set avoids re-expansion but needs new path-matching + set-
  surgery code with fiddly edge cases (a new leaf joining a group, group
  re-membership). It only wins on performance, which is not the bottleneck here.

#### Fork 2 — Affected-subscription lookup: linear scan over live subscriptions (decided)

When a structural event with changed prefix `E` arrives, **scan the live
subscriptions and test each subscription's query path `Q` against `E`**; re-run B2
only for the affected ones. "Affected" = `Q` and `E` have an ancestor/descendant
relationship (element-aligned, D16), i.e. one is a prefix of the other:

1. **`Q` ancestor of `E`** — a container subscription; `E` is a sub-branch
   inserted/removed under it.
2. **`E` ancestor of `Q`** — the §3.5.1.3 not-yet-existent case: a deep path whose
   ancestor branch was just attached.
3. **`Q == E`** — the subscribed node itself was attached/detached.

Anything with no ancestor/descendant relation is **skipped** (untouched subtree).

- **Why the scan, not the P2 index (correctness, not just cost).** Inside
  `ILeafSink::onChange`, the two kinds of span route differently: `changed` (value
  events) routes via the **P2 monitored-prefix index** — only an already-monitored
  leaf can change, so it is always in the index. Structural spans
  (`added`/`removedPrefixes`) **must not** use that index: an armed not-yet-existent
  subscription (S-P4-b) has an **empty monitored set and therefore no index entry**,
  so index-only routing would silently miss it and violate §3.5.1.3. The query-path
  scan is the only thing that reaches those armed subscriptions — so it is
  **required**, not merely a cheap alternative.
- **Why linear is enough.** Subscription count is single-to-low-double-digit
  concurrent streams (P2) and structural events are rare, so an O(subscriptions)
  scan per event is negligible. A dedicated structural index would be premature —
  the P2 index exists for the *value-change hot path*, which structural events are
  not.

#### Scenarios

- **S-P4-a — container gains a leaf.** S1 subscribes `/components` (ON_CHANGE).
  `attachSubtree("/components/component[name=lc1]")` brings `temperature`,`serial`,
  `part-no`. Scan: `Q=/components` is an ancestor of `E` → affected. B2 re-expands,
  diff yields three gained keys → one Notification with three `update`s. S3 on
  `/interfaces/...` has no relation to `E` → skipped.
- **S-P4-b — armed not-yet-existent path (§3.5.1.3).** S2 subscribes
  `/components/component[name=lc1]/state/temperature` before lc1 exists → monitored
  set empty, RPC stays open. Same attach event: `E` is an ancestor of `Q` →
  affected. B2 re-expands, `temperature` now materializes → `update`. S2 goes from
  empty to one leaf with no special-casing.
- **S-P4-c — branch detach.** lc1 pulled →
  `detachSubtree("/components/component[name=lc1]")`. S1 affected; B2 diff yields
  lost keys under that branch → **one** branch-level `delete` of the branch path
  (§3.5.2.3), not per-leaf. (For a SAMPLE S1 this delete is optional; the leaves
  just drop out of the next tick's collection.)

---

### P5 — TARGET\_DEFINED policy (decided)

For a `TARGET_DEFINED` subscription the target MUST decide, **per leaf**, whether
to stream it `ON_CHANGE` or `SAMPLE` (§3.5.1.5.2): event-driven leaves (a state
transition from an external trigger) → ON\_CHANGE; counter/continuously-varying
values → SAMPLE.

#### Framing: a per-leaf classification function, riding existing machinery

TARGET\_DEFINED adds **no new delivery mechanism**. It is one pure function
`resolveMode(leaf) → {ON_CHANGE | SAMPLE}` applied **when a leaf enters the
monitored set** (at subscription resolution, and again on each P4 re-expansion for
hot-plugged leaves). The resolved mode **tags** each monitored leaf, and the
existing P2 dual machinery serves the mixed partition unchanged: tagged-ON\_CHANGE
leaves react to push (`onPushEvent`), tagged-SAMPLE leaves react to the deadline
tick (`onDeadline`). A single `Subscription` therefore streams a mix with zero new
code — the only addition is the per-leaf mode tag.

> ⚠ **Current impl is per-*subscription*, not per-leaf (backlog C2).** Real
> per-leaf mixing needs the push seam (P1/P2), which is not built. Today
> `Backend::preferredMode(xpath)` returns one mode for the whole path (any
> non-Operational leaf ⇒ ON_CHANGE, else SAMPLE) and tags the whole subscription.
> The S-P5-c reject (a client-pinned `sample_interval` on TARGET_DEFINED) **is now
> enforced** with `InvalidArgument` (backlog C3 ✅).

#### Spec-forced (no design choice)

- **Per-leaf decision is mandatory** (§3.5.1.5.2 L1782-1784): event-driven →
  ON\_CHANGE, counter/sampled → SAMPLE.
- **Target chooses the interval** — the mock uses a single server-default
  `sample_interval` for all target-defined SAMPLE leaves (per-leaf interval tuning
  is out of scope / over-engineering for a mock).

#### Spec-permitted choice (not forced — recorded so the Resolver is deliberate)

- **A client-pinned `sample_interval` on TARGET\_DEFINED is rejected.** The spec
  here is **`SHOULD`**, not MUST: "If `sample_interval` is specified … the target
  `SHOULD` reject the subscription by closing the Subscribe RPC specifying an
  `InvalidArgument (3)` error code" (§3.5.1.5.2 L1790-1792). We *choose* to reject
  (the client should not be pinning an interval the target owns). This is a design
  choice the Resolver enforces, **not** a spec-forced behavior. (design-review **O**.)

#### Decision — `resolveMode` derives from the leaf's **schema model** (A, decided)

The classifier is a pure function of the leaf's **schema metadata**, not a separate
streaming knob and not the grouping. In this core that metadata is `LeafType`:

- **`Config` → ON\_CHANGE** — config changes are discrete `Set` events.
- **`State` → ON\_CHANGE** — operational *state* transitions are event-driven
  (link flap, hot-plug presence) — exactly the spec's "state of an entity based on
  an external trigger."
- **`Operational` → SAMPLE** — the enum's own definition is "sensors, counters" —
  continuously-varying / accumulating values.

**Why derive-from-schema is the production-correct call, not just the easy one.**
The decision is framed as "*`resolveMode` is a function of the leaf's schema
model*", with `LeafType` being this core's (coarse) instance of that model. A real
gNMI target derives ON\_CHANGE-vs-SAMPLE from richer schema signals — YANG type
(`yang:counter64` → SAMPLE), `config false`, `oc-ext:atomic`, or a published
"ON\_CHANGE-capable paths" set — which are **all schema metadata**. Keying
`resolveMode` on the schema model is forward-compatible with that: a richer schema
later slots into the *same* function, its structure unchanged (D8/D15: LeafType =
server schema, init-decided). So considering production *strengthens* this choice
rather than flipping it.

**Rejected alternatives.**
- **B — a separate hand-declared per-leaf streaming attribute** (`EventDriven` /
  `Sampled`). Gives the same answers as A for well-formed leaves, but in production
  it **duplicates information the schema already encodes** → two sources that can
  drift. The production answer is "derive from schema," which is A — not a parallel
  knob. Rejected for the redundancy/coupling it introduces.
- **C — infer from group atomicity** (atomic → ON\_CHANGE, else → SAMPLE).
  Structurally wrong: most leaves are ungrouped, so ungrouped `Config` and `State`
  both fall into "else" and are mis-streamed as SAMPLE — a config change would be
  polled instead of pushed. Rejected.

**Benign misclassification + the extension point.** A's mapping is a *proxy* for
event-vs-sampled, so a mislabeled leaf can be mis-streamed — but harmlessly: a
slow-changing leaf wrongly ON\_CHANGE just pushes when it changes; an event-ish
leaf wrongly SAMPLE arrives at the next tick. If exact control is ever needed, a
**per-leaf override** is a *purely additive* layer on `resolveMode` (and in
production that "override" is simply the richer schema signal taking precedence) —
so it is **not built now** (avoids speculative generality), only recorded as the
extension point.

#### Worked example (one PSC card)

| Leaf | `LeafType` | Real nature | `resolveMode` |
|---|---|---|---|
| `/system/config/hostname` | Config | `Set` event | ON\_CHANGE |
| `…/interface[eth0]/state/oper-status` | State | link flap (event) | ON\_CHANGE |
| `…/interface[eth0]/state/counters/in-octets` | Operational | counter | SAMPLE |
| `…/component[psu1]/state/temperature` | Operational | gauge | SAMPLE |
| `…/component[psu1]/state/oper-status` | State | hot-plug event | ON\_CHANGE |
| `/system/ntp/config/server-address` | Config (atomic) | `Set` event | ON\_CHANGE |

All six classify correctly under A. (Under C, `hostname` and the `oper-status`
leaves — ungrouped Config/State — would be wrongly forced to SAMPLE.)

#### Scenarios

- **S-P5-a — mixed subscription.** A `TARGET_DEFINED` subscription to
  `/components/component[psu1]/state` covers `temperature` (Operational→SAMPLE) and
  `oper-status` (State→ON\_CHANGE). The one `Subscription` pushes `oper-status`
  immediately on a hot-plug change (via `onPushEvent`) and emits `temperature` once
  per server-default interval (via `onDeadline`) — same object, two tags.
- **S-P5-b — hot-plug under TARGET\_DEFINED (P4 ∩ P5).** lc1 is attached; P4
  re-expansion brings its leaves into the monitored set, and `resolveMode` runs on
  each as it enters — the new counter leaves are tagged SAMPLE, the new
  presence/oper-status leaves ON\_CHANGE. No special wiring beyond applying the same
  function at the P4 entry point.
- **S-P5-c — client pins `sample_interval`.** A TARGET\_DEFINED request carrying a
  `sample_interval` is rejected by the Resolver with `InvalidArgument (3)`
  (§3.5.1.5.2).

---

## Status

All protocol-layer forks (P1–P5) are decided. No open forks remain; the next step
is bringing the implementation up to the core (D1–D17) and protocol (P1–P5) design.

### Subscription Resolver — setup validation (spec MUSTs, to formalize with P4)

Not a fork, but recorded so the Resolver enforces them: **SAMPLE** (§3.5.1.5.2
L1762-1766) — an unsupported `sample_interval` MUST be rejected by closing the RPC
with `InvalidArgument (3)`; `sample_interval == 0` MUST create the subscription at
the lowest interval the target can support. **Origin** (D16) — empty→`openconfig`;
any other (syntactically valid) origin names an unimplemented schema and is
rejected with **`UNIMPLEMENTED`** (§3.3.4 L1152 / §3.5.2.4 L1900, finding **N**),
`InvalidArgument` being reserved for a malformed path. These live in pipeline
step (1).

---

## RPC status codes — path outcomes (Get / Subscribe)

How a requested path maps to a status (spec §3.3.4 Get, §3.5.2.4 Subscribe). The
spec gives **three** outcomes from **two** independent questions: **routing** (does
any provider own this path's namespace?) and **data availability** (is a value
actually present?). In the integrated server, routing is `Backend::routed(xpath)`
(an owned-prefix match), and data availability is "the snapshot has ≥1 set leaf
after any type filter" (unset leaves never reach the wire).

| Outcome | Condition | Get | Subscribe (STREAM) |
|---|---|---|---|
| **Not implemented** | no owned prefix matches (`!routed`) | `UNIMPLEMENTED` | `UNIMPLEMENTED` |
| **Unknown origin** | valid path, origin ≠ `openconfig` (unimplemented schema) | `UNIMPLEMENTED` | `UNIMPLEMENTED` |
| **Exists, no data (yet)** | routed, but no set leaf | `NOT_FOUND` | silent — RPC **MUST NOT** be closed (§3.5.1.3) |
| **Exists, has value** | routed, ≥1 set leaf | return value(s) | return value(s) |
| **Syntactically invalid** | malformed `gnmi::Path` | `INVALID_ARGUMENT` | `INVALID_ARGUMENT` |

- *"Does not exist (yet)"* = path is owned but currently has no value (hardware
  offline, declared-but-unset config, or a list with no entries). Get →
  `NOT_FOUND`; Subscribe must keep the RPC open and emit if/when it appears.
- *"Not implemented"* = no provider owns the namespace at all.
- *"Unknown origin"* = a syntactically valid path whose `origin` is set to
  anything other than (empty/)`openconfig`. It names a schema the server does not
  implement, so it maps to the **"not implemented"** row → `UNIMPLEMENTED`. This
  is distinct from a *malformed* path (`INVALID_ARGUMENT`). (design-review **N**.)
- For ONCE/POLL, "exists, no data" is simply silent (no value returned), not an
  error.

**Encoding (§3.3.1 L1079-1081 / §3.2 L1011-1012).** Orthogonal to the path
outcome: if the client requests an `encoding` the target does not support, the
target **MUST** return `UNIMPLEMENTED` with a message naming the unsupported
encoding; when the client specifies no encoding, JSON is the default. (Today the
server emits scalar `TypedValue`s regardless of requested encoding — a known
implementation gap, backlog #3 — so this MUST is not yet enforced; recorded here
so the Resolver/Get path grows it.)

This replaces the pre-integration `DataProviderRegistry::fill() → FillResult{routed,
produced}` design: same two signals, now `Backend::routed()` + the snapshot's
set-leaf count. Covered by `tests/e2e/test_get_datatype.py` (Get type filter →
`NOT_FOUND`) and the gnmic/poll/onchange e2e.

> **Atomic + partial subscription also affects Get (§3.5.2.5 L1923):** when a Get
> hits an atomic container but requests only some of its leaves, the response
> SHOULD use the container prefix yet contain ONLY the requested leaves; a target
> that does not support partial atomic reads MAY instead return `INVALID_ARGUMENT`
> (L1925).

## Set — transaction semantics

A `SetRequest` is one **transaction** (spec §3.4): validate every operation first
and apply all-or-none, so the store is never left half-mutated. Once validation
passes the apply cannot fail, so no rollback machinery is needed.

- **Operation order is significant and fixed — `delete` → `replace` → `update`**
  within one `SetRequest` (spec §3.4 L1166-1168).
- **An empty `SetRequest` is a valid no-op, not an error (§3.4 L1178-1180).** A
  `SetRequest` specifying an empty set of paths (e.g. one carrying only
  extensions) **MUST NOT** be treated as an error; it returns an `OK`
  `SetResponse` with no `UpdateResult`s.
- **Status mapping** (mirrors the path-outcome table): a path no provider owns →
  `NOT_FOUND`; an owned but **read-only** path (not a declared config leaf) →
  `INVALID_ARGUMENT`; a malformed path → `INVALID_ARGUMENT`; success → `OK` with a
  per-op `UpdateResult` (`DELETE` / `REPLACE` / `UPDATE`).
- **Coherence:** all value writes of one `SetRequest` land in a single
  `Backend::commit` → one `core::ValueWriter` scope, so a concurrent Subscribe poll
  never sees an atomic record half-written (the write-side counterpart of
  `Notification.atomic`). `replace` on a flat scalar leaf is treated as `update`
  (no subtree to prune); `delete` removes the leaf (data-plane absence) while the
  declared-config schema persists, so the path stays writable for a later re-Set.

Implemented in [src/gnmi/set.cpp](../src/gnmi/set.cpp) + `Backend::commit`
([src/backend/backend.cpp](../src/backend/backend.cpp)).

## Notification prefix is wire compression

`Notification.prefix` is a **path-compression** mechanism: each update's full path
is `prefix ++ update.path`, and the spec does not fix the split — `/a/b/c/d/e` is
equally valid as `prefix=/a/b, path=c/d/e` or `prefix=/a/b/c/d, path=e`. The prefix
carries **semantic** weight only when `atomic=true`, where it names the boundary of
the complete-state snapshot (§2.1.1). This server sets `prefix` for atomic groups
(the group's container); non-atomic notifications carry **full paths** and no
copied request-prefix — the only thing stamped onto their prefix is `target`
(below). Real prefix compression (updates *relative* to a copied prefix) is not
done: it buys nothing for this mock, and the earlier half-done copy double-prefixed
(updates already carried full paths). (Reconciled in C5.)

**`prefix.target` MUST be echoed (§2.2.2.1 L414-424).** The `target` field is a
property of the request `prefix`, not of the data: if a client sets `prefix.target`
on a `SubscribeRequest` (or `GetRequest`/`SetRequest`), the server **MUST** reflect
that same `target` in the `prefix` of every corresponding response `Notification`;
if the client did **not** set it, the server **MUST NOT** set it. The Notification
Builder therefore carries the request's `target` through onto each emitted
`Notification.prefix` (independent of, and in addition to, the path-compression /
atomic-boundary use of `prefix` above). (design-review **R**.)
> ✅ **Implemented (backlog C5).** A shared `echoTarget()` stamps `target` (only)
> onto **every** response Notification.prefix — atomic included — at each build
> site (Get, `buildSubscribeNotifications`, the ON_CHANGE loop). Set echoes it on
> `SetResponse.prefix`. The old non-atomic full-prefix copy was replaced (it
> re-attached origin against C1 and double-prefixed).
