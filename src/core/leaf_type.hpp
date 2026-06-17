#pragma once

namespace gnmid::core {

// Schema classification of a leaf. Config = YANG config-true (writable, a Set target);
// State and Operational are both config-false (read-only) and differ only as the server's
// DEFAULT stream mode for TARGET_DEFINED subscriptions — State/Config resolve to ON_CHANGE,
// Operational to SAMPLE. It is only a default: a client may explicitly subscribe to any leaf
// in any mode. Resolved per leaf via LeafEntry::effectiveType().
enum class LeafType {
    Config,       // writable config leaf (gNMI Set target)
    State,        // read-only state, monitored via ON_CHANGE by default (e.g. oper-status)
    Operational,  // default — read-only telemetry sampled by default (sensors, counters, etc.)
};

}  // namespace gnmid::core
