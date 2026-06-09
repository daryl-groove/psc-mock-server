# psc-mock-server — Design Document

A gNMI server that acts as a PSC (Power Supply Controller) card in an ORv3 system,
exposing mock sensor data to external gNMI clients. Primary goal: validate the gNMI
telemetry pipeline end-to-end before connecting to real hardware.

---

## Deployment Model

**Final target: resident service on the PSC card** — starts automatically, runs in
the background, requires no operator input after boot.

| Aspect | Current (Phase 1) | Target (production) |
|--------|------------------|---------------------|
| Startup | CLI args (`--bind`, `--cert`, `--force-insecure`, …) | No CLI args; config file or build-time constants |
| TLS cert/key | Passed via `--cert` / `--private-key` | Fixed paths on the card (e.g. `/etc/psc/tls/`) |
| Authentication | `--username` / `--password` | Build-time or config file |
| Bind address | `--bind` (default `localhost:50051`) | Fixed port; configurable at build time |
| `main.cpp` | Argument parsing + `RunServer()` | Minimal `main()` → `RunServer()` directly |

**Decision:** CLI parameters are kept for Phase 1 development convenience.
`main.cpp` will be simplified (remove `getopt` loop) in a later cleanup pass once
the server is verified end-to-end. No functional changes needed before that point.

---

## YANG Model

Based on **openconfig-platform-psu** (`public/release/models/platform/openconfig-platform-psu.yang`)
augmenting **openconfig-platform**.

### Exposed Paths (2 PSC units: PSC-0, PSC-1)

```
/components/component[name=PSC-{0,1}]/state/temperature/instant          (ieeefloat32, celsius)
/components/component[name=PSC-{0,1}]/power-supply/state/input-voltage   (ieeefloat32, volts)
/components/component[name=PSC-{0,1}]/power-supply/state/input-current   (ieeefloat32, amps)
/components/component[name=PSC-{0,1}]/power-supply/state/output-voltage  (ieeefloat32, volts)
/components/component[name=PSC-{0,1}]/power-supply/state/output-current  (ieeefloat32, amps)
/components/component[name=PSC-{0,1}]/power-supply/state/output-power    (ieeefloat32, watts)
/components/component[name=PSC-{0,1}]/power-supply/state/capacity        (ieeefloat32, watts, static)
```

> Units per `yang/openconfig-platform-psu.yang` — **volts / amps / watts**, not mV/mA/mW.
> Type `ieeefloat32` maps to `double_val` in gNMI TypedValue (proto 0.10.0).

YANG files (already downloaded into `yang/`):
- `yang/openconfig-platform.yang`
- `yang/openconfig-platform-psu.yang`

---

## Build System & Language Standard

| Item | Decision | Rationale |
|------|----------|-----------|
| Build system | **Meson** (`meson.build`) | Platform standard; already written |
| C++ standard | **C++20** | `std::jthread` for background sensor thread; `std::format` for logging |
| Header extension | **`.hpp`** for new files | Modern C++ convention; copied sysrepo-gnxi files keep `.h` until Phase 1 done, then rename in one batch |

**Current state:**
- `meson.build` — grpc++/protobuf/boost deps + pre-generated proto sources; no protoc dependency
- `proto/generated/` — pre-generated `.pb.cc/.pb.h` committed to repo (stable spec, no runtime codegen)
- `proto/gen_proto.py` — **maintenance utility only** (not a build step); run manually when proto spec changes
- `src/backend/data_provider.hpp` — `IDataProvider` + `DataProviderRegistry` + `addLeaf` overloads
- `src/backend/psc_power_sensor_provider.hpp/.cpp` — uses `.hpp` convention
- `src/gnmi/*.h`, `src/security/*.h`, `src/utils/*.h` — keep `.h` until after Phase 1

---

## Architecture

```
gNMI Client
    │  Subscribe / Get / Set / Capabilities (gRPC)
    ▼
GNMIService (src/gnmi/gnmi.h)
    │  holds DataProviderRegistry registry_
    ├── Capabilities  → capabilities.cpp  (static model list)
    ├── Get           → get.cpp        ─┐
    ├── Set           → set.cpp  (mock) ├─→ registry_.Fill(list, xpath)
    └── Subscribe     → subscribe.cpp  ─┘         │
                                         DataProviderRegistry  (src/backend/data_provider.hpp)
                                             │  fan-out to all matching providers
                                             ├── PscPowerSensorProvider   ✅ implemented
                                             ├── PlatformInfoProvider     (future)
                                             ├── AlarmProvider            (future)
                                             └── ...
```

