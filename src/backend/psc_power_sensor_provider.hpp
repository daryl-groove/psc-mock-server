/*
 * PscPowerSensorProvider — mock PSC power-sensor source.
 *
 * Owns /components/component[name=PSC-x]/... sensor leaves (all Operational). It
 * declares them into the shared registry (through the Backend) at construction,
 * then runs a background jthread that drifts the values on a quantized random
 * walk and pushes them via the Backend's registry — the push-bridge model
 * (device-modelling-conventions §8.3). Real hardware: replace the simulator with
 * register reads that push through the same ValueWriter.
 */

#pragma once

#include <string>
#include <thread>
#include <vector>

#include "backend/provider.hpp"
#include "leaf_id.hpp"

namespace gnmid {

class PscPowerSensorProvider final : public Provider {
public:
    explicit PscPowerSensorProvider(Backend& be);
    ~PscPowerSensorProvider() override = default;   // sim_ jthread auto-stops + joins

    std::string domainPrefix() const override { return "/components/component"; }
    void        start() override;                   // launches the simulator

private:
    // Per-leaf walk state: the leaf's id (for ValueWriter), current value, grid
    // STEP (0 = static), and the lo/hi bounds (nominal ± maxDev).
    struct Walk {
        core::LeafId id;
        double       value;
        double       step;
        double       lo;
        double       hi;
    };

    std::vector<Walk> walks_;
    std::jthread      sim_;   // started by start(), after leaves are declared
};

}  // namespace gnmid
