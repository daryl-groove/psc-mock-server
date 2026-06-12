/*
 * PscPowerSensorProvider — IDataProvider implementation for PSC power sensors.
 *
 * handles paths under:
 *   /components/component[name=PSC-x]/state/temperature/...
 *   /components/component[name=PSC-x]/power-supply/state/...
 *
 * A StoreBackedProvider: the base owns the store and serves reads; this subclass
 * only declares its (all-Operational) sensor leaves and runs a background jthread
 * that drifts the values on a quantized random walk. Real hardware: replace the
 * simulator with register reads that commit through the same path.
 *
 * Modeled after impl/gnmi-grpc/src/gnmi_collector.cpp:StatConnector.
 */

#pragma once

#include "store_backed_provider.hpp"

#include <thread>
#include <vector>

class PscPowerSensorProvider final : public StoreBackedProvider {
public:
    PscPowerSensorProvider();
    ~PscPowerSensorProvider() override = default;  // sim_ jthread auto-stops + joins

    // All PSC sensor leaves are continuous measured values — SAMPLE (the default)
    // is correct, so nothing else is overridden.

protected:
    // Sensor leaves, all Operational, starting at their nominal walk values.
    std::vector<DeclaredLeaf> declareLeaves() const override;

private:
    std::jthread sim_;   // declared last: started only after store_ is seeded
};