**Extensibility rule:** adding a new data domain requires exactly two changes:
1. New `class XxxProvider : public IDataProvider` in `src/backend/`
2. One `registry.Register(make_unique<XxxProvider>())` line in `main.cpp`
gNMI layer (`get.cpp`, `subscribe.cpp`, `gnmi.h`) is never modified.

**Key design decision:** `DataProviderRegistry` is the only boundary between
the gNMI protocol layer and the data layer. gNMI handlers are unaware of
specific provider types.

Reference for backend decoupling pattern: `impl/gnmi-grpc/src/gnmi_collector.cpp:100-157`

**Interface (`src/backend/data_provider.hpp`):**
```cpp
class IDataProvider {
public:
    virtual bool Handles(const std::string& xpath) const = 0;
    virtual void Fill(RepeatedPtrField<Update>* list,
                      const std::string& xpath) = 0;
};

class DataProviderRegistry {
    // Fan-out: calls Fill() on every provider whose Handles() returns true
    void Fill(RepeatedPtrField<Update>* list, const std::string& xpath) const;
    void Register(std::unique_ptr<IDataProvider>);
};

// Shared helpers — available to all providers via #include "data_provider.hpp"
void addLeaf(list, xpath, double value);      // double_val  (voltage, current, temp)
void addLeaf(list, xpath, std::string value); // string_val  (status, firmware, enum)
void addLeaf(list, xpath, bool value);        // bool_val    (enabled, alarm)
void addLeaf(list, xpath, int64_t value);     // int_val     (signed counter)
void addLeaf(list, xpath, uint64_t value);    // uint_val    (unsigned counter)

// TARGET_DEFINED mode resolution — override in event-driven providers
gnmi::SubscriptionMode PreferredMode(xpath);  // default: SAMPLE
```

---

## Path Error Handling

### Spec requirements (§3.3.4 Get, §3.5.2.4 Subscribe)

**Get RPC:**

| Scenario | Required behavior |
|---|---|
| Path syntactically correct, exists or has YANG default | Return value(s) |
| Path syntactically correct, does not exist (yet) | `NOT_FOUND` |
| Path syntactically correct, not implemented by server | `UNIMPLEMENTED` |
| Path syntactically incorrect | `INVALID_ARGUMENT` |

**Subscribe RPC:**

| Scenario | ONCE / POLL | STREAM |
|---|---|---|
| Path exists or has YANG default | Return value(s) | Return value(s) |
| Path syntactically correct, does not exist (yet) | Silent — no value returned | Silent — RPC **MUST NOT** be closed |
| Path not implemented by server | `UNIMPLEMENTED` | `UNIMPLEMENTED` |
| Path syntactically incorrect | `INVALID_ARGUMENT` | `INVALID_ARGUMENT` |

### Key distinction

The spec defines **three** outcomes, distinguished by two independent questions:
**routing** (does any registered provider's prefix own this path's namespace?) and
**data availability** (is a value actually present?).

| Outcome | Condition | Get | Subscribe (STREAM) |
|---|---|---|---|
| **Not implemented** | no registered prefix matches | `UNIMPLEMENTED` | `UNIMPLEMENTED` |
| **Exists, no data (yet)** | a prefix matches, but no value is available | `NOT_FOUND` | silent — RPC **MUST NOT** be closed |
| **Exists, has value** | a prefix matches and a value is available | return value(s) | return value(s) |

**"Does not exist (yet)"** — path is valid in the schema and owned by a provider,
but has no data currently (e.g. hardware temporarily offline, or store not yet
seeded). Subscribe must not close the RPC; Get returns `NOT_FOUND`.

**"Not implemented"** — no registered provider owns the path's namespace at all.
Both Get and Subscribe return `UNIMPLEMENTED`. A mock server does not monitor for
future hardware presence under a namespace it does not serve.

### Implementation

Distinguishing all three requires `DataProviderRegistry::fill()` to report **two**
signals, not a single bool:
- **routed** — at least one provider's registered prefix matched the path
- **produced** — at least one Update was appended

