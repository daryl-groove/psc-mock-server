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

#include <memory>
#include <string>
#include <vector>

#include <gnmi.grpc.pb.h>
#include <utils/utils.h>

using google::protobuf::RepeatedPtrField;
using gnmi::Update;

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
    virtual void fill(RepeatedPtrField<Update>* list,
                      const std::string& xpath) = 0;

    // Return the preferred subscription mode for this xpath under TARGET_DEFINED.
    // Default SAMPLE suits continuous sensor data; override to ON_CHANGE for
    // event-driven leaves (alarms, state transitions).
    virtual gnmi::SubscriptionMode preferredMode(const std::string&) const {
        return gnmi::SAMPLE;
    }
};

// ---------------------------------------------------------------------------
// DataProviderRegistry
// ---------------------------------------------------------------------------

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
    // Returns true if at least one Update was appended to list, false otherwise.
    bool fill(RepeatedPtrField<Update>* list,
              const std::string& xpath) const {
        const int before = list->size();
        for (const auto& [prefix, provider] : routes_) {
            if (matches(xpath, prefix))
                provider->fill(list, xpath);
        }
        return list->size() > before;
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

private:
    // True if xpath (after quote-stripping) starts with prefix at a segment
    // boundary — preventing /foobar from matching prefix /foo.
    static bool matches(const std::string& xpath, const std::string& prefix) {
        std::string norm;
        norm.reserve(xpath.size());
        for (char c : xpath) if (c != '"') norm += c;
        if (!norm.starts_with(prefix)) return false;
        if (norm.size() == prefix.size()) return true;
        const char next = norm[prefix.size()];
        return next == '/' || next == '[';
    }

    std::vector<std::pair<std::string, IDataProvider*>> routes_;
    std::vector<std::unique_ptr<IDataProvider>>         providers_;
};
