# psc-mock-server — documentation index

A **gNMI server** that models an **ORv3 PSC (Power Supply Controller) card**,
exposing mock OpenConfig telemetry/config to gNMI clients. It doubles as a
worked **gNMI implementation example**: the "PSC card" is the concrete device the
example models (hot-pluggable PSU/BBU, sensors, system config). Primary goal:
exercise the gNMI pipeline (Capabilities / Get / Set / Subscribe) end-to-end
before connecting to real hardware.

---

## Architecture (current)

```
gNMI client ──gRPC──► GNMIService            (src/gnmi/)
                        Capabilities / Get / Set / Subscribe
                          │
                          ▼
                      gnmid::Backend          (src/backend/)
                        • routing / namespace ownership (UNIMPLEMENTED vs NOT_FOUND)
                        • writability + schema plane (declared config, persists across delete)
                        • path→LeafId bindings; Set commit; key-omitted list expansion
                          │                                  ▲
                          │ reads/writes                     │ register leaves/groups,
                          ▼                                  │ push values (ValueWriter)
                  gnmid::core::LeafRegistry  (src/core/)     │
                    path-keyed leaf store + notification     │
                    groups (the data layer; gRPC-free)       │
                          ▲                                  │
                          └────────── providers ─────────────┘
                                 PscPowerSensorProvider (sensors, simulator jthread)
                                 SystemConfigProvider   (config, atomic NTP record)
```

- **`gnmid::core`** ([src/core/](../src/core/)) is the pure data layer — a
  path-keyed leaf store + notification groups, no gRPC. Designed in
  [core-data-model-design.md](core-data-model-design.md).
- **`gnmid::Backend`** ([src/backend/](../src/backend/)) is the device/schema
  layer the core deliberately omits: routing, the writability/schema plane, and
  provider wiring. **Providers** populate the registry and push values (the
  push-bridge model).
- **Subscribe is poll-mode** today: ON_CHANGE is detected by polling
  `collectForSubscription` + `changeSeq` diff. Push-native delivery is designed
  (P1/P2 in [protocol-layer-design.md](protocol-layer-design.md)) but not built.

**Adding a data domain:** write a `gnmid::Provider` subclass in `src/backend/`
and one `backend.addProvider(...)` line in `src/main.cpp`; the gNMI layer is
untouched.

---

## Build & test

```bash
meson setup build && cd build && ninja      # build (produces build/psc-mock-server)
meson test                                  # C++ unit tests (core + backend + utils)

# end-to-end (python, against a running server):
./build/psc-mock-server --force-insecure --log-level 1   # then, in tests/e2e/:
python3 tests/e2e/test_onchange.py          # (and the other test_*.py)
```

- **Meson + C++20** (`std::jthread`/`std::shared_mutex`); deps grpc++ / protobuf /
  boost(log). Proto is generated at build time (`protoc` custom targets) and shared
  via one `gnmi_pb` static lib.
- Quick command reference: [test_cmd.txt](test_cmd.txt).

---

## Source layout

| Path | What |
|---|---|
| [src/core/](../src/core/) | `gnmid::core` data layer (canonical path, leaf registry, groups) |
| [src/backend/](../src/backend/) | `gnmid::Backend` + `Provider` base + the two providers |
| [src/gnmi/](../src/gnmi/) | gNMI service: capabilities / get / set / subscribe (+ emit helpers) |
| [tests/core/](../tests/core/) | core unit tests | 
| `tests/test_backend.cpp`, `tests/test_utils.cpp` | backend + utils unit tests |
| [tests/e2e/](../tests/e2e/) | python wire-level e2e (atomic / onchange / poll / set / get / gnmic) |

---

## Deployment

Final target is a **resident service on the PSC card** (auto-start, no operator
input). Today it takes CLI args (`--bind`, `--cert`, `--force-insecure`, …) for
development convenience; simplifying `main.cpp` to build-time/config constants is a
production-cleanup item ([backlog.md](backlog.md)).

---

## Document map

**Current design (authoritative):**
- [core-data-model-design.md](core-data-model-design.md) — the `gnmid::core` data layer, decisions D1–D18.
- [protocol-layer-design.md](protocol-layer-design.md) — gNMI protocol layer; Subscribe modes, the push design P1–P5 (future), RPC status codes.
- [device-modeling-conventions.md](device-modeling-conventions.md) — how a device maps onto the core (slots, hot-plug, dynamic leaves); §8 layering; the concrete PSC path model.
- [design-review.md](design-review.md) — the core/protocol review findings A–T and their status.

**Other:**
- [backlog.md](backlog.md) — deferred implementation / ops items.
- [test_cmd.txt](test_cmd.txt) — manual gNMI command reference.
- [archive/](archive/) — superseded docs kept for history (the pre-integration backend design, the new-core build checklist, exploratory notes). Describe deleted code; not current.