| routed | produced | Get | Subscribe |
|---|---|---|---|
| false | — | `UNIMPLEMENTED` | `UNIMPLEMENTED` |
| true | false | `NOT_FOUND` | WARN, RPC not closed (§3.5.1.3 MUST NOT close) |
| true | true | OK | OK |

- `get.cpp::buildGetUpdate()` → maps the two signals to the table above (§3.3.4)
- `subscribe.cpp::buildSubsUpdate()` → `routed==false` → `UNIMPLEMENTED`; `routed && !produced` → silent, RPC not closed
- `tests/test_data_provider.cpp` → covers all three outcomes

> **Status:** the original implementation collapsed both signals into a single
> `bool` (list growth) and returned `NOT_FOUND` for *unrouted* paths — which
> contradicts the "not implemented → `UNIMPLEMENTED`" rule above. The two-signal
> split lands with the Leaf Store work (B), which also introduces the
> data-availability dimension (routed-but-empty → `NOT_FOUND`).

---

## Reference Repos

| Repo | Location | What we take from it |
|------|----------|---------------------|
| sysrepo-gnxi | `impl/sysrepo-gnxi/` | Full gNMI RPC framework (primary base) |
| gnmi-grpc | `impl/gnmi-grpc/` | Backend decoupling pattern (`StatConnector`) |
| openconfig/gnmi | `spec/gnmi/proto/` | Canonical proto 0.10.0 |
| openconfig/reference | `spec/reference/rpc/gnmi/` | Spec documents |

---

## File Map

### From sysrepo-gnxi (sysrepo stripped, now complete)

| File | Status | Change made |
|------|--------|-------------|
| `src/main.cpp` | ✅ done | Registry created + `PscPowerSensorProvider` registered; `GNMIService(move(registry))` |
| `src/gnmi/gnmi.h` | ✅ done | Holds `DataProviderRegistry registry_`; constructor takes registry by move |
| `src/gnmi/gnmi.cpp` | ✅ done | Passes `registry_` to each RPC handler |
| `src/gnmi/capabilities.cpp` | ✅ done | Static `ModelData` list for openconfig-platform-psu |
| `src/gnmi/get.cpp` | ✅ done | `registry_.Fill(updateList, fullpath)` |
| `src/gnmi/set.cpp` | ✅ done | Mock no-op; returns OK for all operations |
| `src/gnmi/subscribe.cpp` | ✅ done | `registry_.Fill(updateList, fullpath)` |
| `src/utils/utils.h` | ✅ done | Added `xpath_to_gnmi_path()`; no sysrepo deps |
| `src/security/` | ✅ unchanged | No sysrepo dependency |
| `src/utils/log.h/.cpp` | ✅ unchanged | No sysrepo dependency |

### Replaced (sysrepo-gnxi encode/ → our backend/)

| Old (sysrepo-gnxi) | New (psc-mock-server) | Reason |
|--------------------|----------------------|--------|
| `src/gnmi/encode/encode.h` | `src/backend/data_provider.hpp` | `IDataProvider` + `DataProviderRegistry` |
| `src/gnmi/encode/encode.cpp` | `src/backend/psc_power_sensor_provider.cpp` | Mock data generator |
| `src/gnmi/encode/json_ietf.cpp` | _(built into provider)_ | No libyang needed for mock |
| `src/gnmi/encode/load_models.cpp` | _(static list in capabilities.cpp)_ | |
| `src/gnmi/encode/runtime.cpp` | _(removed)_ | |

### New files

| File | Purpose |
|------|---------|
| `src/backend/data_provider.hpp` | `IDataProvider` + `DataProviderRegistry` + `addLeaf` overloads (double/string/bool/int64/uint64) |
| `src/backend/psc_power_sensor_provider.hpp/.cpp` | First `IDataProvider` impl: PSC power + temperature sensors |
| `proto/gnmi.proto` | Canonical OpenConfig spec **0.10.0** |
| `proto/gnmi_ext.proto` | Canonical OpenConfig extensions spec |
| `proto/generated/` | Pre-generated `.pb.cc/.pb.h` (committed; no build-time protoc needed) |
| `proto/gen_proto.py` | Maintenance utility — run manually to regenerate `proto/generated/` when proto spec changes |
| `yang/openconfig-platform.yang` | Downloaded from openconfig/public |
| `yang/openconfig-platform-psu.yang` | Downloaded from openconfig/public |
| `meson.build` | Meson build — grpc++/protobuf/boost/C++20; compiles pre-generated proto .cc directly |

