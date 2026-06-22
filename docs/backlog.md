# Implementation / ops backlog

Deferred **implementation and operational** items — distinct from the **design
findings** tracked in [design-review.md](design-review.md) (A–T) and the **protocol
design** in [protocol-layer-design.md](protocol-layer-design.md) (P1–P5). Salvaged
from the retired `DESIGN.md` plus integration follow-ups. Roughly priority-ordered.

## Global priority (real-application lens, 2026-06-18)

Ranked by what a gNMI server **on a real PSC card** owes a compliant client —
including boundary behaviour (hot-plug, delete, default-value transitions) — **not**
by "it's a mock" (that framing is the wrong yardstick; the mock is one instance of
the real device, so a "low priority for a mock" / "no e2e uses it" note below should
be re-read through this lens). This **supersedes the implicit "follow
push-impl-checklist Phase 3→6" order**: push's core value landed at Phase 2, so its
tail re-enters this ranking rather than auto-executing. Nothing below is deleted —
this only re-orders.

1. **Runtime hot-plug surface on `Backend`** — *the* minimal-viable-complete gap.
   Pluggability is the PSC device's defining trait, and the modelling is already
   designed (`device-modeling-conventions.md` §3/§6) and the **core** seam exists
   (`attach/detachSubtree`). But `Backend` only exposes **constructor-time**
   `declareLeaf`/`declareGroup`; there is **no runtime attach/detach**, and
   `registry()` is contractually **value-writes-only** (using it for structure desyncs
   `ids_`/`ownedPrefixes_`/`writableConfig_`, backend.hpp:69-71). So **hot-plug is not
   reachable through the supported provider API today.** Scope: surface a
   binding-synced runtime attach/detach on `Backend`; exercise it from a *thin* demo
   (one pluggable unit toggling — not a full shelf model, per README scope); add a
   hot-plug e2e (Get reflects appear/disappear, ON_CHANGE client sees add/delete).
   Additive — no core rework. **✅ DONE 2026-06-22.** Runtime attach/detach + binding
   sync landed earlier; the **wire-observable e2e** now closes it: a sim-only
   `SimControl.SetPresent` service (`proto/sim_control.proto`, `--sim`-gated, separate
   from gNMI so presence never enters the served model — out-of-band hardware events
   are not client actions) → `Backend::injectHardwareEvent` → `Provider::onHardwareEvent`
   → `setPresent`. `tests/e2e/test_hotplug.py` subscribes ON_CHANGE to one slot and sees
   the sensor subtree `delete` on remove and re-add on insert, instantly (element-aligned
   per-slot push). 11 C++ + 15 e2e green.
