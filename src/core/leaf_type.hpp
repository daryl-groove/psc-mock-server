#pragma once

namespace gnmid::core {

// Schema classification of a leaf, mirroring the YANG `config true/false`
// annotation. Resolved per leaf via LeafEntry::effectiveType().
enum class LeafType {
    Config,       // writable config leaf (gNMI Set target)
    State,        // read-only operational state reported by device
    Operational,  // default — read-only telemetry (sensors, counters, etc.)
};

}  // namespace gnmid::core
