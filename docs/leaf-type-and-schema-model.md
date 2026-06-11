# LeafType & the schema-as-source-of-truth model

Status: **IMPLEMENTED (uncommitted), 2026-06-11.** Built exactly as the plan below
(sub-choices a=store-on-leaf, b=subtree-list). 7/7 unit (incl. new
`ResetAfterDeleteKeepsConfigType`) + 4/4 e2e green; live-verified that setting an
unseeded config leaf (`/system/config/timezone-name`) succeeds and filters as Config.
Sits on top of checkpoint commit `183005`. This doc captures how a leaf's data type
is determined.

## The concept (agreed)

A leaf's type (`Config` / `State` / `Operational`) is **the server's internally
defined schema — decided at initialisation, stable, and independent of current
data**. Everything else follows from this single fact:

- The **client never supplies** a type. gNMI `SetRequest` updates are `{path,
  value}` only — no type field. A Set merely *populates a value* for a path the
  server's schema already defines as config; it does not "create" or "type" a leaf.
- The type is **not inferred from the path string**. Path naming is free; the
  server declares config-ness explicitly. (This is why `isConfigPath` — which keys
  off the literal segment `"/config/"` — is to be removed.)
- The schema is **separate from the data**. Deleting a config leaf, or setting a
  schema-valid leaf that has no value yet, both remain valid — because the schema
  (what is writable, what type each path is) outlives any particular value.

This resolves two earlier warts:
- **Q1** — `snapshot()` blindly stamping `Config` was a read-time guess; type
  should come from the schema, bound to the leaf.
- **Q2** — `writable = isConfigPath(path)` made the path string load-bearing;
  writability is a schema fact the server declares, not a naming convention.

## Plan

Single source of truth: **each provider declares its schema at construction; type
and writability both derive from it.**

1. **Explicit schema on the provider** (replaces `isConfigPath`).
   `LeafType schemaType(const std::string& path) const`, backed by an explicit
   declaration built in the ctor. SystemConfigProvider declares its config subtrees
   (`/system/config`, `/system/ntp`) → `Config`; default `Operational`.

2. **Type bound to the leaf at creation, from the schema.**
   `WriteOp` carries `LeafType`. A provider's `applyBatch`/seed stamps
   `op.type = schemaType(op.path)` before commit. `LeafStore::commit` stores
   `op.type` on the leaf. `schemaType` is deterministic, so a value-update re-stamps
   the same type — no drift, no create-vs-update branch needed. Sensor seed/sim
   stamp `Operational`. `snapshot()` then just carries the stored type (the blind
   `Config` stamp loop is removed).

3. **`writable` derives from the schema** (delete `isConfigPath`).
   `writable(path) = schemaType(path) == Config`. Because the schema is independent
   of data, delete-then-reset and setting schema-valid-but-unset paths both work
   (faithful to gNMI, and to "the server provides the leaves").

4. **Get filter unchanged** — already reads `it->second.type`.

5. **Tests.** `ApplyBatchCreatesNewLeaf` now *succeeds* with type `Config` (correct).
   Add a delete-then-reset test asserting the re-set leaf is `Config`.
   `NonConfigPathIsNotWritable` (`/system/state`) still passes (not in a declared
   config subtree).

## Files touched

`data_provider.hpp` (WriteOp gains `LeafType`), `leaf_store.cpp` (commit stores
type), `system_config_provider.{hpp,cpp}` (`schemaType`, applyBatch/seed stamp,
`writable`, remove `isConfigPath`, drop the snapshot stamp loop),
`psc_power_sensor_provider` (seed/sim stamp `Operational`), tests. **This touches
the recently-stabilised write path (WriteOp/commit/applyBatch) — re-run the Set
suite to confirm green.**

## Sub-choices (DECIDED)

- **(a) Where type lives — STORE ON THE LEAF AT CREATION.** Type is stamped from
  `schemaType` into the leaf when it is created and carried thereafter (the plan in
  §2). Matches "leaf bound, decided at init". Accepts the write-path churn
  (WriteOp/commit/applyBatch).
- **(b) Schema shape — EXPLICIT SUBTREE PREFIX LIST.** A provider declares its
  config subtrees (`/system/config`, `/system/ntp`) at construction. Prefix matching
  under the hood, but an explicit server declaration — not the `"/config/"`
  substring heuristic — so path names are free.

## Not in scope / deferred

Real per-leaf YANG schema annotation (a full schema registry) is the "true" model
a real target has; the explicit per-provider declaration above is the lightweight
stand-in. Revisit only if a provider grows genuinely mixed config/state/operational
data that a coarse subtree declaration can't express.
