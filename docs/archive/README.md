# Archive — historical / superseded docs

These describe **deleted code** or **completed work** and are kept only for history
and the reasoning trail. They are **not current** — see [../README.md](../README.md)
for the live docs.

| Doc | What it was | Superseded by |
|---|---|---|
| `DESIGN.md` | Pre-integration server design (old `LeafStore` / `DataProviderRegistry` / `IDataProvider` backend) | `../README.md` + the current design docs; deferred items → `../backlog.md` |
| `IMPL-CHECKLIST.md` | Build checklist for the `gnmid::core` greenfield (all boxes done, core now integrated) | `../core-data-model-design.md` + git history |
| `store-backed-provider-template.md` | Old `StoreBackedProvider` base (deleted) | `gnmid::Provider` + `gnmid::Backend` (`../device-modeling-conventions.md` §8) |
| `leaf-type-and-schema-model.md` | LeafType / schema-as-source-of-truth (old backend impl) | `src/core/leaf_type.hpp` + `../device-modeling-conventions.md` §1 |
| `onchange-delivery-and-source-binding.md` | Exploratory push-vs-poll / write-side seam discussion | `../protocol-layer-design.md` P1–P5 + `../device-modeling-conventions.md` §8.3 |
