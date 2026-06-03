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
```

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
- Fix POLL `sync_response` missing (`subscribe.cpp:304`)
- Fix non-existent path closes RPC (`subscribe.cpp:63`)
- Fix `updates_only` flag (`subscribe.cpp:139`, `subscribe.cpp:157`)
- **Goal:** ONCE / POLL / STREAM all behave correctly per spec

### Phase 3 — ON_CHANGE (easier without sysrepo)
- Add value cache in `PscPowerSensorProvider` (previous reading per path)
- In `subscribe.cpp` STREAM loop: compare current vs cached, emit only on change
- Add `heartbeat_interval` support
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

| Priority | Gap | File |
|----------|-----|------|
| P1 | POLL `sync_response` missing | `subscribe.cpp:304` |
| P1 | Non-existent path closes RPC | `subscribe.cpp:63` |
| P2 | `ON_CHANGE` not implemented | `subscribe.cpp:216` |
| P2 | `updates_only` ignored | `subscribe.cpp:139` |
| P3 | `suppress_redundant` not implemented | `subscribe.cpp:36` |
| P3 | `sample_interval=0` not handled | `subscribe.cpp:213` |
