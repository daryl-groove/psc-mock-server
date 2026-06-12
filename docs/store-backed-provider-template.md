# StoreBackedProvider base + per-leaf typed declaration

Status: **IMPLEMENTED (uncommitted), 2026-06-12.** Sits on top of `67535f3` (DataType
filtering with `schemaType` subtree lookup). This refactor unifies how providers are
built so future ones extend with one pattern. It **supersedes the `schemaType`
subtree-prefix approach** in `67535f3` (see "Change from committed" below). 7/7 unit
+ 7/7 e2e green.

## As built (deviations from the sketch below)

- **`DeclaredLeaf`, not `SeededLeaf`.** The one hook is `declareLeaves()`, and its
  value is `std::optional<gnmi::TypedValue>`: `nullopt` = declared (writable / typed)
  but NOT seeded. This makes "declared-but-unset" a first-class row in the table.
- **timezone-name decision = declare-but-don't-seed** (the doc's first option).
  `/system/config/timezone-name` is declared `Config` with a `nullopt` value, so
  `ApplyBatchCreatesNewLeaf` keeps its meaning (Set creates a declared-but-unset
  leaf). A new `UndeclaredConfigLeafIsNotWritable` test covers the new gate (an
  undeclared `/system/config/...` path is refused).
- **`commitStamped(const WriteBatch&)`** (const ref, not by value): it rebuilds a
  stamped batch internally, so borrowing the input avoids a copy.
- **`initLeaves()` is called from each subclass ctor, not the base ctor** — a base
  ctor cannot dispatch to the derived `declareLeaves()`. The sensor calls it before
  starting `sim_`; the sim commits each tick through `commitStamped` (a callback),
  so its leaves are *decided* Operational from `schema_`, not defaulted.
- **Header-only base** (`store_backed_provider.hpp`): the methods are small and
  this avoids adding a `.cpp` to the build target and every test target.
- **Single `typedValue()` wrapping authority.** Both providers' `declareLeaves()`
  now build their `DeclaredLeaf` values through one set of `typedValue()` overloads
  in `data_provider.hpp` (the scalar→`gnmi::TypedValue` mapping lives in exactly one
  place); `WriteBatch::set`'s typed overloads delegate to it too. So both providers
  declare leaves with the identical shape `DeclaredLeaf{path, type, typedValue(x),
  ts}` — only the literal `x` differs (irreducible: sensor doubles vs config mixed
  types). A `typedValue(const char*)→string` overload kills the const-char*→bool
  footgun at declaration sites.

## Why

The two providers diverge in two ways that hurt the "template for future providers"
goal:

1. **Boilerplate duplication.** Both own a `LeafStore`, both `fill()` =
   `store_.collect(...)`, both `snapshot()` = `store_.snapshot(...)`, both seed a
   `WriteBatch` in the ctor.
2. **Leaf type is decided two different ways.** SystemConfig stamps it via
   `applyStamped`/`schemaType`; the sensor relies silently on `Leaf`/`WriteOp`'s
   default `Operational`. So "where is a leaf's type decided?" has no single answer
   — exactly what breaks a template.

## Design

Pattern: **Layer Supertype (Fowler) + Template Method**, with a **declarative,
data-driven leaf table**. `IDataProvider` is the registry's polymorphic seam;
`StoreBackedProvider` is the shared-implementation layer for the family of
store-backed providers; the base owns the skeleton and subclasses fill thin hooks.
This is the canonical solution for "shared implementation across a family of
polymorphic types" — not composition/Strategy, which is for *orthogonal* axes of
variation. Ours cluster (config↔writable↔event-driven; sensor↔read-only↔sim), so a
single inheritance axis is simpler and sufficient; pushing the `atomicPrefix`/sim
*logic* into pure data-config would only spawn callbacks. Guiding principle:
**push variation into the declarative table; keep behaviour as thin overridable
hooks.**

**Each provider declares its leaves with their type at the definition site; a
`StoreBackedProvider` base drives seed/fill/snapshot/writable/type-stamping
uniformly. A leaf's type is decided in ONE place: that declaration.**

