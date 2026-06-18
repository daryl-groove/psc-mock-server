# Device Modelling Conventions — slots, hot-plug, and dynamic leaves

> **Purpose.** This document records the *modelling conventions* for how a device's
> gNMI/OpenConfig surface maps onto the core data layer — settled in a design
> discussion (2026-06-16) about empty containers, hot-pluggable slots, and dynamic
> leaves. It is **not** a new core mechanism: the core (`core-data-model-design.md`,
> D1–D18) is unchanged. These conventions belong to the **provider / schema layer**
> that sits *above* the core and decides *what to register and when*.
>
> It is also meant as a **review lens**: a future pass should re-check the core and
> protocol design against the checklist at the end.

---

## 0. The one core fact everything rests on (D18)

The core stores exactly two kinds of entity: **leaves** (`LeafEntry`, the
value-bearing nodes) and **notification groups** (`NotificationGroup`, notification
scopes). **There is no object for an interior / container node.** A container exists
*implicitly*, exactly when ≥1 leaf lives under its prefix; it is an *addressing
scope*, not a stored thing.

Everything below is a consequence of this.

---

## 1. Two planes: data vs schema

The meaning of "a path exists" splits across two planes, and conflating them is the
root of most confusion:

| Plane | Carried by | "Does `/a/b/c` exist?" |
|---|---|---|
| **Data / instance** | Get/Subscribe `Update`s = leaf path→value | Yes iff there is a leaf at/under it. An empty subtree yields **no** updates. |
| **Schema / model** | Capabilities RPC (§3.2) + the published YANG/OpenConfig model | The model says which paths *can* exist and which lists *can* grow. |

**Consequence — an empty (non-presence) container is indistinguishable from a
non-existent one at the data plane.** Both yield zero updates. So:

- "This path **may** grow leaves in future" is **schema** knowledge (advertised via
  Capabilities + the model), **not** something a server conveys by materialising an
  empty container.
- A client *watches for* future growth by subscribing to the not-yet-existent path;
  the server keeps it **armed** and emits when leaves appear (§3.5.1.3 — protocol
  layer **P4**, deferred). The core does **not** store an empty node to support this.

---

## 2. Presence is a *value*, not an empty container

YANG distinguishes **non-presence** containers (exist iff they have children — the
OpenConfig telemetry norm) from **presence** containers (existence is itself state).
OpenConfig generally avoids presence containers and instead encodes presence as an
explicit **leaf**.

The canonical example is a chassis bay:

- **OpenConfig `/components/component[name=…]/state/empty`** (boolean). When
  `empty = true` it means **"slot present but unpopulated"**, and — per the OC
  definition — *"no other corresponding subtree of data will exist for the
  component."*

So **a slot that "exists but is empty" is not an empty container; it is a node that
carries presence/state leaves with real values** (`name`, `empty=true`, …). The
container path itself stays an implicit prefix; what makes it perceptible is the
leaves under it.

---

## 3. The modelling rule: permanent presence markers + dynamic device subtree

For a hot-pluggable device, split its paths into two sets:

| Set | Examples | Lifetime |
|---|---|---|
| **Presence markers** (only if the *slot* is physically permanent, e.g. a fixed bay) | the list **key** (`…/state/name`) and **`…/state/empty`** | **permanent** — registered at init, never removed; `empty` flips |
| **Device subtree** (everything that only exists when a device is physically there) | `…/sensors/vout`, `…/state/oper-status`, `…/state/serial-no`, `…/state/temperature` | **dynamic** — `registerLeaf`/`attachSubtree` on insert, `unregister`/`detachSubtree` on remove |

Lifecycle:

- **init (slot present, empty):** register only the presence markers
  (`name`, `empty=true`). The slot is now perceptible; a client subscribed to it sees
  it is empty — distinct from "no such slot" (which yields nothing).
