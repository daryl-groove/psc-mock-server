/*
 * PscPowerSensorProvider — mock PSC power sensor data generator.
 *
 * Units per yang/openconfig-platform-psu.yang:
 *   volts / amps / watts  (NOT mV/mA/mW)
 *
 * Values drift on a quantized random walk around realistic ORv3 PSC operating
 * points: each tick a leaf either holds its value or steps by one grid unit.
 * Holding is what gives ON_CHANGE something to suppress. Replace simulate()
 * with real hardware register reads when targeting actual hardware.
 */

#include "psc_power_sensor_provider.hpp"

#include <algorithm>
#include <chrono>

// One sensor leaf: nominal operating point, grid STEP (0 → static, never moves),
// and MAX_DEV as a fraction of nominal that bounds the walk.
struct SensorDef {
    const char* suffix;
    double      nominal;
    double      step;
    double      maxDev;
};

// Units and bounds per openconfig-platform-psu.yang.
static const SensorDef SENSORS[] = {
    { "/state/temperature/instant",         45.0, 0.5, 0.05 },  // celsius
    { "/power-supply/state/input-voltage",  54.0, 0.1, 0.02 },  // volts (54V bus)
    { "/power-supply/state/input-current",  10.0, 0.1, 0.05 },  // amps
    { "/power-supply/state/output-voltage", 12.0, 0.1, 0.01 },  // volts (12V rail)
    { "/power-supply/state/output-current", 20.0, 0.1, 0.05 },  // amps
    { "/power-supply/state/output-power",  240.0, 1.0, 0.05 },  // watts
    { "/power-supply/state/capacity",      300.0, 0.0, 0.0  },  // watts (static)
};

static constexpr double STEP_PROBABILITY = 0.3;   // chance a leaf moves per tick
static constexpr auto   UPDATE_INTERVAL  = std::chrono::seconds(1);

static std::string leafPath(const std::string& unit, const char* suffix) {
    return "/components/component[name=" + unit + "]" + suffix;
}

PscPowerSensorProvider::PscPowerSensorProvider()
    : rng_(std::random_device{}()) {
    // Seed every leaf synchronously so a Get/Subscribe arriving before the first
    // simulator tick finds data (avoids a cold-start NOT_FOUND race).
    const int64_t now = static_cast<int64_t>(get_time_nanosec());
    for (const auto& unit : PSC_UNITS)
        for (const auto& s : SENSORS)
            store_.set(leafPath(unit, s.suffix), s.nominal, now);

    sim_ = std::jthread([this](std::stop_token stop) { simulate(stop); });
}

void PscPowerSensorProvider::fill(RepeatedPtrField<Update>* list,
                                  const std::string& xpath) const {
    store_.collect(xpath, list);
}

void PscPowerSensorProvider::simulate(std::stop_token stop) {
    // Walk state lives only on this thread → no extra locking. lo/hi bound the
    // walk to nominal ± maxDev.
    struct Walk { std::string path; double value; double step; double lo; double hi; };
    std::vector<Walk> walks;
    for (const auto& unit : PSC_UNITS)
        for (const auto& s : SENSORS)
            walks.push_back(Walk{ leafPath(unit, s.suffix), s.nominal, s.step,
                                  s.nominal * (1.0 - s.maxDev),
                                  s.nominal * (1.0 + s.maxDev) });

    std::bernoulli_distribution stepNow(STEP_PROBABILITY);
    std::bernoulli_distribution stepUp(0.5);

    std::unique_lock lock(simMu_);
    do {
        const int64_t now = static_cast<int64_t>(get_time_nanosec());
        for (auto& w : walks) {
            if (w.step > 0.0 && stepNow(rng_)) {
                w.value += stepUp(rng_) ? w.step : -w.step;
                w.value = std::clamp(w.value, w.lo, w.hi);
            }
            store_.set(w.path, w.value, now);
        }
    } while (!simCv_.wait_for(lock, stop, UPDATE_INTERVAL,
                             [&] { return stop.stop_requested(); }));
}
