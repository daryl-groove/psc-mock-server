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
#include <optional>
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
// Provider write model — the transaction a gNMI Set, or a source driver's tick,
// applies to a store.
//
// A WriteBatch groups every mutation that must land together. commit() applies
// the whole batch under one store lock, so a concurrent reader never observes a
// record half-written — which is what makes an atomic container's *write* side as
// coherent as its telemetry (spec §2.1.1). A single-leaf write is just a one-op
// batch; there is deliberately no lock-per-leaf write path a loop could use to
// tear a multi-leaf record. The batch is store-agnostic: path normalisation
// (quote/key stripping) happens in the store at commit time, not here.
// ---------------------------------------------------------------------------

struct WriteOp {
    enum class Kind { Set, Remove };
    std::string      xpath;
    Kind             kind;
    gnmi::TypedValue val;          // Set only
    int64_t          collectedNs;  // Set only
};

class WriteBatch {
public:
    // Typed builders mirror the gNMI scalar types (see addLeaf): each constructs
    // the TypedValue at the call site, where the value's static type is known.
    // A bare const char* would bind to the bool overload, so string callers must
    // pass std::string explicitly.
    WriteBatch& set(const std::string& xpath, double value, int64_t collectedNs) {
        gnmi::TypedValue v; v.set_double_val(value);
        return set(xpath, v, collectedNs);
    }
    WriteBatch& set(const std::string& xpath, const std::string& value,
                    int64_t collectedNs) {
        gnmi::TypedValue v; v.set_string_val(value);
        return set(xpath, v, collectedNs);
    }
    WriteBatch& set(const std::string& xpath, bool value, int64_t collectedNs) {
        gnmi::TypedValue v; v.set_bool_val(value);
        return set(xpath, v, collectedNs);
    }
    WriteBatch& set(const std::string& xpath, int64_t value, int64_t collectedNs) {
        gnmi::TypedValue v; v.set_int_val(value);
        return set(xpath, v, collectedNs);
    }
    WriteBatch& set(const std::string& xpath, uint64_t value, int64_t collectedNs) {
        gnmi::TypedValue v; v.set_uint_val(value);
        return set(xpath, v, collectedNs);
    }
    // The path the gNMI Set side takes: the wire value's type is not known at the
    // call site, so it is forwarded already built.
    WriteBatch& set(const std::string& xpath, const gnmi::TypedValue& value,
                    int64_t collectedNs) {
        ops_.push_back(WriteOp{xpath, WriteOp::Kind::Set, value, collectedNs});
        return *this;
    }
    WriteBatch& remove(const std::string& xpath) {
        ops_.push_back(WriteOp{xpath, WriteOp::Kind::Remove, {}, 0});
        return *this;
    }
    // Append a pre-built op — lets the registry partition a batch by owning prefix.
    WriteBatch& add(WriteOp op) {
        ops_.push_back(std::move(op));
        return *this;
    }

    const std::vector<WriteOp>& ops() const { return ops_; }
    bool empty() const { return ops_.empty(); }

private:
    std::vector<WriteOp> ops_;
};

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
    // (writable) from the mutation (applyBatch) lets Set validate the whole
    // request before touching any store — validate-then-apply.

    // May this path be written at all? Called only for a matching prefix.
    virtual bool writable(const std::string& /*xpath*/) const { return false; }

    // Apply one write transaction (already partitioned to this provider's
    // prefix by the registry). Returns false if the provider refused (e.g.
    // read-only); the registry surfaces that so Set can map it to a status code.
    // The batch's ops land together under the store's lock — see WriteBatch.
    virtual bool applyBatch(const WriteBatch& /*batch*/) { return false; }

    // ---- atomic containers (optional) ------------------------------------
    // If xpath falls within an atomic container this provider owns, return that
    // container's absolute prefix; else nullopt (the default — most data is
    // per-leaf, eventually-consistent). All leaves sharing an atomic prefix MUST
    // be emitted as ONE atomic Notification carrying the complete state of the
    // prefix at one timestamp (spec §2.1.1 / §3.5.2.5): omitted leaves are
    // implicitly deleted, so a changed container is re-sent in full, never diffed.
    virtual std::optional<std::string> atomicPrefix(const std::string&) const {
        return std::nullopt;
    }
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

    // Atomic-container prefix owning xpath, from the first matching provider, or
    // nullopt. The emit side groups leaves by this so each atomic container
    // becomes one atomic Notification (spec §2.1.1).
    std::optional<std::string> atomicPrefix(const std::string& xpath) const {
        for (const auto& [prefix, provider] : routes_) {
            if (matches(xpath, prefix)) {
                if (auto ap = provider->atomicPrefix(xpath)) return ap;
            }
        }
        return std::nullopt;
    }

    // ---- write fan-out --------------------------------------------------
    // Both mirror fill()/snapshot(): visit every provider whose prefix owns the
    // path, OR-reducing routed/applied. writable() is the validate phase (no
    // mutation); commit() is the apply phase. Set calls writable() for every
    // path first and aborts on any !routed or read-only path, so a Set either
    // fully applies or mutates nothing.

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

    // Apply a write transaction. Partitions the batch by owning prefix and hands
    // each provider only the ops it owns, so every provider applies its slice
    // under its own store lock in one shot. An atomic container lives wholly in
    // one provider, so per-provider atomicity is sufficient — gNMI promises no
    // atomicity across providers.
    WriteResult commit(const WriteBatch& batch) {
        WriteResult res{false, false};
        for (const auto& [prefix, provider] : routes_) {
            WriteBatch slice;
            for (const auto& op : batch.ops())
                if (matches(op.xpath, prefix)) slice.add(op);
            if (slice.empty()) continue;
            res.routed = true;
            if (provider->applyBatch(slice)) res.applied = true;
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
