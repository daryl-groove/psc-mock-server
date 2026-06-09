/*
 * PscPowerSensorProvider — mock PSC power sensor data generator.
 *
 * Units per yang/openconfig-platform-psu.yang:
 *   volts / amps / watts  (NOT mV/mA/mW)
 *
 * All readXxx() functions produce slowly-drifting values around
 * realistic ORv3 PSC operating points. Replace each function body
 * with a real hardware register read when targeting actual hardware.
 */

#include "psc_power_sensor_provider.hpp"
#include <boost/log/trivial.hpp>

// ORv3 PSC nominal operating values — units per openconfig-platform-psu.yang
static constexpr double NOMINAL_TEMP_C      = 45.0;   // celsius
static constexpr double NOMINAL_INPUT_VOLT  = 54.0;   // volts  (54V DC bus)
static constexpr double NOMINAL_INPUT_CURR  = 10.0;   // amps
static constexpr double NOMINAL_OUTPUT_VOLT = 12.0;   // volts  (12V rail)
static constexpr double NOMINAL_OUTPUT_CURR = 20.0;   // amps
static constexpr double NOMINAL_OUTPUT_PWR  = 240.0;  // watts
static constexpr double CAPACITY            = 300.0;  // watts (rated, static)

PscPowerSensorProvider::PscPowerSensorProvider()
    : rng_(std::chrono::steady_clock::now().time_since_epoch().count()) {}

// ---------------------------------------------------------------------------
// IDataProvider interface
// ---------------------------------------------------------------------------

bool PscPowerSensorProvider::handles(const std::string& xpath) const {
    return xpath.starts_with("/components/component");
}

void PscPowerSensorProvider::fill(RepeatedPtrField<Update>* list,
                                   const std::string& xpath) {
    using Reader = double (PscPowerSensorProvider::*)(const std::string&);
    static const struct { const char* suffix; Reader read; } LEAVES[] = {
        { "/state/temperature/instant",         &PscPowerSensorProvider::readTemperature   },
        { "/power-supply/state/input-voltage",  &PscPowerSensorProvider::readInputVoltage  },
        { "/power-supply/state/input-current",  &PscPowerSensorProvider::readInputCurrent  },
        { "/power-supply/state/output-voltage", &PscPowerSensorProvider::readOutputVoltage },
        { "/power-supply/state/output-current", &PscPowerSensorProvider::readOutputCurrent },
        { "/power-supply/state/output-power",   &PscPowerSensorProvider::readOutputPower   },
        { "/power-supply/state/capacity",       &PscPowerSensorProvider::readCapacity      },
    };

    // gnmi_to_xpath quotes key values (e.g. [name="PSC-0"]); strip quotes so
    // canonical paths ([name=PSC-0]) compare correctly.
    std::string norm;
    norm.reserve(xpath.size());
    for (char c : xpath) if (c != '"') norm += c;

    const bool norm_has_key = (norm.find('[') != std::string::npos);

    std::vector<std::string> units;
    for (const auto& unit : PSC_UNITS)
        if (norm.find(unit) != std::string::npos)
            units.push_back(unit);
    if (units.empty()) units = PSC_UNITS;

    for (const auto& unit : units) {
        const std::string base = "/components/component[name=" + unit + "]";

        // Return true when the leaf's full path starts with norm.
        // Case 1: direct starts_with (handles exact leaf, subtree, keyed path).
        // Case 2: norm has no key filter — strip brackets from full path and retry
        //         (e.g. /components/component matches /components/component[name=PSC-0]/...).
        auto want = [&](const char* suffix) -> bool {
            std::string full = base + suffix;
            if (full.rfind(norm, 0) == 0) return true;
            if (!norm_has_key) {
                std::string keyless;
                bool skip = false;
                for (char c : full) {
                    if (c == '[') { skip = true;  continue; }
                    if (c == ']') { skip = false; continue; }
                    if (!skip) keyless += c;
                }
                if (keyless.rfind(norm, 0) == 0) return true;
            }
            return false;
        };

        for (const auto& leaf : LEAVES)
            if (want(leaf.suffix))
                addLeaf(list, base + leaf.suffix, (this->*leaf.read)(unit));
    }
}

// ---------------------------------------------------------------------------
// Mock sensor reads — replace with hardware access
// ---------------------------------------------------------------------------

double PscPowerSensorProvider::withNoise(double base, double noise_pct) {
    std::uniform_real_distribution<double> dist(-noise_pct, noise_pct);
    return base * (1.0 + dist(rng_));
}

double PscPowerSensorProvider::readTemperature(const std::string&) {
    return withNoise(NOMINAL_TEMP_C, 0.05);
}
double PscPowerSensorProvider::readInputVoltage(const std::string&) {
    return withNoise(NOMINAL_INPUT_VOLT, 0.02);
}
double PscPowerSensorProvider::readInputCurrent(const std::string&) {
    return withNoise(NOMINAL_INPUT_CURR, 0.05);
}
double PscPowerSensorProvider::readOutputVoltage(const std::string&) {
    return withNoise(NOMINAL_OUTPUT_VOLT, 0.01);
}
double PscPowerSensorProvider::readOutputCurrent(const std::string&) {
    return withNoise(NOMINAL_OUTPUT_CURR, 0.05);
}
double PscPowerSensorProvider::readOutputPower(const std::string&) {
    return withNoise(NOMINAL_OUTPUT_PWR, 0.05);
}
double PscPowerSensorProvider::readCapacity(const std::string&) {
    return CAPACITY;
}