---

## Implementation Phases

### Phase 1 — Strip sysrepo, wire extensible backend ✅ build verified

```bash
cd psc-mock-server && meson setup build && ninja -C build
# → produces build/psc-mock-server  (15 MB)
```

**What was done:**
- Sysrepo + libyang fully removed from all source files
- `IDataProvider` + `DataProviderRegistry` + `addLeaf` overloads in `data_provider.hpp`
- `PscPowerSensorProvider` registered via dependency injection in `main.cpp`
- `xpath_to_gnmi_path()` + `gnmi_to_xpath()` with `gnmi::Path` qualification in `utils.h`
- `capabilities.cpp` returns static openconfig-platform-psu model info
- gnmi.proto upgraded to **0.10.0** (matches `spec/gnmi/proto/gnmi/gnmi.proto`)
- Proto codegen moved to **pre-generated** (`proto/generated/`); `meson.build` no longer needs protoc
- `gen_proto.py` kept as maintenance utility — run manually when proto spec changes

**Goal:** ✅ server starts, Subscribe STREAM SAMPLE returns drifting mock PSC values

### Phase 2 — Fix Type B Subscribe bugs
- ✅ Fix POLL `sync_response` missing (`subscribe.cpp:267`) — MUST per spec §3.5.2
- ✅ Fix `updates_only` flag (`subscribe.cpp:139`) — skip initial snapshot when set
- ✅ Fix path filter in `Fill()` — return only leaves matching subscribed path
- Non-existent path closes RPC (`subscribe.cpp:63`) — not applicable; our implementation returns empty updates gracefully, no error propagated
- ✅ Add POLL initial snapshot — aligns with Go ref impl (see note below)
- **Goal:** ✅ ONCE / POLL / STREAM all behave correctly per spec

#### POLL initial snapshot — design note
Spec §3.5.1.5.3 does not explicitly mandate an initial snapshot when a POLL
subscription is established (data retrieval is described as triggered by a
Poll message). However, the OpenConfig Go reference implementation
(`spec/gnmi/subscribe/subscribe.go:435`) sends an initial snapshot +
`sync_response` immediately after subscription setup, before the first Poll
trigger. This gives the client a consistent starting state without requiring
an explicit Poll.

Decision: **implemented** — aligned with the Go reference impl, sends initial
snapshot + `sync_response` at POLL subscription establishment.

### Phase 3 — ON_CHANGE
ON_CHANGE requires a fundamentally different data flow from STREAM SAMPLE.
STREAM SAMPLE calls `Fill()` on every sample interval (pull/periodic);
ON_CHANGE must only emit a Notification when a value actually changes (event-driven).

**Spec requirements (§3.5.1.3):**
- Initial snapshot: **MUST** send updates for all matching paths immediately on subscription establishment, then `sync_response`. Same as ONCE — spec is explicit MUST, not optional.
- Changed values: **SHOULD** only transmit when value changes after the initial snapshot.
- `heartbeat_interval`: if set, **MUST** re-send all values once per interval regardless of change.

**Architecture (aligned with Go reference `spec/gnmi/subscribe/subscribe.go`):**

- Add a per-leaf value cache inside `PscPowerSensorProvider` (previous reading per path)
- `Fill()` compares new reading vs cached; emits only changed leaves
- `subscribe.cpp` STREAM handler: send initial snapshot + `sync_response` first (MUST), then enter loop that calls `Fill()` and emits only if updateList is non-empty
- Add `heartbeat_interval` support: force-emit all leaves periodically even if unchanged

**Notification semantics for ON_CHANGE vs STREAM SAMPLE:**

| | STREAM SAMPLE | ON_CHANGE |
|--|--------------|-----------|
| Trigger | timer (sample_interval) | value change detected |
| Content | full state every tick | only changed leaves |
| `atomic` | `false` — leaves independent | `false` for sensors; `true` only for config objects with coupling (e.g. route entry) |
| `prefix` | not set (full path per Update) | same — not set for sensors; could be set to common ancestor for large subtrees |

