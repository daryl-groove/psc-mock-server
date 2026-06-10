/*
 * IDataProvider — abstract interface for a single data domain.
 *
 * Implement one subclass per data category:
 *   PscPowerSensorProvider  →  /components/.../power-supply/...
 *   PlatformInfoProvider    →  /components/.../state/...        (future)
 *   AlarmProvider           →  /system/alarms/...               (future)
 *
 * DataProviderRegistry holds all registered providers and dispatches
 * fill() calls by prefix. gNMI RPC handlers (get.cpp, subscribe.cpp)
 * only interact with the registry — they are unaware of specific providers.
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gnmi.grpc.pb.h>
#include <utils/utils.h>

using google::protobuf::RepeatedPtrField;
using gnmi::Update;

// ---------------------------------------------------------------------------
// Provider read model — the snapshot a Subscribe handler diffs against.
//
// fill()/collect() produce Updates (path + value) for Get; Subscribe needs the
// per-leaf collection timestamp too, because Notification.timestamp MUST be the
// time the value was collected / the event occurred (spec §3.5.2.3), not the
// emission time. These types carry that timestamp. LeafStore is one producer of
// them; the interface stays independent of LeafStore by owning the types here.
// ---------------------------------------------------------------------------

struct Leaf {
    gnmi::TypedValue val;
    int64_t          collectedNs;
};

// Path → Leaf for a query. Ordered for deterministic diff/iteration order.
using Snapshot = std::map<std::string, Leaf>;

// ---------------------------------------------------------------------------
// addLeaf — shared helpers for all IDataProvider implementations
//
// Each overload sets the path from xpath and the appropriate TypedValue field.
// Covers the gNMI scalar types used in OpenConfig YANG models:
//   double  → double_val   (ieeefloat32, decimal64)
//   string  → string_val   (string, enumeration, identityref)
//   bool    → bool_val
//   int64_t → int_val      (int8..int64)
//   uint64_t→ uint_val     (uint8..uint64)
// ---------------------------------------------------------------------------

inline void addLeaf(RepeatedPtrField<Update>* list,
                    const std::string& xpath, double value) {
    auto* u = list->Add();
    xpath_to_gnmi_path(xpath, u->mutable_path());
    u->mutable_val()->set_double_val(value);
}

inline void addLeaf(RepeatedPtrField<Update>* list,
                    const std::string& xpath, const std::string& value) {
    auto* u = list->Add();
    xpath_to_gnmi_path(xpath, u->mutable_path());
    u->mutable_val()->set_string_val(value);
}

inline void addLeaf(RepeatedPtrField<Update>* list,
                    const std::string& xpath, bool value) {
    auto* u = list->Add();
    xpath_to_gnmi_path(xpath, u->mutable_path());
    u->mutable_val()->set_bool_val(value);
}

inline void addLeaf(RepeatedPtrField<Update>* list,
                    const std::string& xpath, int64_t value) {
    auto* u = list->Add();
    xpath_to_gnmi_path(xpath, u->mutable_path());
    u->mutable_val()->set_int_val(value);
}

inline void addLeaf(RepeatedPtrField<Update>* list,
                    const std::string& xpath, uint64_t value) {
    auto* u = list->Add();
    xpath_to_gnmi_path(xpath, u->mutable_path());
    u->mutable_val()->set_uint_val(value);
}

// ---------------------------------------------------------------------------
// IDataProvider
// ---------------------------------------------------------------------------

class IDataProvider {
public:
    virtual ~IDataProvider() = default;

    // Populate gNMI Update list for the given xpath.
    // Called only when the registered prefix matches — no need to re-check.
    // Appends entries to list — does NOT clear it.
    // const: serving a read never mutates provider state; value updates arrive
    // out-of-band through the provider's LeafStore.
    virtual void fill(RepeatedPtrField<Update>* list,
                      const std::string& xpath) const = 0;

    // Return the preferred subscription mode for this xpath under TARGET_DEFINED.
    // Default SAMPLE suits continuous sensor data; override to ON_CHANGE for
    // event-driven leaves (alarms, state transitions).
    virtual gnmi::SubscriptionMode preferredMode(const std::string&) const {
        return gnmi::SAMPLE;
    }

    // Current value + collection timestamp of every leaf selected by xpath.
    // This is the read model Subscribe diffs for ON_CHANGE and stamps for
    // collection-accurate timestamps (spec §3.5.2.3). A provider that only
    // implements fill() would not stream correctly, so this is required.
    virtual Snapshot snapshot(const std::string& xpath) const = 0;

    // ---- write side (optional) -------------------------------------------
    // Most providers serve read-only `state`; the defaults below refuse every
    // write. A `config true` provider overrides them. Splitting the query
    // (writable) from the mutation (applyUpdate/applyDelete) lets Set validate
    // the whole request before touching any store — validate-then-apply.

    // May this path be written at all? Called only for a matching prefix.
    virtual bool writable(const std::string& /*xpath*/) const { return false; }

    // Apply one Set update / delete. Returns false if the provider refused
    // (e.g. read-only); the registry surfaces that so Set can map it to a
    // status code. ts is the collection/event time to stamp the leaf with.
    virtual bool applyUpdate(const std::string& /*xpath*/,
                             const gnmi::TypedValue& /*val*/,
                             int64_t /*ts*/) { return false; }
    virtual bool applyDelete(const std::string& /*xpath*/) { return false; }
};

