# Push Implementation Checklist (ON_CHANGE push-native)

The phased build plan for bringing the implementation up to the **decided** push
design (`protocol-layer-design.md` P1–P5). Subscribe is poll+diff today; this turns
it push-native. Ordered so the cheap conformance fixes and the test safety net land
**before** the one-way-door event substrate.

**Door note.** Tier 0/1 are two-way doors (additive, localized). **Push (Phase 1–2)
is the one remaining one-way door** — the cross-thread event substrate + concurrency
model — so do it deliberately, on top of the Phase 0 test net. P3/P4/P5, C2 and
`suppress_redundant` all *ride* push and fall in after it.

**Source of truth.** Design = `protocol-layer-design.md` (P1–P5, with the ⚠ backlog
pointers) + `core-data-model-design.md` (D6 ValueWriter seam / D14 changeSeq / D17
COW). Implementation gaps = `backlog.md` C1–C5. Don't re-open decided forks; when a
*new* fork surfaces, analyze trade-offs and recommend an option before coding.

---

## Phase 0 — Pre-push punch list (independent, ship first)

Small, decided, no dependencies. **C1 is a real correctness bug** (a spec-conformant
client is wrongly rejected). See `backlog.md` C1–C5.

- [x] **C1 origin boundary helper** — `validateOrigin()` in `src/utils/utils.h`,
      shared by get/set/subscribe (empty→OK, `openconfig`→OK, other→`UNIMPLEMENTED`);
      `gnmi_to_xpath` no longer embeds origin, so the strip-before-`Backend` is free
      at every call site. **Re-attach on responses deferred** (D16 extension point —
      spec mandates only `target` echo (C5), not origin; empty origin ≡ openconfig
      by client convention). Tests: `test_utils` (`ValidateOrigin.*` +
      `GnmiToXpath.OriginIsNotEmbedded`) and `tests/e2e/test_origin.py`. Verified
      `''`→OK, `openconfig`→OK, `cli`→`UNIMPLEMENTED` (path + prefix).
- [x] **C3** reject `TARGET_DEFINED` + client `sample_interval` with `InvalidArgument`
      (setup check in `handleStream`, `sample_interval == 0` stays allowed). Done
      **return-only** — `TryCancel()` makes the client observe `CANCELLED` instead
      (verified), so it is omitted. Test: `tests/e2e/test_target_defined.py` (reject
      + positive establish).
- [x] **C6** (found via C3) — dropped all 7 `context->TryCancel()` setup/build-failure
      rejects in subscribe.cpp; each now delivers its real status (was masked as
      `CANCELLED`). Verified TARGET_DEFINED→`InvalidArgument`, PROTO→`UNIMPLEMENTED`.
- [x] **C4** advertise `openconfig-system` in Capabilities (`kSupportedModels`) —
      added with its real `yang/openconfig-system.yang` revision (`2026-03-31`).
      Verified live: Capabilities lists all three models.
- [x] **C5** echo `prefix.target` on **every** response `Notification.prefix`
      (atomic included). Done via a shared `echoTarget()` (subscribe_emit) stamped
      post-hoc at each build site (Get, `buildSubscribeNotifications`, the ON_CHANGE
      loop) — **target only**, replacing the old non-atomic full-prefix `CopyFrom`
      (which also re-attached origin against C1 and double-prefixed). Set already
      echoes target on `SetResponse.prefix`. Test: `tests/e2e/test_target_echo.py`
      (Get exercises atomic + non-atomic; ONCE; MUST-NOT-set when unset).
- [ ] **#2 C++ emit unit test** — lock current `subscribe_emit.cpp` behaviour
      (atomic partition + `changeSeq` diff) as a regression net for the push rework.
- [ ] **e2e isolation** — each test self-starts a fresh server or restores state on
      teardown (today they share mutable NTP state and fail when run back-to-back).
- **Gate:** 8 C++ suites + all e2e green; emit behaviour pinned by the new C++ test.

## Phase 1 — Core event seam (P1)

- [ ] `ILeafSink` + `ChangeBatch{changed, added, removedPrefixes}` / `LeafChange`
      (enriched payload R1, single unified `onChange` R2).
- [ ] `ValueWriter` assembles `LeafChange` at commit; registry dispatches on
      commit / `attachSubtree`·`registerLeaf` / `detachSubtree`·`unregisterLeaf|Group`.
- [ ] `LeafRegistry` holds an optional `ILeafSink*`; dispatch is a no-op when unset
      (poll/test paths keep working).
- [ ] tests: payload correctness, unset = no-op, `shared_ptr<const ChangeBatch>`
      lifetime across post-unlock dispatch (L=B).

## Phase 2 — Subscription object + threading (P2)

- [ ] Driver-agnostic `Subscription` (`start` / `onPushEvent` / `onDeadline` /
      `nextDeadline`).
- [ ] thread-per-stream loop (`cv.wait_until(nextDeadline())`); per-stream serialized
      writer.
- [ ] subscription index (monitored-prefix → subscriptions), one mutex.
- [ ] single high-water `changeSeq` watermark **captured under the snapshot lock**
      (P3 Fork4 / S8 — not a global value read after an unlocked send).

## Phase 3 — Notification builder on push (P3)

- [ ] `ChangeBatch` → `Notification`(s) (reuse `buildFull`/`buildChange` logic).
- [ ] atomic re-read from core (B1); watermark dedupe; partial-atomic scope (§3.5.2.5,
      payload = subscribed members only).
- [ ] delete granularity = one branch delete; atomic-delete aligned to container
      prefix (S9); default-value transition = `update` not delete (§3.5.2.3).

## Phase 4 — Re-expansion under hot-plug (P4)

- [ ] retain subscription **query paths**; on a structural event re-run
      `collectForSubscription(query)` + diff (B2); update monitored set + index.
- [ ] affected-subscription **linear scan** over live subs (reaches armed
      not-yet-existent subscriptions, S-P4-b — the index alone would miss them).

## Phase 5 — TARGET_DEFINED per-leaf + SAMPLE suppression (P5 / C2 / #4)

- [ ] `resolveMode(leaf)` per leaf at monitored-set entry; tag leaves; the mixed
      ON_CHANGE/SAMPLE partition rides the P1/P2 dual machinery (replaces the current
      per-subscription `preferredMode`, backlog C2).
- [ ] `suppress_redundant` (#4) via the watermark; `heartbeat_interval` overrides.

## Phase 6 — Cutover & validation

- [ ] swap the `subscribe.cpp` poll loop for the push driver (decide: keep the poll
      path as a fallback, or remove — a fork to weigh at that point).
- [ ] new push e2e: a value change is reflected on the wire **immediately**, not at
      the next poll tick.
- **Gate:** all green; ON_CHANGE is push-driven.
