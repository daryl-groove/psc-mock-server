/*
 * PscPowerSensorProvider — IDataProvider implementation for PSC power sensors.
 *
 * Handles paths under:
 *   /components/component[name=PSC-x]/state/temperature/...
 *   /components/component[name=PSC-x]/power-supply/state/...
 *
 * Mock: generates slowly-drifting values around ORv3 nominal operating points.
 * Real: replace readXxx() bodies with hardware register reads.
 *
 * Modeled after impl/gnmi-grpc/src/gnmi_collector.cpp:StatConnector.
 */

#pragma once

#include "data_provider.hpp"

#include <string>
#include <chrono>
#include <random>

// Simulated PSC unit names — match gNMI path component[name=<unit>]
inline const std::vector<std::string> PSC_UNITS = { "PSC-0", "PSC-1" };

class PscPowerSensorProvider final : public IDataProvider {
public:
    PscPowerSensorProvider();
    ~PscPowerSensorProvider() override = default;

    bool Handles(const std::string& xpath) const override;

    // Populates Update list with current mock sensor readings for xpath.
    void Fill(RepeatedPtrField<Update>* list,
              const std::string& xpath) override;

private:
    // Mock sensor reads — replace with hardware access for real hardware
    double readTemperature(const std::string& unit);    // celsius
    double readInputVoltage(const std::string& unit);   // volts
    double readInputCurrent(const std::string& unit);   // amps
    double readOutputVoltage(const std::string& unit);  // volts
    double readOutputCurrent(const std::string& unit);  // amps
    double readOutputPower(const std::string& unit);    // watts
    double readCapacity(const std::string& unit);       // watts (static)

    // Adds ±noise_pct% random drift to simulate sensor variation
    double withNoise(double base, double noise_pct);

    std::mt19937 rng_;
};
