/*
 * IDataProvider — abstract interface for a single data domain.
 *
 * Implement one subclass per data category:
 *   PscPowerSensorProvider  →  /components/.../power-supply/...
 *   PlatformInfoProvider    →  /components/.../state/...        (future)
 *   AlarmProvider           →  /system/alarms/...               (future)
 *
 * DataProviderRegistry holds all registered providers and dispatches
 * Fill() calls. gNMI RPC handlers (get.cpp, subscribe.cpp) only
 * interact with the registry — they are unaware of specific providers.
 */

#pragma once

#include <memory>
#include <vector>
#include <string>

#include <gnmi.grpc.pb.h>  // transitively includes repeated_field.h
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

    // Return true if this provider has data for the given xpath.
    // Used by DataProviderRegistry to route Fill() calls.
    virtual bool Handles(const std::string& xpath) const = 0;

    // Populate gNMI Update list for the given xpath.
    // Appends entries to list — does NOT clear it.
    virtual void Fill(RepeatedPtrField<Update>* list,
                      const std::string& xpath) = 0;

    // Return the preferred subscription mode for this xpath under TARGET_DEFINED.
    // Default SAMPLE suits continuous sensor data; override to ON_CHANGE for
    // event-driven leaves (alarms, state transitions).
    virtual gnmi::SubscriptionMode PreferredMode(const std::string&) const {
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

    void Register(std::unique_ptr<IDataProvider> provider) {
        providers_.push_back(std::move(provider));
    }

    // Fan-out: calls Fill() on every provider whose Handles() returns true.
    // Multiple providers may contribute updates for the same xpath.
    void Fill(RepeatedPtrField<Update>* list,
              const std::string& xpath) const {
        for (auto& p : providers_) {
            if (p->Handles(xpath))
                p->Fill(list, xpath);
        }
    }

    // Returns the first matching provider's preferred subscription mode.
    // Used by subscribe.cpp to resolve TARGET_DEFINED on a per-leaf basis.
    gnmi::SubscriptionMode PreferredMode(const std::string& xpath) const {
        for (auto& p : providers_) {
            if (p->Handles(xpath))
                return p->PreferredMode(xpath);
        }
        return gnmi::SAMPLE;
    }

private:
    std::vector<std::unique_ptr<IDataProvider>> providers_;
};
