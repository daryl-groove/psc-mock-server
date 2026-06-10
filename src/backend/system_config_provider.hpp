/*
 * SystemConfigProvider — system configuration (openconfig-system).
 *
 * Serves /system/config/... (hostname, banners). Unlike the sensor provider,
 * config is event-driven and writable: there is NO background simulator —
 * values change only when a client issues gNMI Set. That makes this the
 * deterministic ON_CHANGE source (Set update -> Update, Set delete -> delete)
 * and the natural, semantically-correct home for the write path (config is
 * `config true`, unlike read-only sensor `state`).
 *
 * Piece A (this file) is read-only: it seeds defaults and serves Get/Subscribe.
 * The write side (applyUpdate/applyDelete + registry write fan-out) lands in
 * Piece B. See docs/onchange-delivery-and-source-binding.md §6.1.
 */

#pragma once

#include "data_provider.hpp"
#include "leaf_store.hpp"

class SystemConfigProvider final : public IDataProvider {
public:
    SystemConfigProvider();                       // seeds default config leaves
    ~SystemConfigProvider() override = default;

    void fill(RepeatedPtrField<Update>* list,
              const std::string& xpath) const override;

    Snapshot snapshot(const std::string& xpath) const override {
        return store_.snapshot(xpath);
    }

    // Config is event-driven, so a TARGET_DEFINED subscription should stream on
    // change rather than sample. This is the first provider to prefer ON_CHANGE.
    gnmi::SubscriptionMode preferredMode(const std::string&) const override {
        return gnmi::ON_CHANGE;
    }

    // ---- write side (Piece B) ----
    // /system/config is `config true`: every leaf under our prefix is writable.
    // The registry only calls these for a matching path, so an unconditional
    // true is correct. Mutating store_ here is what the existing poll+diff loop
    // turns into an ON_CHANGE Update / delete — no new trigger needed.
    bool writable(const std::string&) const override { return true; }
    bool applyUpdate(const std::string& xpath, const gnmi::TypedValue& val,
                     int64_t ts) override;
    bool applyDelete(const std::string& xpath) override;

private:
    LeafStore store_;
};
