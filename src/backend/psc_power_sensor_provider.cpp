/*
 * PscPowerSensorProvider — mock PSC power sensor data generator.
 *
 * Units per yang/openconfig-platform-psu.yang:
 *   volts / amps / watts  (NOT mV/mA/mW)
 *
 * Values drift on a quantized random walk around realistic ORv3 PSC operating
 * points: each tick a leaf either holds its value or steps by one grid unit.
 * Holding is what gives ON_CHANGE something to suppress. Replace the simulator
 * with real hardware register reads when targeting actual hardware.
 *
 * All simulation state (RNG, walk table, sleep primitives) is local to the
 * background thread — the provider itself holds only the store and the jthread.
 */

#include "psc_power_sensor_provider.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <stop_token>
#include <string>
#include <vector>

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

// Units and bounds per openconfig-platform-psu.yang.
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

// Per-leaf walk state. lo/hi bound the value to nominal ± maxDev.
struct Walk {
    std::string path;
    double      value;
    double      step;
    double      lo;
    double      hi;
};

std::string leafPath(const std::string& unit, const char* suffix) {
    return "/components/component[name=" + unit + "]" + suffix;
}

// The full leaf set, each walk starting at its nominal value. Single source of
// truth: used both to seed the store and to drive the simulator.
std::vector<Walk> buildWalks() {
    std::vector<Walk> walks;
    for (const auto& unit : PSC_UNITS)
        for (const auto& s : SENSORS)
            walks.push_back(Walk{ leafPath(unit, s.suffix), s.nominal, s.step,
                                  s.nominal * (1.0 - s.maxDev),
                                  s.nominal * (1.0 + s.maxDev) });
    return walks;
}

// Background driver. Each tick stamps all leaves with one collection timestamp
// (keeps bundling honest, §3.5.2.1). mu/cv exist only to back the interruptible
// wait; they guard no shared state.
void runSimulator(LeafStore& store, std::vector<Walk> walks,
                  std::stop_token stop) {
    std::mt19937 rng(std::random_device{}());
    std::bernoulli_distribution stepNow(STEP_PROBABILITY);
    std::bernoulli_distribution stepUp(0.5);

    std::mutex mu;
    std::condition_variable_any cv;
    std::unique_lock lock(mu);
    do {
        const int64_t now = get_time_nanosec();
        for (auto& w : walks) {
            if (w.step > 0.0 && stepNow(rng)) {
                w.value += stepUp(rng) ? w.step : -w.step;
                w.value = std::clamp(w.value, w.lo, w.hi);
            }
            store.set(w.path, w.value, now);
        }
    } while (!cv.wait_for(lock, stop, UPDATE_INTERVAL,
                          [&] { return stop.stop_requested(); }));
}

} // namespace

PscPowerSensorProvider::PscPowerSensorProvider() {
    std::vector<Walk> walks = buildWalks();

    // Seed synchronously so a Get/Subscribe arriving before the first simulator
    // tick finds data (avoids a cold-start NOT_FOUND race).
    const int64_t now = static_cast<int64_t>(get_time_nanosec());
    for (const auto& w : walks)
        store_.set(w.path, w.value, now);

    sim_ = std::jthread(
        [this, walks = std::move(walks)](std::stop_token stop) mutable {
            runSimulator(store_, std::move(walks), stop);
        });
}

void PscPowerSensorProvider::fill(RepeatedPtrField<Update>* list,
                                  const std::string& xpath) const {
    store_.collect(xpath, list);
}