**Notification.prefix design decision:**
Spec allows setting `Notification.prefix` to a common path ancestor so each
`Update.path` carries only the relative suffix. For PSC sensors all sharing
`/components/component[name=PSC-x]`, this would reduce wire size. However:
- Go reference does not apply prefix path compression for ordinary STREAM/ON_CHANGE sensor data; prefix is used mainly for `Target`/`Origin` identification and `atomic` batches
- Implementing per-unit prefix requires splitting one `BuildSubscribeNotification()` call into multiple per-unit Notification writes — moderate refactor
- **Decision:** keep full paths in each Update for now; revisit if wire size becomes a concern or if a client requires prefix-relative paths

- **Goal:** ON_CHANGE subscriptions work end-to-end

### Phase 4 — Capabilities + JSON_IETF polish
- Populate `ModelData` with correct name/organization/version from openconfig-platform-psu
- Advertise both JSON and JSON_IETF in `supported_encodings`
- Add `addLeafJsonIetf(list, xpath, json_string)` to `data_provider.hpp` — sets `json_ietf_val`
  instead of individual leaf `double_val` entries; needed because `json_ietf_val` and `string_val`
  are both `std::string` in C++ and cannot share the same `addLeaf` overload
- Switch `PscPowerSensorProvider::Fill()` to return JSON_IETF subtree when encoding is JSON_IETF
  (currently returns individual `double_val` leaves regardless of requested encoding)
- Verify JSON_IETF output has correct YANG namespace prefixes (e.g. `openconfig-platform:`)
- **Goal:** gnmic / telegraf can discover and use the server without manual config

---

## Type B Gaps to Fix (from sysrepo-gnxi analysis)

See `sysrepo-gnxi_GNMI_STATUS.md` Section 11 for full list.
Critical ones for this project:

| Priority | Gap | File | Status |
|----------|-----|------|--------|
| P1 | POLL `sync_response` missing | `subscribe.cpp:267` | ✅ fixed |
| P1 | Non-existent path closes RPC | `subscribe.cpp:63` | ✅ non-issue — returns empty updates, no error |
| P1 | POLL initial snapshot missing | `subscribe.cpp:246` | ✅ fixed — aligns with Go ref impl |
| P1 | Path key filter not applied | `psc_power_sensor_provider.cpp` | ✅ fixed |
| P2 | `ON_CHANGE` not implemented | `subscribe.cpp:216` | Phase 3 |
| P2 | `TARGET_DEFINED` silently ignored (proto3 default = 0) | `subscribe.cpp:159` | ✅ fixed — routes via `IDataProvider::PreferredMode()` per leaf; default SAMPLE |
| P2 | `updates_only` ignored | `subscribe.cpp:139` | ✅ fixed |
| P2 | Unrecognised path returns silent empty instead of `NOT_FOUND`/`UNIMPLEMENTED` | `get.cpp`, `subscribe.cpp`, `data_provider.hpp` | ✅ fixed — Get returns NOT_FOUND; Subscribe logs WARN, RPC not closed |
| P3 | `suppress_redundant` not implemented | `subscribe.cpp:36` | pending |
| P3 | `sample_interval=0` not handled | `subscribe.cpp:213` | ✅ non-issue — loop capped at 200ms, interval=0 fires every iteration = lowest possible |

### Tool note — gnmic poll mode
`gnmic --mode poll` (openconfig/gnmic ≥ 0.46) establishes a POLL subscription
but **never sends Poll trigger messages**; no data is returned. This is a known
limitation of the gnmic CLI — it is distinct from the `gnmi_cli` tool
(openconfig/gnmi) which does send automatic poll triggers via `PollingInterval`.

Use `tests/e2e/test_poll.py` to verify POLL behaviour end-to-end.

---

## Architecture Improvement Directions

Improvements identified during design discussions. Ordered roughly by priority
and dependency (later items often depend on earlier ones).

---

### A. StaticLeafProvider — Template Method Base Class

**Problem:** `PscPowerSensorProvider::fill()` embeds unit-iteration and leaf-matching
logic inline. Any future provider with a fixed leaf list (`PlatformInfoProvider`,
`FanProvider`, …) would duplicate the same loop structure.

**Improvement:** Extract a `StaticLeafProvider` abstract base class using the
Template Method pattern. The base provides the `fill()` implementation; subclasses
only supply a leaf table and per-leaf readers.