```cpp
// Per-leaf schema, declared explicitly. Type is hardcoded here — not computed
// from a path substring. Supports mixed types in one provider (each entry its own).
struct LeafDef { std::string path; LeafType type; };

class StoreBackedProvider : public IDataProvider {
protected:
    LeafStore store_;
    // The provider's declared schema: path -> LeafType. Built at construction.
    // Stable, independent of current data (a leaf may be declared but unset).
    // Used for both writable() and stamping a leaf's type at creation.
    std::map<std::string, LeafType> schema_;

    // The single leaf-creation entry point: stamp each op's type from schema_,
    // then commit. Every provider (seed, Set, simulator) creates leaves this way.
    void commitStamped(WriteBatch b);
    LeafType schemaType(const std::string& path) const;  // schema_ lookup

    // The single Template-Method hook: a subclass produces its leaves (from its own
    // richer table — sensor's walk params, config's defaults) as a normalised view.
    // The base ctor calls it to seed (via commitStamped) and to build schema_.
    struct SeededLeaf { std::string path; LeafType type; gnmi::TypedValue value;
                        int64_t collectedNs; };
    virtual std::vector<SeededLeaf> declareLeaves() const = 0;
public:
    void fill(RepeatedPtrField<Update>* l, const std::string& x) const override
        { store_.collect(x, l); }
    Snapshot snapshot(const std::string& x) const override
        { return store_.snapshot(x); }
    bool writable(const std::string& p) const override
        { return schemaType(p) == LeafType::Config; }
};
```

- **leaf type decided once:** in each provider's `LeafDef` declaration. Uniform
  answer across all providers.
- **mixed types per provider:** fully supported and expected (OpenConfig mixes
  config + state). The disjoint invariant is *per leaf*, not per provider — no
  conflict.
- **writable + type share `schema_`** — one source of truth, no drift, no path
  string heuristic.

Each provider then shrinks to its essentials:
- **PscPowerSensor:** declares its sensor leaves (all `Operational`) + keeps the
  `sim_` jthread (the sim commits via `commitStamped`, so its leaves are *decided*
  Operational, not defaulted). Overrides nothing else.
- **SystemConfig:** declares its config leaves (`Config`) + keeps `atomicPrefix`,
  `applyBatch` (→ `commitStamped`), `preferredMode` = ON_CHANGE.

## Stays in subclasses (do NOT pull into the base — essential divergence)

Sensor's `sim_` jthread + walk metadata; config's atomic containers, write side,
ON_CHANGE mode. The base stays thin: store + creation + read + the writable rule.

## Change from committed (`67535f3`)

`schemaType` becomes a `schema_` **table lookup** (explicit per-leaf type), replacing
the `CONFIG_SUBTREES` prefix match. `isConfigPath` is already gone; `CONFIG_SUBTREES`
goes too.

**Behaviour/test impact to decide at implementation:** with a per-leaf schema, a Set
to an **undeclared** path is not writable → rejected (schema = the leaves the server
declares; consistent with "the server provides the leaves"). This changes
`ApplyBatchCreatesNewLeaf` (which sets the unseeded `timezone-name`): either declare
`timezone-name` in the schema, or change that test to expect rejection of an
undeclared path.

## Open implementation wrinkles (resolve when coding)

- **LeafDef vs richer definitions.** The sensor's real definition carries walk
  metadata (nominal/step/maxDev), config's carries a default string value. So the
  base should consume a minimal `{path, type, initialValue}` list that each provider
  *produces* from its own richer table (sensor from `buildWalks()`, config from its
  defaults) — the base does not impose one definition struct on everyone.
- **How the base gets `schema_`.** Provider passes its declared `{path,type}` set to
  the base ctor, or registers it before first use.
- **Atomic/NTP leaves.** Still declared per-leaf (all `Config`); `atomicPrefix`
  stays a SystemConfig override, unrelated to type.

## Further-future (record, do NOT do now)

Schema currently lives per-provider. The *most faithful* gNMI model is one
device-wide schema: a central Schema registry every provider registers its leaves
with, and `writable`/type lookups consult. Closer to a real target (one YANG
schema), but heavier and more coupled — per-provider declaration is simpler and the
registry already partitions by prefix. Seam over infrastructure: revisit only if a
real need appears.

## References

Builds on docs/leaf-type-and-schema-model.md (LeafType, type-at-creation) and the
long-noted DESIGN.md §A "StoreBackedProvider". Second provider now justifies it.
