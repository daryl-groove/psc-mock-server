#include "backend/psc_power_sensor_provider.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <random>
#include <stop_token>
#include <string>
#include <vector>

#include "backend/backend.hpp"      // brings gnmi.pb.h before utils.h (which needs gnmi::)
#include "backend/gnmi_value.hpp"
#include "canonical_path.hpp"
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

std::string unitBase(const std::string& unit) {
    return "/components/component[name=" + unit + "]";
}

std::string leafPath(const std::string& unit, const char* suffix) {
    return unitBase(unit) + suffix;
}

}  // namespace

PscPowerSensorProvider::PscPowerSensorProvider(Backend& be) : Provider(be) {
    // Each PSU lives in a PERMANENT slot: the name + empty markers are registered
    // once and never removed (M2). The sensor subtree is DYNAMIC — attached when the
    // PSU is present. Units boot present (empty=false + sensors) so a Get/Subscribe
    // before any hot-plug sees the same data a fixed inventory would.
    std::lock_guard lk(mu_);
    for (const auto& unit : PSC_UNITS) {
        const std::string base = unitBase(unit);
        be_.declareLeaf(base + "/state/name", core::LeafType::Operational, typedValue(unit));
        core::LeafId emptyId =
            be_.declareLeaf(base + "/state/empty", core::LeafType::Operational, typedValue(true));
        slots_[unit] = Slot{ emptyId, false, {} };
        setPresentLocked(unit, true);   // boot present
    }
}

void PscPowerSensorProvider::setPresent(const std::string& unit, bool present) {
    std::lock_guard lk(mu_);
    setPresentLocked(unit, present);
}

void PscPowerSensorProvider::setPresentLocked(const std::string& unit, bool present) {
    auto it = slots_.find(unit);
    if (it == slots_.end()) return;        // unknown slot — no-op
    Slot& slot = it->second;
    if (slot.present == present) return;    // idempotent

    const std::string base = unitBase(unit);
    if (present) {
        core::SubtreeSpec spec;
        for (const auto& s : SENSORS)
            spec.leaves.push_back({ leafPath(unit, s.suffix), core::LeafType::Operational,
                                    typedValue(s.nominal) });
        std::map<std::string, core::LeafId> ids = be_.attachSubtree(spec);
        slot.walks.clear();
        for (const auto& s : SENSORS) {
            const std::string p = core::canonicalize(leafPath(unit, s.suffix)).str();
            slot.walks.push_back(Walk{ ids.at(p), s.nominal, s.step,
                                       s.nominal * (1.0 - s.maxDev),
                                       s.nominal * (1.0 + s.maxDev) });
        }
    } else {
        // Detach the device subtree only — the two branches the sensors live under,
        // never the permanent name/empty markers that share /state with temperature.
        be_.detachSubtree(base + "/power-supply");
        be_.detachSubtree(base + "/state/temperature");
        slot.walks.clear();
    }

    core::ValueWriter w = be_.registry().writeValues();   // flip the presence marker
    w.set(slot.emptyId, typedValue(!present), get_time_nanosec());
    slot.present = present;
}

void PscPowerSensorProvider::start() {
    sim_ = std::jthread([this](std::stop_token stop) {
        std::mt19937 rng(std::random_device{}());
        std::bernoulli_distribution stepNow(STEP_PROBABILITY);
        std::bernoulli_distribution stepUp(0.5);

        std::mutex waitMu;
        std::condition_variable_any cv;
        std::unique_lock waitLock(waitMu);
        do {
            const int64_t now = get_time_nanosec();
            // One ValueWriter scope per tick: all present slots' leaves share a
            // collection time and become visible together, so a reader never
            // straddles two ticks. slots_ is read under mu_ (vs setPresent).
            std::lock_guard lk(mu_);
            core::ValueWriter w = be_.registry().writeValues();
            for (auto& [unit, slot] : slots_) {
                if (!slot.present) continue;
                for (auto& wk : slot.walks) {
                    if (wk.step > 0.0 && stepNow(rng)) {
                        wk.value += stepUp(rng) ? wk.step : -wk.step;
                        wk.value = std::clamp(wk.value, wk.lo, wk.hi);
                    }
                    w.set(wk.id, typedValue(wk.value), now);
                }
            }
        } while (!cv.wait_for(waitLock, stop, UPDATE_INTERVAL,
                              [&] { return stop.stop_requested(); }));
    });
}

}  // namespace gnmid