```cpp
// Sketch — final API TBD
class StaticLeafProvider : public IDataProvider {
protected:
    struct LeafDef {
        const char*                               suffix;
        std::function<double(const std::string&)> read;
    };
    // Subclass supplies these three:
    virtual std::vector<std::string> resolveUnits(const std::string& xpath) const = 0;
    virtual std::string              basePath(const std::string& unit) const = 0;
    virtual std::span<const LeafDef> leafTable() const = 0;
public:
    // Default fill() calls resolveUnits() + basePath() + leafTable() in a loop.
    void fill(RepeatedPtrField<Update>* list, const std::string& xpath) override;
};
```

**Trade-off:** Adds a base-class indirection. Only worth extracting when 2+ providers
share the structure.

**When:** When `PlatformInfoProvider` (or any second static provider) is added.

---

### B. Leaf Store Architecture

**Why static/dynamic still matters at the business level:**
PSC sensor paths are determined by hardware spec — they never disappear while the
card is running. Alarm paths (`/system/alarms/alarm[id=TEMP-HIGH]`) genuinely appear
and disappear at runtime. This distinction reflects *business semantics*, not a
limitation of gNMI. The architecture should embrace both naturally.

**Problem with current "compute on the fly" design:**
`fill()` currently mixes two separate concerns in one call:
- **Does this path exist?** (answered implicitly by whether fill() adds anything)
- **What is its current value?** (computed fresh every call with rand())

There is also no historical state — every call produces an independent value, so
it is impossible to detect whether a value actually changed.

**Problems this causes:**
1. **ON_CHANGE detection** — need a previous value to diff against; not available.
2. **External value injection** — no way for a background hardware thread (or test
   code) to push updated values into the provider.

**Architecture:**

```
Hardware thread / external caller
       │  store.set(xpath, value)   ← stamps collection time T
       ▼
  LeafStore  ─  thread-safe path→{value, collected_ns} map
       │  store.get(xpath)  /  store.snapshot()  /  store.diff(snapshot)
       ▼
  Provider::fill() — reads from store, not from hardware directly
```

Key components:

| Component | Role |
|-----------|------|
| `LeafStore` | `std::map<std::string, Leaf>` where `Leaf = {gnmi::TypedValue val; int64_t collected_ns;}`, protected by `std::shared_mutex`. Provides `set()`, `get()`, `snapshot()`, `diff(snapshot)`. Stores a per-leaf **collection timestamp**, not just the value (see Timestamp semantics below). |
| `ILeafStoreProvider` | `IDataProvider` subclass whose `fill()` reads from its `LeafStore`. Exposes `setLeaf()` / `removeLeaf()` for external updates. |
| Background thread | Hardware poller or simulator: calls `setLeaf()` at some interval. |

**ON_CHANGE integration (Phase 3 prerequisite):**
- `LeafStore::diff(snapshot)` returns only leaves whose values changed since the snapshot.
- `subscribe.cpp` ON_CHANGE handler: send initial snapshot, take a store snapshot,
  then loop calling `diff()` and emitting only changed leaves.
- `heartbeat_interval`: force full emit periodically even if no change.

**Dynamic leaf management (e.g. AlarmProvider):**
- `setLeaf(xpath, value)` — leaf appears; ON_CHANGE subscriber sees it as a new update.
- `removeLeaf(xpath)` — leaf disappears; Notification `delete` field carries the path.
- gNMI Notification supports both `update` (values) and `delete` (removed paths) in the
  same message — use this for alarm clear events.

**gNMI `delete` field in Notification (spec §2.1):**
```protobuf
message Notification {
    repeated Update update = 4;
    repeated Path   delete = 5;   // paths that were removed
}
```

**Timestamp semantics (spec §2.1, §3.5.2.3):**
`Notification.timestamp` MUST be the time the value was *collected from the underlying
source* (or, for ON_CHANGE, the time the event occurred) — **not** the time the message
is emitted. Under the current compute-on-the-fly design these coincide, so
`subscribe.cpp` using `get_time_nanosec()` ("now") is accidentally correct. Once a
background writer updates the store at time T and emission happens at T+δ, "now" is
wrong. Therefore each leaf carries its own `collected_ns`, and emit uses that.

To keep bundling honest (spec §3.5.2.1 — bundling MUST NOT obscure distinct
timestamps), the background writer updates all leaves of a single tick under one
timestamp T; leaves sharing T can then be bundled into one Notification accurately.

**When:** Phase 3 (ON_CHANGE) is blocked on at least the per-leaf cache part of this.
Full external-injection API is needed when connecting to real hardware.

---

### C. ON_CHANGE — Additional Detail (Phase 3 supplement)

