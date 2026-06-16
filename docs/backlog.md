# Implementation / ops backlog

Deferred **implementation and operational** items — distinct from the **design
findings** tracked in [design-review.md](design-review.md) (A–T) and the **protocol
design** in [protocol-layer-design.md](protocol-layer-design.md) (P1–P5). Salvaged
from the retired `DESIGN.md` plus integration follow-ups. Roughly priority-ordered.

| # | Item | Notes |
|---|---|---|
| 1 | **Push-native ON_CHANGE** (event seam + threading) | The big next step. Designed as **P1/P2** in [protocol-layer-design.md](protocol-layer-design.md); subscribe is poll+diff today. Phased build plan: **[push-impl-checklist.md](push-impl-checklist.md)** (Phase 0 punch list → P1 seam → P2 threading → P3/P4/P5). Now well-positioned: a localized swap of the Change Source against a real consumer (the integrated server). |
| 2 | **Re-add a C++ emit unit test** | The old `test_subscribe_emit` was dropped in the core integration (tied to deleted types). The atomic-partition + `changeSeq`-diff logic in `subscribe_emit.cpp` is currently covered only by python e2e; a fast C++ unit test is worth restoring. |
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
