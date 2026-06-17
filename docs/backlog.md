# Implementation / ops backlog

Deferred **implementation and operational** items — distinct from the **design
findings** tracked in [design-review.md](design-review.md) (A–T) and the **protocol
design** in [protocol-layer-design.md](protocol-layer-design.md) (P1–P5). Salvaged
from the retired `DESIGN.md` plus integration follow-ups. Roughly priority-ordered.

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
- **e2e tests are not isolated** — they share one server's mutable state (the NTP record gets mutated/deleted by earlier tests), so they pass only against a *fresh* server and fail when run back-to-back. Make each test self-start a server or restore state on teardown.

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

**Do with the push rework (P1/P2 reshapes these emit/routing paths anyway):**
- **R2 — `echoTarget` is at 3 build sites, not the single `writeAll` sink.** Every
  Subscribe write funnels through `writeAll`, but target is stamped upstream in
  `buildSubscribeNotifications` + the ON_CHANGE loop. The ON_CHANGE site is proof of
  the risk — it bypasses the builder and had to hand-add its own `echoTarget`. A
  future emit path that forgets it silently drops `prefix.target` (§2.2.2.1 MUST).
  Fix: stamp inside `writeAll` (pass the target in). Get has no `writeAll`, so keep
  its call.
- **R3 — `validateOrigin` is wired at ~8 sites while the strip is centralized in
  `gnmi_to_xpath`.** Coverage is complete today, but a future path that calls
  `gnmi_to_xpath` gets origin stripped *without* validation → an unimplemented origin
  (`cli`) served as openconfig. Consider a single request-boundary seam. (Caveat:
  `gnmi_to_xpath` returns a string, can't itself return a Status — needs thought.)

**Batch — ride the next edit that touches these files:**
- **R4 — leftover `if (!status.ok()) { return status; }` braced single-statement
  blocks** (5 sites in [subscribe.cpp](../src/gnmi/subscribe.cpp)) after the C6 `TryCancel` removal; collapse to
  one-liners (the braces only held the deleted call).
- **R5 — Get re-validates the prefix origin once per request path** (the loop calls
  `buildGetNotifications` per path) and opens a second `if (prefix != nullptr)` a few
  lines above the existing one ([get.cpp](../src/gnmi/get.cpp) L69); hoist to `run()` once.
- **R6 — Set's delete loop hand-inlines `validateOrigin`+`checkWritable`** while
  replace/update go through `validateWrite` ([set.cpp](../src/gnmi/set.cpp) L80); asymmetric, two
  validation styles in one RPC.
- **R7 — ON_CHANGE `echoTarget` reads target via `request.subscribe().prefix().target()`**
  while the builder path reads `request.prefix().target()` ([subscribe.cpp](../src/gnmi/subscribe.cpp) L245); same
  value, two expressions → drift risk. Resolve once. (Subsumed if R2 is done.)

**Separate refactor — low priority:**
- **R8 — the 3 new e2e files duplicate** path-builders, the `yield request; while
  True: sleep` iterator, server bootstrap, and `/system` path builders from sibling
  e2e files. Pre-existing suite-wide pattern (no shared module/conftest); extract a
  shared e2e helper. (Overlaps the Minor "e2e tests are not isolated" item above.)

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