- **insert:** flip `empty → false` (and `oper-status → ACTIVE`) via `ValueWriter`,
  **and grow** the device subtree leaves.
- **remove:** flip `empty → true`, **and drop only the device subtree leaves**. The
  presence markers stay, so the slot remains visible.

Per §2, the device subtree (sensors, `oper-status`, `serial`, …) is **genuinely
dynamic** — when the slot is empty those leaves **do not exist** (OC's "no other
subtree when empty"). Making a sensor leaf *always present* would be wrong: a client
would see a value for a device that is not inserted.

> Note: strictly, only `{name, empty}` are permanent; `oper-status`, `serial`,
> `part-no` etc. belong to the dynamic device subtree (they do not exist when the slot
> is empty). This fixed-slot lifecycle is the worked example in `PscPowerSensorProvider`
> (§9), exercised by `PscHotPlugTest` (`tests/test_backend.cpp`): insert/remove via its
> `setPresent()` backdoor — the markers persist, the sensor subtree appears/vanishes.

### Two valid granularities — choose by what is physically permanent

- **Fixed slot, swappable device** (a chassis PSU/BBU bay): the *slot* is permanent
  → presence markers persist, device subtree is dynamic (this section).
- **Whole component is pluggable** (e.g. a transceiver that only has a component entry
  when present): the *entire* `component[name=…]` subtree appears/vanishes
  (`attach/detachSubtree` of the whole branch). This is the `HotPlugAttachThenDetachBranch`
  worked example (Scenario 4) in `tests/test_integration_scenario.cpp`.

Both are supported; the difference is only *which prefix is permanent*.

---

## 4. Flag vs true-dynamic — use each at the right level

Two ways to express "not currently there", applied at **different granularities**:

- **Presence flag (`empty`) — for the slot level.** A permanent marker leaf whose
  boolean value says populated-or-not. Use this for *slot presence*. (We do.)
- **True dynamic add/remove — for the device-data level.** `registerLeaf` /
  `attachSubtree` on insert, `unregister` / `detachSubtree` on remove. Use this for
  *sensor / device leaves*.

**Do not** push a per-leaf "exposed/enabled" flag down to every sensor (pre-register
everything, hide with a flag). It is heavier and fights the core model:

- it breaks the invariant **stored = present**, forcing an "is it exposed?" filter in
  *every* read path (`collectLeaves`, `collectForSubscription`, `getLeaf`);
- it requires knowing **all** possible leaves up front — impossible for unbounded list
  keys;
- the core already treats *leaf added/removed* as the wire event (D14 key-set diff;
  structural dispatch), so true-dynamic is what the architecture is built for.

Net: **flag for "slot present/empty", dynamic for "device data exists/not".**

---

## 5. List keys: schema template vs concrete instances

- `/components/component[name]/…` with an **unbound key** is a **schema** path. The
  core never stores a "template" — it stores **concrete keyed entries**
  (`[name=PSU1]/…`, `[name=PSU2]/…`), each materialised dynamically when that device
  appears. "`name` is flexible" means *the set of entries is dynamic*, not that a key
  mutates.
- **A list key is an immutable identity.** You do not change `PSU1` into `PSU2`; they
  are different entries (changing identity = delete-old + create-new).
- **Watching across all keys** (`[name=*]`) is **wildcard matching** (gNMI wildcards;
  finding **Q**) — **deferred**: the core takes element-aligned *exact* prefixes;
  wildcard expansion is a protocol-layer concern.
- **Per-instance dynamic** (PSU1 and PSU2 each appearing with their own `vout`) is
  **supported today** — one `attachSubtree` per device.

### Forward design note — a "component template" with named key variables (provider/schema layer, NOT core)

A natural developer wish (raised 2026-06-16) is to declare *once* that a branch
**tolerates any key** at a position — algebra-style "free variables" — and then
instantiate concrete entries by binding them, instead of repeating
`[name=PSU3]` literals. This is sound, but it belongs **above** the core and needs the
right shape:

- **It decomposes into two layers, neither of which overthrows the core:** a *template*
  = a **schema** declaration ("these positions are list keys"), and *instantiation* = a
  **factory** that binds the variables and emits a concrete `SubtreeSpec`, which is then
  registered through the existing core API. The core keeps storing **concrete leaves**
  only — this is purely additive at a higher layer.
- **Bind by KEY NAME, not by `printf` position.** gNMI keys are a named,
  order-independent `map<string,string>` (D16 sorts them), so a positional
  `(/a/b/*/c/d/*, p1, p2)` form is wrong: it cannot express multi-key entries
  (`[class=*][name=*]` — which `*` is which?) and drops the key name that is part of the
  canonical path. The correct shape binds *named* placeholders:
  `template "/components/component[name=?]/state/vout" + {name: "PSU3"}`.
- **Mock-pragmatic vs production.** For the mock a plain **provider factory function**
  (`makePsuSpec(name)`) already gives the DRY + parameterisation. The richer
  *first-class, inspectable, validatable* template — that can also be **advertised via
  Capabilities** — is exactly what a real gNMI target already has: **its YANG/OpenConfig
  schema**. So "a declared template with key variables" = "the server has a schema", and
  it lives in the **schema / Capabilities layer** (deferred), not the core.

Decision: record it here as the shape to build when the provider/schema layer is done;
do **not** add template/wildcard *registration* to the core.

---

## 6. What the core supports today vs what is deferred

| Capability | Status |
|---|---|
| Implicit containers; grow/shrink leaves under any (possibly stable) prefix | **now** (`registerLeaf`, `attach/detachSubtree`) |
| Presence-marker leaf with a flipping `empty` flag | **now** (a leaf + `ValueWriter`) |
| Concrete dynamic list entries (`[name=PSU1]`, `[name=PSU2]`) | **now** |
| Subscribe to a not-yet-existent path → armed → emit on appearance (§3.5.1.3) | **deferred** — protocol **P4** |
| Wildcard key match `[name=*]` across instances (§3.3/§3.4) | **deferred** — finding **Q** |
| "This path may exist" advertised to clients (Capabilities + model) | **deferred** — schema/Capabilities layer above the core |

---

## 7. Review checklist (use this to revisit the design)

When reviewing the core/protocol against these conventions, ask:

1. **Empty ≠ stored.** Does anything in the core try to represent an empty container
   as a stored object? (It should not — D18.)
2. **Presence as a value.** Is "slot present but empty" modelled as a *leaf*
   (`empty=true`), never as a childless container?
3. **Permanent vs dynamic split.** For each modelled device, is the permanent set
   minimal (`{name, empty}` for a fixed slot) and is the whole device subtree
   (incl. `oper-status`, sensors) dynamic?
4. **No per-sensor exposed-flag.** Are device leaves added/removed for real, rather
   than pre-registered and hidden behind a flag?
5. **Armed subscription (P4).** When the protocol layer is built, does a subscription
   to a not-yet-existent slot/sensor stay open and emit on `attachSubtree`?
   (§3.5.1.3; affected-subscription scan, not the value-change index.)
6. **Wildcard (Q).** Is `[name=*]` cleanly addable at the protocol layer without core
   changes? (The core's element-aligned exact-prefix model should not block it.)
7. **Capabilities.** Is "what *can* exist" advertised from the schema/model, not
   inferred from instance data?
8. **Key immutability.** Does any code assume a list key can mutate in place? (It
   cannot — different keys are different entries.)

---

## 8. Layering & ergonomics — where developer-friendliness lives

Settled in a 2026-06-16 architecture discussion. Recorded so these are not
re-litigated; only the load-bearing conclusions are kept here (the option-by-option
deliberation lives in the conversation, not this doc).

### 8.1 The stance

The core optimises **correctness, invariants (D1/D6/D11), spec-faithful seams, and
performance (L=B / D17 COW)** — **not** end-developer ergonomics. Making registrations
read nicely is the **provider / authoring layer's** job. So the core feeling
"low-level" is by design and acceptable, **on the condition** that it exposes the
seams the ergonomic layer needs — verified true (`attach/detachSubtree`,
`collectForSubscription`, `writeValues`, structural events all suffice). *Scope: this
holds for authoring sugar / observability; a genuine **semantic** gap could not be
wrapped away, which is why the two non-sugar items (8.5 retroactive, 8.2 nested-atomic)
are tracked separately, not as "the wrapper will fix it".*

### 8.2 Do NOT turn the core into a tree

A tree-of-nodes core was considered and rejected. A tree is closer to YANG's native
model and *can* meet the spec, but "meets spec" is not the deciding axis (flat does
too). The tree re-implements all path logic on nodes (canonicalisation does not go
away), reintroduces the dangling **raw-pointer** handle that D1 removed, multiplies the
concurrency invariants, and is a one-way-door rewrite of a done core — for a payoff
("more intuitive") that is achievable **above** the core: a tree-shaped builder emits
into the flat store (authoring = tree, storage = flat). The **bmcweb flavour** lives in
the provider/handler layer, not in low-level storage. (D16 already weighed structured
A2 vs flat A3 and chose flat for the same reasons.)

**Nested atomic groups are not spec-supported** regardless of representation: an
`atomic=true` notification declares the *complete* state of its prefix's subtree, with
no carve-out/override concept (§2.1.1 / §3.5.2.5). A tree lets you *write* nesting, but
the spec does not cleanly support it. (Nested *non-atomic* "inner-wins" is just the
longest-ancestor rule D3 already uses — a possible D5 relaxation, **not** a tree
requirement.)

### 8.3 Binding model: PUSH-bridge, not pull-callback

The core is a **push / value-store** (providers `ValueWriter::set`; the core holds the
current value). Binding a leaf to a data source (e.g. D-Bus) means **the provider
watches the source and pushes** values in — *not* the core calling a getter on read. A
pull / read-callback would run blocking I/O **under the shared read lock** (fights D2)
and has no stored COW version for a snapshot to hold (fights D17). Push is also the
correct model for gNMI streaming (SAMPLE / ON_CHANGE). Internalise this when writing
providers; the core needs no change for it.

### 8.4 Seam-shaping backlog (co-design with the provider layer)

Small, additive shape tweaks that make the authoring layer cleaner. Best shaped *with*
their consumer; **A** is consumer-agnostic and could be done early.

- **A. ✅ DONE — `attachSubtree` returns a path→`LeafId` map** (`std::map<std::string,
  LeafId>`), not a positional `vector<LeafId>`. The binding layer needs *which id ↔ which
  path*; a positional vector coupled callers to spec order.
- **B. A safe path-*construction* helper** in `canonical_path` (escape key values when
  assembling `[name=…]`) — the inverse of the parse-side escaping we already have, so
  providers don't hand-escape.
- **C. ✅ DONE — Group identification by prefix.** The core now identifies a group *solely*
  by its prefix: `registerGroup(prefix, …)` / `getGroup(prefix)` / `unregisterGroup(prefix)`,
  no `name` at all (went further than the original "keep name, just default it" — `name` was
  end-to-end dead weight). See `core-data-model-design.md` D4.
- **D. (optional, low)** an immediate-children (`ls`-style) navigation query — only if
  the authoring layer needs to *browse* existing state; builders mostly *write*.

### 8.5 Small core ergonomics adds, and the one re-decision

- **Additive, "add when it helps":** leaf→group reverse lookup (group name on the
  `getLeaf` snapshot); `ungroupedLeaves()` dev-time audit (catch path typos).
- **Re-decision (NOT additive):** *retroactive group assignment* — `registerGroup`
  adopting existing ungrouped leaves under its prefix. Removes the "groups before
  leaves" ordering footgun and closes the "leaf-under-prefix ⟺ member" invariant gap,
  **but** reopens D3 and gives `registerGroup` a side-effect on existing leaves. Decide
  deliberately, not as a drive-by.

### 8.6 Rejected / already done (recorded so they are not re-proposed)

- **Rejected:** `expectedGroup` on `registerLeaf` (a redundant assertion over
  path-derived membership); a tree-core rewrite (8.2); a per-sensor "exposed" flag
  (breaks `stored == present`).
- **Already shipped (this core):** `wouldConflict()` / `registeredPrefixes()` + a D5
  overlap error that states *why* (§2.1.1) and points at `wouldConflict`.
- **Provider/schema-layer, deferred (see §5 forward note + §6):** the named-key
  component template, wildcard `[name=*]` (Q), Capabilities/model advertisement.

---

## 9. Concrete model — the PSC card's exposed paths

The worked instance these conventions describe, as the two providers register it
(`src/backend/`). Based on **openconfig-platform-psu** (augmenting
openconfig-platform) and **openconfig-system**.

### Sensors — `PscPowerSensorProvider`, hot-pluggable PSU slots (M2)

Two PSU slots, `PSC-0` and `PSC-1`. Each slot carries two **permanent** markers
(registered once, never removed) plus a **dynamic** sensor subtree that exists only
while the PSU is present (§3):

```
/components/component[name=PSC-{0,1}]/state/name                         permanent marker (= unit name)
/components/component[name=PSC-{0,1}]/state/empty                        permanent marker (false = populated)

/components/component[name=PSC-{0,1}]/state/temperature/instant          celsius  ┐ dynamic device subtree:
/components/component[name=PSC-{0,1}]/power-supply/state/input-voltage    volts    │ attached on insert,
/components/component[name=PSC-{0,1}]/power-supply/state/input-current    amps     │ detached on remove
/components/component[name=PSC-{0,1}]/power-supply/state/output-voltage   volts    │ (54V bus / 12V rail)
/components/component[name=PSC-{0,1}]/power-supply/state/output-current   amps     │
/components/component[name=PSC-{0,1}]/power-supply/state/output-power     watts    │
/components/component[name=PSC-{0,1}]/power-supply/state/capacity         watts    ┘ (capacity static)
```

Slots **boot present** (empty=false + sensors), so a Get/Subscribe before any
hot-plug sees the same data a fixed inventory would. `setPresent(unit, bool)` is the
insert/remove backdoor (Fork B): insert attaches the sensor subtree + flips
`empty=false`; remove detaches the two device branches (`/power-supply`,
`/state/temperature`) — leaving the markers — + flips `empty=true`. A background
`jthread` drifts the present slots' values on a quantized random walk, one
`ValueWriter` scope per tick. Sensors and markers are all `Operational`.

> Units are **volts / amps / watts**, not mV/mA/mW. YANG `ieeefloat32` maps to
> `double_val` in `gnmi::TypedValue`.

### System config — `SystemConfigProvider`, all `Config`

Flat scalars change only via Set; the NTP server record is an **atomic group**
(one `registerGroup(atomic=true)`, members auto-assigned by prefix).

```
/system/config/hostname                                          seeded
/system/config/login-banner                                      seeded
/system/config/motd-banner                                       seeded
/system/config/timezone-name                                     declared, UNSET (Set creates it)
/system/ntp/servers/server[address=10.0.0.1]/config/address  ┐
                                              .../port        │  atomic record
                                              .../version     │  (one Notification,
                                              .../iburst      │   one timestamp)
                                              .../association-type ┘
```

`timezone-name` is the **declared-but-unset** case (§1 schema plane): writable
even with no value, because the schema outlives any value. The NTP record is the
atomic-container worked example (`tests/e2e/test_atomic.py`).
