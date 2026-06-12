/*
 * StoreBackedProvider — Layer Supertype for providers backed by a LeafStore.
 *
 * The shared skeleton for the family of providers that keep their data in a
 * LeafStore. The base owns the store, the declared schema (path -> LeafType), and
 * the single leaf-creation primitive (commitStamped). Each subclass contributes
 * only its essential divergence:
 *   - the leaves it declares (declareLeaves), with each leaf's type decided at
 *     that one declaration site;
 *   - any write side / simulator / atomic-container / subscription-mode policy.
 *
 * A leaf's LeafType is decided in ONE place — its DeclaredLeaf entry — and bound
 * to the leaf at creation by commitStamped (seed, gNMI Set, and the simulator
 * tick all route through it). writable() and the Get type filter both read the
 * same schema_, so there is no path-string heuristic and no drift.
 *
 * See docs/store-backed-provider-template.md and docs/leaf-type-and-schema-model.md.
 */

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "data_provider.hpp"
#include "leaf_store.hpp"

class StoreBackedProvider : public IDataProvider {
public:
    void fill(RepeatedPtrField<Update>* list,
              const std::string& xpath) const override {
        store_.collect(xpath, list);
    }

    Snapshot snapshot(const std::string& xpath) const override {
        return store_.snapshot(xpath);
    }

    // config true ⟺ writable; both answers come from the one declared schema, so
    // a path the server never declared is not writable (a Set to it is refused).
    bool writable(const std::string& xpath) const override {
        return schemaType(xpath) == LeafType::Config;
    }

protected:
    // One declared leaf, as the base consumes it: a path, the type decided at the
    // declaration site, and an optional initial value. A nullopt value means the
    // leaf is declared (writable / typed) but NOT seeded — the schema outlives any
    // value, so a declared-but-unset config leaf is valid and a Set creates it.
    struct DeclaredLeaf {
        std::string                     path;
        LeafType                        type;
        std::optional<gnmi::TypedValue> value;
        int64_t                         collectedNs;
    };

    // The single Template-Method hook: a subclass produces its leaves from its own
    // richer table (the sensor's walk metadata, config's defaults) as this
    // normalised view. initLeaves() consumes it to build schema_ and seed.
    virtual std::vector<DeclaredLeaf> declareLeaves() const = 0;

    // Build schema_ from declareLeaves() and seed the leaves that carry a value.
    // A subclass calls this from its constructor body — NOT the base ctor, which
    // could not dispatch to the derived declareLeaves(). The sensor seeds this way
    // before starting its simulator, avoiding a cold-start NOT_FOUND race.
    void initLeaves() {
        WriteBatch seed;
        for (const auto& d : declareLeaves()) {
            schema_[stripPathQuotes(d.path)] = d.type;
            if (d.value) seed.set(d.path, *d.value, d.collectedNs);
        }
        commitStamped(seed);
    }

    // The single leaf-creation entry point: stamp each Set op's type from schema_,
    // then commit under the store's one lock. Every writer — seed, gNMI Set, the
    // simulator tick — creates leaves through here, so a leaf's type is always the
    // schema's, never a default. Deterministic, so a value-update re-stamps the
    // same type — no create-vs-update branch needed.
    void commitStamped(const WriteBatch& batch) {
        WriteBatch stamped;
        for (auto op : batch.ops()) {
            if (op.kind == WriteOp::Kind::Set) op.type = schemaType(op.xpath);
            stamped.add(op);
        }
        store_.commit(stamped);
    }

    // Schema type of a path, by exact lookup in the declared schema. A path the
    // provider never declared is State: owned (the registry routed it here) but
    // not config — so not writable.
    LeafType schemaType(const std::string& xpath) const {
        auto it = schema_.find(stripPathQuotes(xpath));
        return it == schema_.end() ? LeafType::State : it->second;
    }

    LeafStore store_;
    // path -> LeafType, built once from declareLeaves(). Stable, independent of
    // current data: a leaf may be declared here yet absent from the store.
    std::map<std::string, LeafType> schema_;
};