2. **P4 push-routing for list-level ON_CHANGE** (push-impl-checklist Phase 4) — **← NOW
   NEXT** (#1 done). With hot-plug reachable, a client with ON_CHANGE on
   `/components/component` (bare key-omitted) should be woken **instantly** on
   insert/remove; today only the element-aligned per-slot form
   (`/components/component[name=PSC-0]`) is instant (proven by `test_hotplug.py`), the
   bare list falls back to the ~1s liveness re-diff. Correctness is already safe (the
   ~1s snapshot+diff **catches** structural changes — it does not miss them), so this is
   a latency + routing-mechanism refinement on top of #1, not a data-loss bug. The
   `test_hotplug.py` harness is the ready-made driver: add a bare-list subscription case
   and tighten its assertion from "~1s" to "instant".
3. **#5 `[name=*]` wildcards** — real clients issue wildcard subscriptions; key-omitted
   lists already work, literal `[name=*]` + mid-path multi-key do not.
4. **#6 YANG schema validation** — a real device validates paths/keys/types; re-weigh
   on real-application merit (boundary correctness), not the "low priority for a mock"
   note in the table. Still gated by the libyang-weight trade-off (a hardcoded
   path-schema table is the lighter option).
5. **#3 JSON_IETF encoding** — "更完整", not required: we are read-mostly telemetry, and
   a collector consumes scalar typed values fine. It's an **emit-boundary serializer**
   (View → `json_ietf_val`), additive, **no architectural rework** to add later — so
   optional/deferred. *Separate small honesty fix worth doing regardless:*
   `buildSubscribeNotifications` rejects PROTO as `UNIMPLEMENTED` yet accepts
   JSON/JSON_IETF and then emits scalar (= PROTO-shaped) values — align Capabilities +
   encoding acceptance with what we actually emit.
6. **Deferred — perf / internal / approximation, no client-facing obligation or
   boundary at stake:** S3/P3 (no-relock builder + high-water watermark; pure perf, no
   driver), the driver-agnostic `Subscription` object refactor (readability/
   extensibility, no driver — do it only when you next touch `handleStream` and it
   actually blocks you), P5 per-leaf TARGET_DEFINED (the per-subscription approximation
   works today, C2), #8 trie routing (trigger: concurrent streams → low hundreds),
   #4 `suppress_redundant` (niche), #7 main.cpp (deploy-time).

| # | Item | Notes |
|---|---|---|
| 1 | **Push-native ON_CHANGE** (event seam + threading) | The big next step. Designed as **P1/P2** in [protocol-layer-design.md](protocol-layer-design.md); subscribe is poll+diff today. Phased build plan: **[push-impl-checklist.md](push-impl-checklist.md)** (Phase 0 punch list → P1 seam → P2 threading → P3/P4/P5). Now well-positioned: a localized swap of the Change Source against a real consumer (the integrated server). |
| 2 ✅ | **Re-add a C++ emit unit test — DONE** | Rebuilt against the post-integration types as `tests/test_subscribe_emit.cpp` (11 cases, meson `subscribe_emit`): constructs `Backend::View`s directly to pin atomic partition, `changeSeq` diff (incl. suppress-on-equal-seq), atomic whole-record re-send, delete-container, timestamp-collapse, and `echoTarget`. Replaces the old test that was tied to deleted types. |
| 3 | **JSON_IETF encoding** | Get/Subscribe currently emit individual scalar leaves (`double_val`, …) regardless of requested encoding; Capabilities advertises JSON + JSON_IETF. Real JSON_IETF subtree wrapping (with YANG namespace prefixes) is pending. Needs a `json_ietf_val` path distinct from `string_val`. |
| 4 | **`suppress_redundant` (SAMPLE)** | Client-set, per-leaf; heartbeat overrides it. Spec §3.5.1.5.2. Detailed under **P3** in [protocol-layer-design.md](protocol-layer-design.md); rides the `changeSeq` watermark when push lands. |
| 5 | **Richer wildcards (`[name=*]`)** | Finding **Q** in [design-review.md](design-review.md). The Backend already handles **key-omitted** list queries (`/components/component` → all units, via `selects()`); literal `[name=*]` and mid-path multi-key wildcards are not done. |
| 6 | **YANG schema validation** | `validateGnmiPath()` ([src/utils/utils.h](../src/utils/utils.h)) checks only structural integrity (non-empty names/keys). No check that a path exists in a loaded model, that list keys are present, or that value types match. libyang is heavyweight (and against the no-sysrepo/libyang goal); a hardcoded path-schema table is the lighter option. Low priority for a mock. |
| 7 | **`main.cpp` production cleanup** | Remove the `getopt` CLI loop for the resident-service target; replace with build-time constants / a config file. Isolated to `main.cpp`; do at deployment time. |
| 8 | **Trie-based provider routing** | `Backend` routing is a linear scan over owned prefixes (≤ a handful today). A path-segment trie gives O(depth) instead of O(N) — only worth it past ~10 domains. |

## Push layer — recorded design options (deferred; decided with user during Phase 2)

These are the two deliberate "do the simple thing now, here's the upgrade and its
trigger" calls made while designing the push driver (P2 / S2). Recorded so the
upgrade path is explicit and nobody re-derives it.

- **Subscription routing — linear scan now; query-path trie is the high-N upgrade
  (NOT a monitored-prefix index).** `ILeafSink::onChange` (on the writer thread)
  must find which live subscriptions a `ChangeBatch` touches. We do a **linear scan
  over live subscriptions** for *both* value and structural events — one mechanism,
  zero maintenance, reuses the existing element-aligned matching, and reaches armed
  not-yet-existent subscriptions (P4) for free. Negligible at single-to-low-double-
  digit concurrent streams (P2). **Trigger to revisit: concurrent streams → low
  hundreds.** The elegant high-N form is a **query-path trie** (key = each sub's
  retained query path): it unifies value routing (leaf P → subs whose query is an
  ancestor of P, walk P's ancestors) *and* structural routing (prefix E → subs whose
  query is ancestor-of / descendant-of E, walk ancestors + E's subtree), armed subs
  included, and is maintained **only at sub create/destroy** (query paths are fixed;
  P4 re-expansion changes the materialized set, not the query). This is **cleaner
  than the P2-sketch "monitored-prefix index"**, which would need updating on every
  hot-plug re-expansion *and* still need a scan for armed subs (= two mechanisms). The
  swap is localized to the hub's routing method (Subscription/builder untouched).

- **Subscribe cancellation on sync gRPC — ~1s liveness poll now; cancel-as-event is
  the async/T3 upgrade.** The push loop blocks on `cv.wait_until(min(nextDeadline,
  now+~1s))` and re-checks `ServerContext::IsCancelled()` on the cap, because gRPC's
  **synchronous** server API exposes cancellation only as a *pollable flag*
  (`IsCancelled()`) + a failed `Read/Write`, not as an event that can wake our
  condition variable. This only bites a **fully-idle ON_CHANGE stream** (no pushes,
  no heartbeat) whose client silently disconnects — any push/heartbeat would `Write`
  and catch the cancel immediately. The zero-poll fix is gRPC's **callback/async API**
  (`ServerBidiReactor::OnCancel()` / a CompletionQueue tag), which delivers cancel as
  an event — that is the design's **T3** rung (P2), deferred to thousands of streams.
  Revisit if/when we move to async gRPC, or if the liveness poll ever profiles as a
  concern. (It is a cleanup-latency knob, not a correctness issue.)

## Implementation gaps vs current design (cold-read code review, 2026-06-16)

Cases where the **code lags a *decided* design** (distinct from the table above,
which is genuinely deferred scope). Each is a `protocol-layer-design.md` /
`core-data-model-design.md` claim the implementation does not yet honour. Found by
a Step-2 doc-vs-code pass; priority-ordered. **C1 was a correctness bug** (a
spec-conformant client wrongly rejected) — now **fixed** (✅ below); the rest are
missing/partial behaviour.

| # | Gap | Doc says | Code does | Fix |
|---|---|---|---|---|
| **C1 ✅** | **Origin handling (D16 O2) not implemented; `origin=openconfig` wrongly → `UNIMPLEMENTED`** | boundary defaults empty→`openconfig`, **strips** before core, **re-attaches** on the wire; only a *non-`openconfig`* valid origin → `UNIMPLEMENTED` (N) | ~~`gnmi_to_xpath` **embeds** `origin + ":"`, so any non-empty origin (incl. `openconfig`) missed routing.~~ | **DONE** — `validateOrigin()` ([src/utils/utils.h](../src/utils/utils.h)) shared by get/set/subscribe; `gnmi_to_xpath` no longer embeds origin (strip is free). Re-attach on responses **deferred** (D16 extension point; spec mandates only `target` echo, not origin). Covered by `test_utils` + `tests/e2e/test_origin.py`. |
| **C2** | **TARGET_DEFINED is per-leaf (P5)** — one `Subscription` streams a mix (ON_CHANGE leaves push, SAMPLE leaves tick) | `Backend::preferredMode(xpath)` ([src/backend/backend.cpp](../src/backend/backend.cpp) L108) returns **one** mode for the whole path (any non-Operational leaf ⇒ ON_CHANGE); `resolveStreamMode` tags the whole subscription | True per-leaf mixing needs push (P1/P2), not yet built. For now document P5 as "per-subscription approximation pending push"; implement per-leaf tagging with the push seam. |
| **C3 ✅** | **TARGET_DEFINED + client `sample_interval` is rejected (S-P5-c)** with `InvalidArgument` | ~~no such check; only `> INT64_MAX` guarded.~~ | **DONE** — setup check in `handleStream` ([src/gnmi/subscribe.cpp](../src/gnmi/subscribe.cpp)): `mode==TARGET_DEFINED && sample_interval!=0 → InvalidArgument` (return-only; see C6). Test: `tests/e2e/test_target_defined.py`. |
| **C4 ✅** | **`openconfig-system` is served** (device-modeling §9: `/system/...` + NTP) | ~~Capabilities advertised only `openconfig-platform` + `-psu`; model discovery missed the system model.~~ | **DONE** — added `openconfig-system` to `kSupportedModels` ([src/gnmi/capabilities.cpp](../src/gnmi/capabilities.cpp)) with the real `yang/openconfig-system.yang` revision `2026-03-31` (copied into `yang/`). Verified live. |
| **C5 ✅** | **`prefix.target` MUST be echoed on every response Notification** (R, §2.2.2.1) | ~~Get/Subscribe copied the request prefix onto the non-atomic notification only; atomic notifications dropped `target`.~~ | **DONE** — shared `echoTarget()` stamps target-only onto every notification at each build site ([subscribe_emit.cpp](../src/gnmi/subscribe_emit.cpp); Get, `buildSubscribeNotifications`, ON_CHANGE loop). Replaced the non-atomic full-prefix `CopyFrom` (it re-attached origin vs C1 + double-prefixed). Set already echoes it. Test: `tests/e2e/test_target_echo.py`. |
| **C6 ✅** | **Subscribe error rejects deliver `CANCELLED`, not the intended status** (status-code table: `UNIMPLEMENTED`/`INVALID_ARGUMENT`) | ~~7 sites in subscribe.cpp called `context->TryCancel()` then `return Status(...)`; `TryCancel()` masked the real code as `CANCELLED` (verified during C3).~~ | **DONE** — dropped all 7 `TryCancel()` from the setup/build-failure rejects ([src/gnmi/subscribe.cpp](../src/gnmi/subscribe.cpp)); a returned non-OK `Status` closes the RPC with its own code. Verified: TARGET_DEFINED→`InvalidArgument` (`test_target_defined.py`), PROTO encoding→`UNIMPLEMENTED`. |

Minor (record, low priority):
- **Set rejects an extension-only request** with `UNIMPLEMENTED`; spec §3.4 L1179-1180 says a `SetRequest` carrying only extensions is valid (supporting unknown extension *semantics* is not required — so an OK no-op is the conformant response).
- **Atomic whole-record delete** emits `add_delete_()` with an empty path under the container prefix ([src/gnmi/subscribe_emit.cpp](../src/gnmi/subscribe_emit.cpp) L162-167) — relies on `prefix == container` to mean "delete the container"; works but subtle.
- **e2e tests are not isolated** — ✅ **RESOLVED.** Each test now self-starts a fresh
  server via the pytest `gnmi_server` fixture (`tests/e2e/conftest.py`), so the
  Set/delete tests no longer share mutable state; the suite is order-independent and
  back-to-back-safe (verified). (See R8 above.)

## Code-review follow-ups (`/code-review high` on `1f3ab3d`, 2026-06-17)

A high-effort multi-angle review of the C1–C6 conformance commit. **No crashes**;
the diff is otherwise confirmed correct (origin validation covers every Get/Set/
Subscribe entry incl. updates_only/POLL/ONCE; `echoTarget` reaches every current
emit path; the 7 `TryCancel` removals are return-only). Findings R1–R8, triaged by
when to act. Numbered R* to not clash with the tables above.

**Behavioral:**
- **R1 ✅ DONE — `TARGET_DEFINED`→SAMPLE streamed at ~5 Hz (no server-default interval).**
  `resolveStreamMode`→`preferredMode` ([backend.cpp](../src/backend/backend.cpp) L108) returns only the *mode*, never an
  interval; the sub keeps its own `sample_interval`, which for `TARGET_DEFINED` is
  forced to **0** (C3 rejects non-zero). In the SAMPLE tick the due-test
  `elapsed > nanoseconds{sample_interval()}` ([subscribe.cpp](../src/gnmi/subscribe.cpp) L215) is then
  `elapsed > 0` → due every ~200 ms loop iteration (L251 cap) → 5 Hz flood. Design
  P5 says target-defined SAMPLE leaves should get a **single server-default
  interval**. Fix: when a sub resolves to SAMPLE with `sample_interval==0`, stamp a
  server-default interval (at the chronomap insert, [subscribe.cpp](../src/gnmi/subscribe.cpp) L186-187), and
  make `tests/e2e/test_target_defined.py` assert cadence (not just `sync_response`,
  which currently masks it). **Orthogonal to push** — P2 swaps the poll loop for
  `wait_until(nextDeadline)` but interval==0 still spins, so this interval-value fix
  carries over and is not wasted now. (Full per-leaf TARGET_DEFINED is deferred C2.)
  **Resolved**: `kTargetDefinedSampleIntervalNs` (1s, matched to the sensor drift
  tick) stamped at the chronomap insert ([subscribe.cpp](../src/gnmi/subscribe.cpp) L186) for
  `TARGET_DEFINED`→SAMPLE only — an explicit SAMPLE+0 keeps the 200ms floor.
  `test_target_defined.py` positive case now asserts the steady-state gap > 0.5s
  (the old flood would fail it).

**Done this session (commit `refactor(gnmi): code-review cleanups`):**
- **R2 ✅ DONE — `echoTarget` moved into the single `writeAll` sink.** Was stamped at
  3 build sites (`buildSubscribeNotifications` + the ON_CHANGE loop); now `writeAll`
  takes the target and echoes once, so every Subscribe emit path gets it uniformly and
  a future path can't forget it. Get keeps its own call (it has no `writeAll`). Pinned
  by `test_subscribe_emit.cpp` (EchoTarget) + `test_target_echo.py`.
- **R4 ✅ DONE** — collapsed the 5 `if (!status.ok()) { return status; }` braces to
  one-liners in [subscribe.cpp](../src/gnmi/subscribe.cpp).
- **R5 ✅ DONE** — Get's prefix-origin check hoisted to `run()` (once); the per-path
  re-validation and the double `if (prefix != nullptr)` are gone ([get.cpp](../src/gnmi/get.cpp)).
- **R6 ✅ DONE** — delete/update/replace now all validate through a shared
  `validatePath` ([set.cpp](../src/gnmi/set.cpp)); no more asymmetric inline.
- **R7 ✅ DONE (subsumed by R2)** — target is sourced once per handler and passed to
  `writeAll`, so the two-expression drift is gone.

**Deferred to the push rework:**
- **R3 — single origin-validation seam.** `validateOrigin` is wired at the get/set/
  subscribe boundaries while the strip is centralized in `gnmi_to_xpath`; coverage is
  complete today, but a future path calling `gnmi_to_xpath` would strip origin without
  validating. A structural single seam is awkward (`gnmi_to_xpath` returns a string,
  can't carry a Status), and its proper home is the **push Resolver (pipeline step 1)**.
  Documented for now as an INVARIANT comment on `gnmi_to_xpath` ([utils.h](../src/utils/utils.h)); build the
  real seam with the Resolver.

**Separate refactor — low priority:**
- **R8 ✅ DONE — shared e2e harness extracted.** The whole `tests/e2e/` suite is now
  pytest: `tests/e2e/gnmi_helpers.py` holds the path-builders (`gpath`/`psc_path`/
  `ntp_config_path`/`sys_config_path`), `path_to_str`, the `hold_open` subscribe
  iterator, and the atomic leaf extractors (`leaf_names`/`leaf_map`); `tests/e2e/
  conftest.py` holds the server bootstrap (`gnmi_server` fixture) + `stub`. The
  per-file duplication is gone. (Done together with the isolation fix below.)

## Resolved (recorded so they are not re-raised)

- **Parent-path queries** (old DESIGN.md item D — `/components` returned NOT_FOUND).
  Resolved by the core integration: the element-aligned core matches `/components`
  over keyed entries, and the Backend expands **key-omitted** list paths
  (`/components/component`) to all entries (`selects()`), so both the container and
  key-omitted forms now return all units. (Full `[name=*]` wildcard remains #5.)

## Tooling note — gnmic poll mode

`gnmic --mode poll` establishes a POLL subscription but **never sends Poll trigger
messages**, so no data is returned — a known gnmic CLI limitation, distinct from
`gnmi_cli` (which auto-triggers via `PollingInterval`). Use
[tests/e2e/test_poll.py](../tests/e2e/test_poll.py) to verify POLL end-to-end.
