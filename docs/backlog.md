# Implementation / ops backlog

Deferred **implementation and operational** items — distinct from the **design
findings** tracked in [design-review.md](design-review.md) (A–T) and the **protocol
design** in [protocol-layer-design.md](protocol-layer-design.md) (P1–P5). Salvaged
from the retired `DESIGN.md` plus integration follow-ups. Roughly priority-ordered.

| # | Item | Notes |
|---|---|---|
| 1 | **Push-native ON_CHANGE** (event seam + threading) | The big next step. Designed as **P1/P2** in [protocol-layer-design.md](protocol-layer-design.md); subscribe is poll+diff today. Now well-positioned: it's a localized swap of the Change Source against a real consumer (the integrated server). |
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
a Step-2 doc-vs-code pass; priority-ordered. **C1 is a correctness bug** (a
spec-conformant client is wrongly rejected); the rest are missing/partial behaviour.

| # | Gap | Doc says | Code does | Fix |
|---|---|---|---|---|
| **C1** | **Origin handling (D16 O2) not implemented; `origin=openconfig` wrongly → `UNIMPLEMENTED`** | boundary defaults empty→`openconfig`, **strips** before core, **re-attaches** on the wire; only a *non-`openconfig`* valid origin → `UNIMPLEMENTED` (N) | `gnmi_to_xpath` ([src/utils/utils.h](../src/utils/utils.h) L55-56) **embeds** `origin + ":"` into the xpath string — never defaults/strips/validates/re-attaches. Any non-empty origin (incl. the canonical `openconfig`) misses routing → `UNIMPLEMENTED`. Verified: `''`→OK, `openconfig`→`UNIMPLEMENTED`, `cli`→`UNIMPLEMENTED`. | One boundary helper shared by get/set/subscribe: empty→`openconfig`, accept `openconfig`, other valid origin→`UNIMPLEMENTED`, strip before `Backend`, (optionally) re-attach on responses. Stop embedding origin in `gnmi_to_xpath`. |
| **C2** | **TARGET_DEFINED is per-leaf (P5)** — one `Subscription` streams a mix (ON_CHANGE leaves push, SAMPLE leaves tick) | `Backend::preferredMode(xpath)` ([src/backend/backend.cpp](../src/backend/backend.cpp) L108) returns **one** mode for the whole path (any non-Operational leaf ⇒ ON_CHANGE); `resolveStreamMode` tags the whole subscription | True per-leaf mixing needs push (P1/P2), not yet built. For now document P5 as "per-subscription approximation pending push"; implement per-leaf tagging with the push seam. |
| **C3** | **TARGET_DEFINED + client `sample_interval` is rejected (S-P5-c)** with `InvalidArgument` | no such check; [src/gnmi/subscribe.cpp](../src/gnmi/subscribe.cpp) only guards `> INT64_MAX`, `resolveStreamMode` ignores a pinned interval | Add a setup check: `mode==TARGET_DEFINED && has sample_interval → InvalidArgument`. |
| **C4** | **`openconfig-system` is served** (device-modeling §9: `/system/...` + NTP) | Capabilities ([src/gnmi/capabilities.cpp](../src/gnmi/capabilities.cpp) L32-35) advertises only `openconfig-platform` + `-psu`; model discovery misses the system model | Add `openconfig-system` to `kSupportedModels`. |
| **C5** | **`prefix.target` MUST be echoed on every response Notification** (R, §2.2.2.1) | Set echoes it; Get/Subscribe copy the request prefix onto the **non-atomic** notification only — atomic notifications carry their container prefix and **drop** `target` | Carry `target` onto every emitted `Notification.prefix` (atomic included). |

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