*(Phase 3 is documented above under Implementation Phases. This records design details
decided during discussion.)*

Depends on: **Leaf Store Architecture (B)** for the per-leaf previous-value cache.

**`suppress_redundant` is a SAMPLE-only field** (spec §3.5.1.5.2). ON_CHANGE already
transmits only on change *by definition*, so it does not combine with ON_CHANGE — it
is discussed here only because, like ON_CHANGE's heartbeat, it interacts with
`heartbeat_interval`.

**`heartbeat_interval` + `suppress_redundant` interaction (SAMPLE, spec §3.5.1.5.2):**

| `suppress_redundant` | normal sample tick | heartbeat tick |
|---|---|---|
| false (default) | emit all leaves | emit all leaves |
| true | emit only changed leaves | **MUST emit regardless** — heartbeat overrides suppression |

The key spec rule (previously documented backwards): a heartbeat tick MUST generate
an update *regardless of whether `suppress_redundant` is true*. Heartbeat is a
liveness signal — it forces a full re-emit even when nothing has changed.

`suppress_redundant` is currently P3 in the Type B Gaps table. Implementing it
correctly requires the Leaf Store to track the **current** value per leaf, while the
**last-emitted** state is tracked per-subscriber (in the subscribe handler), never in
the shared store — otherwise concurrent subscribers would corrupt each other's
suppression state.

**ON_CHANGE for dynamic providers (e.g. AlarmProvider):**
gNMI explicitly anticipates leaves appearing and disappearing during the life of a
subscription (STREAM mode MUST NOT close the RPC for a non-existent path). An
AlarmProvider is a natural example: alarm paths like
`/system/alarms/alarm[id=TEMP-HIGH]` appear when an alarm fires and disappear when
cleared. The Leaf Store `removeLeaf()` + Notification `delete` field handle this
cleanly without any special-casing in `subscribe.cpp`.

---

### D. Parent Path Query Support

**Current behavior:** Registered prefix is `/components/component`. A client querying
`/components` alone gets `NOT_FOUND` because no provider prefix matches.

```
gnmic get --path /components   →  NOT_FOUND   (current)
                                  all PSC leaves  (desired)
```

**Options:**
1. Register a synthetic aggregating provider under `/components` that delegates to
   all known component providers.
2. Extend `DataProviderRegistry::fill()` to also dispatch when the *query* is a
   prefix of a *registered* prefix (inverted match direction).
3. Register providers under the shortest relevant prefix so the provider can serve
   the entire subtree.

Option 2 risks partial results if providers only serve specific key instances.
Option 3 shifts more responsibility to individual providers.
Option 1 is most explicit but requires a manual aggregator per subtree root.

**Decision:** Explicitly deferred. Serving full `/components` subtrees is spec-valid
but not required for the PSC use case (clients query specific sensor paths).
Revisit if an operator tool or telegraf config requests `/components` or root `/`.

---

### E. Trie-Based Routing

**Current behavior:** `DataProviderRegistry::fill()` iterates `routes_` linearly:
O(N) string comparison per `fill()` call, where N = number of registered providers.

**Improvement:** Replace the `routes_` vector with a path-segment trie. `fill()`
descends the trie along xpath segments and collects all providers at matching nodes.
O(depth) lookup instead of O(N).

**Trade-off:** Trie adds implementation complexity and only helps when N is large.
With N ≤ 5–10 providers, the linear scan is faster due to CPU cache locality and
no pointer chasing.

**When:** If N exceeds ~10 providers. Not needed for Phase 1–4.

---

### F. YANG Schema Validation

**Current `validateGnmiPath()`** (in `src/utils/utils.h`) checks only structural
integrity: non-empty elem names, non-empty key names. It does NOT validate:
- Whether the path exists in any loaded YANG module.
- Whether required list keys are present (e.g. `component` requires a `name` key).
- Whether leaf value types match the YANG-defined type.

Spec §3.3.4: structurally invalid paths → `INVALID_ARGUMENT`. Paths valid in
syntax but absent from the schema → currently `NOT_FOUND` for unregistered paths
(acceptable for a mock server, but `UNIMPLEMENTED` would be more precise).

**Full YANG validation** options:
- **libyang** — correct but heavyweight; conflicts with the "no sysrepo/libyang" goal.
- **Hardcoded path schema table** (prefix → required keys + allowed leaf names) — lightweight but manual maintenance burden.
- **Path pattern matching** against a compile-time list of known-good paths — middle ground.

