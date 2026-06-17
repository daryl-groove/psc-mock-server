/*
 * Backend — the device/schema layer over the core LeafRegistry.
 *
 * The core (gnmid::core) stores only live leaves (D18: stored == present) and is
 * deliberately ignorant of two things the gNMI service still needs, which live
 * here instead:
 *
 *   - Routing / namespace ownership: which paths are "implemented" at all, so a
 *     Get/Subscribe/Set to an unowned path is UNIMPLEMENTED rather than empty
 *     (spec §3.3.4 / §3.5.2.4).
 *   - The writability / schema plane: a declared config path stays writable even
 *     when its value is currently absent (device-modelling-conventions §1). The
 *     core only knows leaves that exist; "this path may be Set" outlives any value.
 *
 * Providers populate the registry through this class. It also keeps the
 * path -> LeafId binding (the map foreseen in §8.4-A) so a Set can write a leaf
 * that already exists, and so the sensor driver can push by id.
 *
 * Single owner of the registry and the providers; non-copyable/non-movable
 * (the registry is non-movable and providers hold a Backend& back-reference).
 */

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "gnmi.pb.h"

#include "leaf_registry.hpp"
#include "backend/provider.hpp"

namespace gnmid {

// One operation in a gNMI Set transaction, in xpath terms (origin-less). The
// Backend canonicalizes and resolves to the core's LeafId/ValueWriter.
struct SetOp {
    enum class Kind { Update, Delete };
    Kind             kind;
    std::string      xpath;        // canonicalized internally
    gnmi::TypedValue value;        // Update only
    int64_t          collectedNs;  // Update only
};

class Backend {
public:
    Backend() = default;

    Backend(const Backend&)            = delete;
    Backend& operator=(const Backend&) = delete;

    // --- provider wiring (main.cpp) ---
    // Records the provider's owned prefix, starts its driver, and takes ownership.
    // The provider has already registered its leaves/groups in its constructor.
    void addProvider(std::unique_ptr<Provider> p);

    // --- provider-facing registration (called from a Provider constructor) ---
    // A group is identified by its prefix (D4); declare it before its leaves so they
    // auto-assign (D3).
    void         declareGroup(const std::string& prefix, bool atomic);
    core::LeafId declareLeaf(const std::string& xpath, core::LeafType type,
                             std::optional<gnmi::TypedValue> value = std::nullopt);

    // Mutable registry access for a provider's driver (value writes only — never
    // structural; structure goes through declare*/commit so the bindings stay in
    // sync).
    core::LeafRegistry& registry() noexcept { return registry_; }

    // --- reads (gNMI Get / Subscribe) ---
    struct View {
        core::LeafSnapshot           leaves;  // SET leaves only (unset never hit the wire)
        std::vector<core::GroupView> groups;  // owning groups (atomic ones drive bundling)
        bool                         routed = false;
    };
    View snapshot(const std::string& xpath) const;

    // Effective stream mode for a TARGET_DEFINED subscription (P5): derived from
    // the schema type of the leaves under xpath — Operational -> SAMPLE,
    // Config/State -> ON_CHANGE. Empty/unowned -> SAMPLE.
    gnmi::SubscriptionMode preferredMode(const std::string& xpath) const;

    // --- writes (gNMI Set) ---
    bool routed(const std::string& xpath) const;    // some provider owns the namespace
    bool writable(const std::string& xpath) const;  // a declared config path (persists across delete)

    // Apply a Set transaction. Deletes unregister the leaf (data-plane absence);
    // updates to an absent declared path re-create it; all value writes land in one
    // ValueWriter scope so a multi-leaf / atomic record is coherent to readers.
    void commit(const std::vector<SetOp>& ops);

private:
    static std::string canon(const std::string& xpath);

    core::LeafRegistry                     registry_;
    std::vector<std::unique_ptr<Provider>> providers_;
    std::vector<core::CanonicalPath>       ownedPrefixes_;   // for routing
    std::map<std::string, core::LeafId>    ids_;             // canonical path -> id binding
    std::set<std::string>                  writableConfig_;  // canonical declared config paths
};

}  // namespace gnmid
