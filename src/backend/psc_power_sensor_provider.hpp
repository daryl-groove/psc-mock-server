/*
 * PscPowerSensorProvider — IDataProvider implementation for PSC power sensors.
 *
 * handles paths under:
 *   /components/component[name=PSC-x]/state/temperature/...
 *   /components/component[name=PSC-x]/power-supply/state/...
 *
 * fill() reads from a LeafStore; a background jthread drifts the values on a
 * quantized random walk. Real hardware: replace simulate() with register reads
 * that call store_.set().
 *
 * Modeled after impl/gnmi-grpc/src/gnmi_collector.cpp:StatConnector.
 */

#pragma once

#include "data_provider.hpp"
#include "leaf_store.hpp"

#include <condition_variable>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

// Simulated PSC unit names — match gNMI path component[name=<unit>]
inline const std::vector<std::string> PSC_UNITS = { "PSC-0", "PSC-1" };

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

private:
    // Background simulator: drifts sensor values on a quantized random walk so
    // that ON_CHANGE has genuine "no change" ticks to suppress. Each tick stamps
    // all leaves with one collection timestamp (keeps bundling honest, §3.5.2.1).
    void simulate(std::stop_token stop);

    LeafStore                   store_;
    std::mt19937                rng_;     // used only by the simulator thread
    std::mutex                  simMu_;
    std::condition_variable_any simCv_;
    std::jthread                sim_;     // declared last: started after store_ seed
};