**When:** Low priority for a mock server. Revisit if a compliance checker or fuzzer
produces misleading responses for malformed-but-syntactically-valid paths.

---

### G. main.cpp Simplification (Production Cleanup)

*(Already noted in the Deployment Model section at the top of this document.)*

Remove the `getopt` loop and all CLI flags. Replace with build-time constants or a
minimal config file read. `main()` becomes:

```cpp
int main() {
    RunServer(kBindAddr, BuildCreds());
}
```

**When:** After the server is verified end-to-end against real hardware. No
functional changes are required before that point.

---

## Implementation Priority Analysis

### Dependency Graph

```
B (Leaf Store)
    └── C (ON_CHANGE / Phase 3)   ← blocked until B is done

A (StaticLeafProvider)            ← independent, but see ordering note below
D (Parent Path)                   ← independent
E (Trie Routing)                  ← independent
F (YANG Schema Validation)        ← independent
G (main.cpp Simplification)       ← independent
```

**A vs B ordering note:** A extracts the loop structure; B replaces how values are
computed (compute-on-the-fly → read from store). If A is done first, the
extracted base class's `read()` signature will be designed for compute-on-the-fly
and needs to be redesigned again when B is done — two touches to the same code.
Recommended order: B first (migrate `PscPowerSensorProvider` to use a Leaf Store),
then A (extract `StaticLeafProvider` from the already-migrated provider). The base
class emerges in its final form in one pass.

---

### Multi-Dimensional Scoring

| Item | Dependency blocker | Impact (what it unlocks) | Change cost | Deferral cost | Risk | **Score** |
|------|--------------------|--------------------------|-------------|---------------|------|-----------|
| **B** Leaf Store | None | Phase 3, hardware connection, dynamic leaves, proper data/behavior separation | High — rethinks provider data flow | **High** — each new provider added before B is one more class to migrate later | Medium (threading) | **1st** |
| **C** ON_CHANGE | Requires B | Spec Phase 3 feature; enables `heartbeat_interval`, `suppress_redundant` | Medium — new subscribe.cpp path | High once B exists — cheapest right after B | Low (builds on B cleanly) | **2nd** |
| **A** StaticLeafProvider | None (but best after B) | Reduces duplication for future static providers; keeps PSC provider clean | Low-Medium — refactor, no API change | Medium — grows with number of static providers | Low | **3rd** |
| **G** main.cpp cleanup | None | Production deployment readiness | Very Low — isolated to main.cpp | Low — cost stays constant | Negligible | **4th** |
| **D** Parent Path | None | Full-subtree queries (`/components`) work | Medium — registry fan-out change | Low — clients can always be pointed at specific paths | Low | **5th** |
| **F** YANG Schema | None | Correctness of `INVALID_ARGUMENT` vs `NOT_FOUND` for malformed paths | High (libyang) or maintenance burden (hardcoded table) | Low — mock server clients use well-formed paths | Medium (new dependency) | **6th** |
| **E** Trie Routing | None | O(depth) routing instead of O(N) | Medium — internal registry refactor | None — linear scan is fine at current scale | Low | **7th** |

---

### Recommended Implementation Order

| Order | Item | Rationale |
|-------|------|-----------|
| 1 | **B — Leaf Store** | Foundational. Blocks Phase 3. Migration cost is lowest now (1 provider). The longer this is deferred, the more providers need to be migrated simultaneously. |
| 2 | **C — ON_CHANGE (Phase 3)** | Highest-value feature once B is in place. The subscribe.cpp changes build directly on the Leaf Store API. Doing it immediately after B avoids context-switching back to this area later. |
| 3 | **A — StaticLeafProvider** | Do before adding a second static provider (`PlatformInfoProvider`, etc.). At that point the base class emerges from real duplication, not speculation. With B already done, A produces the final-form base class in one pass. |
| 4 | **G — main.cpp cleanup** | Do at production deployment time. Zero interaction with the data layer; safe to defer. |
| 5 | **D — Parent Path** | Revisit only if an operator tool or monitoring system needs subtree queries. |
| 6 | **F — YANG Schema** | Revisit only if a compliance or fuzz-testing requirement appears. |
| 7 | **E — Trie Routing** | Revisit only when N providers exceeds ~10. Premature at current scale.
