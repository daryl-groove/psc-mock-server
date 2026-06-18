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
- [x] **#2 C++ emit unit test** — `tests/test_subscribe_emit.cpp` (11 cases, meson
      target `subscribe_emit`) constructs `Backend::View`s directly and pins the
      atomic partition + `changeSeq` diff (incl. suppress-on-equal-seq) + atomic
      whole-record re-send + delete-container + timestamp-collapse + `echoTarget`
      behaviour deterministically — the regression net for the push rework.
- [x] **e2e isolation** — migrated the 10 standalone scripts to a pytest suite
      (`pytest tests/e2e`). A `tests/e2e/conftest.py` `gnmi_server` fixture spawns a
      **fresh server per test** on a private port (`-b 127.0.0.1:<free>`, readiness
      via `channel_ready_future`, torn down after) — so the Set/delete tests no
      longer poison each other; verified by running the suite back-to-back and the
      formerly-destructive tests 3× (all green). Shared helpers extracted to
      `tests/e2e/gnmi_helpers.py` (path-builders, `hold_open` subscribe iterator,
      leaf extractors) — backlog **R8** done with it. `gnmic` test skips when the CLI
      is absent. (pytest is a user-site dev dep; grpcio stays system-wide.)
- **Gate:** ✅ 9 C++ suites + 11 e2e green; emit behaviour pinned by the new C++ test.

## Phase 1 — Core event seam (P1) ✅ DONE

`src/core/leaf_sink.hpp` + the registry wiring; **additive, no consumer yet** (the
server attaches no sink, so the poll path is byte-for-byte unchanged — 11 e2e green).
Implementation forks decided with the user (recorded in core D6 / P3 Fork 1–2):

- [x] **`ILeafSink` + `ChangeBatch{changed, added, removedPrefixes}` / `LeafChange`**
      (`leaf_sink.hpp`). Enriched payload R1 (path D16/L=B + value D17 handles, changeSeq,
      collectedNs captured at commit); single `onChange(shared_ptr<const ChangeBatch>)`
      **(Fork 2)**, `noexcept` **(尖角 2)**.
- [x] **`ValueWriter` assembles the batch at commit; registry dispatches AFTER unlock.**
      `setValueLocked` appends an enriched `LeafChange` on a real (value-gated) change;
      the writer's destructor releases the exclusive lock **then** calls
      `dispatch()` — the sink never runs under the write lock **(Fork 1: destructor,
      not explicit commit())**. `ValueWriter` is now **non-movable** (guaranteed copy
      elision means no move is needed; removes the moved-from-husk hazard, 尖角 1).
- [x] **Structural sources**: `registerLeaf`/`attachSubtree` → `added` (unset leaves
      included faithfully, Fork 3b); `detachSubtree`/`unregisterLeaf` → branch-level
      `removedPrefixes`. **`unregisterGroup` emits nothing (Fork 4 carve-out:
      ungroup ≠ delete)** — reconciled in core D6 / P3 Fork 2.
- [x] **`LeafRegistry` holds an optional `ILeafSink*` (`setSink`); no-op when unset** —
      and the batch is **not even built** without a sink, so the poll/test path pays
      nothing (Fork 5).
- [x] **tests** — `tests/core/test_leaf_sink.cpp` (9 cases): enriched changed payload,
      value-gated no-op records nothing, no-sink = no dispatch, `added` incl. unset,
      attachSubtree one-batch, detach/unregisterLeaf removedPrefixes, unregisterGroup
      no-event, and the L=B lifetime (batch outlives the writer scope **and** a later
      detach). 10 C++ suites green.

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
