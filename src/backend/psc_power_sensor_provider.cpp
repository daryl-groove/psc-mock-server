#include "backend/psc_power_sensor_provider.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <stop_token>
#include <string>
#include <vector>

#include "backend/backend.hpp"      // brings gnmi.pb.h before utils.h (which needs gnmi::)
#include "backend/gnmi_value.hpp"
#include <utils/utils.h>            // get_time_nanosec

namespace gnmid {

namespace {

// Simulated PSC unit names — match gNMI path component[name=<unit>].
const std::vector<std::string> PSC_UNITS = { "PSC-0", "PSC-1" };

// One sensor leaf: nominal operating point, grid STEP (0 → static, never moves),
// and maxDev as a fraction of nominal that bounds the walk.
struct SensorDef {
    const char* suffix;
    double      nominal;
    double      step;
    double      maxDev;
};

// Units and bounds per openconfig-platform-psu.yang (volts/amps/watts).
const SensorDef SENSORS[] = {
    { "/state/temperature/instant",         45.0, 0.5, 0.05 },  // celsius
    { "/power-supply/state/input-voltage",  54.0, 0.1, 0.02 },  // volts (54V bus)
    { "/power-supply/state/input-current",  10.0, 0.1, 0.05 },  // amps
    { "/power-supply/state/output-voltage", 12.0, 0.1, 0.01 },  // volts (12V rail)
    { "/power-supply/state/output-current", 20.0, 0.1, 0.05 },  // amps
    { "/power-supply/state/output-power",  240.0, 1.0, 0.05 },  // watts
    { "/power-supply/state/capacity",      300.0, 0.0, 0.0  },  // watts (static)
};

constexpr double STEP_PROBABILITY = 0.3;   // chance a leaf moves per tick
constexpr auto   UPDATE_INTERVAL  = std::chrono::seconds(1);

std::string leafPath(const std::string& unit, const char* suffix) {
    return "/components/component[name=" + unit + "]" + suffix;
}

}  // namespace

PscPowerSensorProvider::PscPowerSensorProvider(Backend& be) : Provider(be) {
    // Declare + seed every sensor leaf synchronously, so a Get/Subscribe arriving
    // before the first simulator tick finds data. declareLeaf returns the id the
    // simulator pushes through.
    for (const auto& unit : PSC_UNITS)
        for (const auto& s : SENSORS) {
            core::LeafId id = be_.declareLeaf(leafPath(unit, s.suffix),
                                              core::LeafType::Operational,
                                              typedValue(s.nominal));
            walks_.push_back(Walk{ id, s.nominal, s.step,
                                   s.nominal * (1.0 - s.maxDev),
                                   s.nominal * (1.0 + s.maxDev) });
        }
}

void PscPowerSensorProvider::start() {
    sim_ = std::jthread([this](std::stop_token stop) {
        std::mt19937 rng(std::random_device{}());
        std::bernoulli_distribution stepNow(STEP_PROBABILITY);
        std::bernoulli_distribution stepUp(0.5);

        std::mutex mu;
        std::condition_variable_any cv;
        std::unique_lock lock(mu);
        do {
            const int64_t now = get_time_nanosec();
            // One ValueWriter scope per tick: all leaves share a collection time
            // and become visible together, so a reader never straddles two ticks.
            core::ValueWriter w = be_.registry().writeValues();
            for (auto& wk : walks_) {
                if (wk.step > 0.0 && stepNow(rng)) {
                    wk.value += stepUp(rng) ? wk.step : -wk.step;
                    wk.value = std::clamp(wk.value, wk.lo, wk.hi);
                }
                w.set(wk.id, typedValue(wk.value), now);
            }
        } while (!cv.wait_for(lock, stop, UPDATE_INTERVAL,
                              [&] { return stop.stop_requested(); }));
    });
}

}  // namespace gnmid