// ---------------------------------------------------------------------------
// DataProviderRegistry
// ---------------------------------------------------------------------------

// Two independent signals needed to pick the spec-correct status code
// (§3.3.4 / §3.5.2.4):
//   routed   — some provider's registered prefix owns this path's namespace
//   produced — some provider actually appended a value
// not routed            → UNIMPLEMENTED
// routed but !produced  → NOT_FOUND (Get) / silent, RPC not closed (Subscribe)
// produced              → OK
struct FillResult {
    bool routed;
    bool produced;
};

// Subscribe's snapshot fan-out result. `routed` mirrors FillResult.routed so the
// handler can still answer UNIMPLEMENTED for a path no provider owns; `snap` is
// the merged read model across every matching provider.
struct SnapResult {
    Snapshot snap;
    bool     routed;
};

// Write fan-out result, mirroring FillResult's two signals for status mapping
// (spec §3.4 SetResponse / §3.3.4):
//   routed   — some provider's prefix owns this path (else UNIMPLEMENTED/NOT_FOUND)
//   applied  — a provider accepted the write (routed && !applied = the path is
//              read-only → INVALID_ARGUMENT)
// writable() reuses this struct as a dry run: `applied` there means "would be
// accepted", so Set can validate the whole request before mutating anything.
struct WriteResult {
    bool routed;
    bool applied;
};

class DataProviderRegistry {
public:
    DataProviderRegistry() = default;

    // Move-only (unique_ptr members)
    DataProviderRegistry(DataProviderRegistry&&) = default;
    DataProviderRegistry& operator=(DataProviderRegistry&&) = default;
    DataProviderRegistry(const DataProviderRegistry&) = delete;
    DataProviderRegistry& operator=(const DataProviderRegistry&) = delete;

    // Register provider to handle all paths whose xpath starts with prefix.
    // Ownership transfers to the registry.
    void addProvider(std::string prefix, std::unique_ptr<IDataProvider> provider) {
        routes_.emplace_back(std::move(prefix), provider.get());
        providers_.push_back(std::move(provider));
    }

    // Fan-out: calls fill() on every provider whose registered prefix matches xpath.
    // Reports routing and production separately so callers can distinguish
    // "not implemented" from "exists but no data" (see FillResult).
    FillResult fill(RepeatedPtrField<Update>* list,
                    const std::string& xpath) const {
        const int before = list->size();
        bool routed = false;
        for (const auto& [prefix, provider] : routes_) {
            if (matches(xpath, prefix)) {
                routed = true;
                provider->fill(list, xpath);
            }
        }
        return FillResult{routed, list->size() > before};
    }

    // Fan-out snapshot: merges the read model of every provider whose prefix
    // matches xpath. Mirrors fill() so Subscribe can stamp collection-accurate
    // timestamps and diff for ON_CHANGE (spec §3.5.2.3). `routed` distinguishes
    // "no provider owns this path" (UNIMPLEMENTED) from "owned but empty".
    SnapResult snapshot(const std::string& xpath) const {
        SnapResult res{{}, false};
        for (const auto& [prefix, provider] : routes_) {
            if (matches(xpath, prefix)) {
                res.routed = true;
                Snapshot s = provider->snapshot(xpath);
                res.snap.merge(s);
            }
        }
        return res;
    }

    // Returns the preferred subscription mode from the first matching provider.
    // Used by subscribe.cpp to resolve TARGET_DEFINED on a per-leaf basis.
    gnmi::SubscriptionMode preferredMode(const std::string& xpath) const {
        for (const auto& [prefix, provider] : routes_) {
            if (matches(xpath, prefix))
                return provider->preferredMode(xpath);
        }
        return gnmi::SAMPLE;
    }

    // ---- write fan-out --------------------------------------------------
    // All three mirror fill()/snapshot(): visit every provider whose prefix
    // owns xpath, OR-reducing routed/applied. writable() is the validate phase
    // (no mutation); set()/del() are the apply phase. Set calls writable() for
    // every path first and aborts on any !routed or read-only path, so a Set
    // either fully applies or mutates nothing.

    WriteResult writable(const std::string& xpath) const {
        WriteResult res{false, false};
        for (const auto& [prefix, provider] : routes_) {
            if (matches(xpath, prefix)) {
                res.routed = true;
                if (provider->writable(xpath)) res.applied = true;
            }
        }
        return res;
    }

    WriteResult set(const std::string& xpath, const gnmi::TypedValue& val,
                    int64_t ts) {
        WriteResult res{false, false};
        for (const auto& [prefix, provider] : routes_) {
            if (matches(xpath, prefix)) {
                res.routed = true;
                if (provider->applyUpdate(xpath, val, ts)) res.applied = true;
            }
        }
        return res;
    }

    WriteResult del(const std::string& xpath) {
        WriteResult res{false, false};
        for (const auto& [prefix, provider] : routes_) {
            if (matches(xpath, prefix)) {
                res.routed = true;
                if (provider->applyDelete(xpath)) res.applied = true;
            }
        }
        return res;
    }

private:
    // True if the registered prefix owns this xpath's namespace.
    static bool matches(const std::string& xpath, const std::string& prefix) {
        return isPathPrefix(prefix, stripPathQuotes(xpath));
    }

    std::vector<std::pair<std::string, IDataProvider*>> routes_;
    std::vector<std::unique_ptr<IDataProvider>>         providers_;
};
