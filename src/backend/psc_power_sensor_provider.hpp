/*
 * PscPowerSensorProvider — mock PSC power-sensor source, modelled as hot-pluggable
 * PSU slots (M2, device-modelling-conventions §3).
 *
 * Each slot under /components/component[name=PSC-x] carries two PERMANENT markers
 * (state/name, state/empty) registered once and never removed, plus a DYNAMIC
 * sensor subtree (all Operational) that exists only while the PSU is present.
 * setPresent() simulates insert/remove: it attaches/detaches the sensor subtree
 * through the Backend and flips `empty`. A background jthread drifts the present
 * slots' sensor values on a quantized random walk and pushes them via the Backend's
 * registry — the push-bridge model (§8.3). Real hardware: replace the simulator
 * with register reads, and drive setPresent() from the real insert/remove signal.
 */

#pragma once

#include <map>
#include <mutex>
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

    // Hardware-event backdoor (Fork B): simulate a PSU being inserted (present=true)
    // or removed. Insert attaches the sensor subtree (which then drifts) and flips
    // the slot's `empty` marker to false; remove detaches the sensors and flips
    // `empty` back to true. The permanent name/empty markers always remain (M2).
    // Idempotent; safe to call while the simulator runs; unknown unit is a no-op.
    void setPresent(const std::string& unit, bool present);

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

    // One PSU slot: the permanent `empty` marker (flips on insert/remove) plus the
    // current sensor walk state (non-empty iff the PSU is present).
    struct Slot {
        core::LeafId      emptyId;
        bool              present = false;
        std::vector<Walk> walks;
    };

    void setPresentLocked(const std::string& unit, bool present);  // mu_ already held

    std::map<std::string, Slot> slots_;   // by unit name
    std::mutex                  mu_;       // guards slots_ (simulator vs setPresent)
    std::jthread                sim_;      // started by start(), after slots exist
};

}  // namespace gnmid
