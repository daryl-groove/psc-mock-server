/*
 * PscPowerSensorProvider — IDataProvider implementation for PSC power sensors.
 *
 * handles paths under:
 *   /components/component[name=PSC-x]/state/temperature/...
 *   /components/component[name=PSC-x]/power-supply/state/...
 *
 * fill() reads from a LeafStore; a background jthread drifts the values on a
 * quantized random walk. Real hardware: replace the simulator with register
 * reads that call store_.set().
 *
 * Modeled after impl/gnmi-grpc/src/gnmi_collector.cpp:StatConnector.
 */

#pragma once

#include "data_provider.hpp"
#include "leaf_store.hpp"

#include <thread>

class PscPowerSensorProvider final : public IDataProvider {
public:
    PscPowerSensorProvider();
    ~PscPowerSensorProvider() override = default;  // sim_ jthread auto-stops + joins

    // Reads current mock sensor readings for xpath from the store.
    void fill(RepeatedPtrField<Update>* list,
              const std::string& xpath) const override;

    // All PSC sensor leaves are continuous measured values — SAMPLE is correct.
    gnmi::SubscriptionMode preferredMode(const std::string&) const override {
        return gnmi::SAMPLE;
    }

    // Read model for Subscribe: current value + collection timestamp per leaf.
    // Sensor readings are runtime measurements → Operational, which is Leaf's
    // default type, so no stamping is needed here.
    Snapshot snapshot(const std::string& xpath) const override {
        return store_.snapshot(xpath);
    }

private:
    LeafStore    store_;
    std::jthread sim_;   // declared last: started only after store_ is seeded
};
