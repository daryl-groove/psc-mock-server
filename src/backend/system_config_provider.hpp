/*
 * SystemConfigProvider — system configuration (openconfig-system).
 *
 * Owns the writable /system config domain. Unlike the sensor provider there is NO
 * background driver: values change only when a client issues gNMI Set, which the
 * poll+diff loop turns into ON_CHANGE notifications.
 *
 * Two shapes of config coexist here, which is the point:
 *   /system/config/...                                  flat scalars, per-leaf, NON-atomic.
 *   /system/ntp/servers/server[address=X]/config/...    an ATOMIC record (a group):
 *       the whole NTP server record is delivered as one atomic Notification
 *       (spec §2.1.1), so changing one field re-sends the whole record and an
 *       omitted field is implicitly deleted.
 *
 * Writability/typing live in the Backend's schema plane (a declared Config path
 * stays Set-able even when its value is currently absent).
 */

#pragma once

#include <string>

#include "backend/provider.hpp"

namespace gnmid {

class SystemConfigProvider final : public Provider {
public:
    explicit SystemConfigProvider(Backend& be);

    std::string domainPrefix() const override { return "/system"; }
    // No start() override: config is event-driven (Set), no simulator.
};

}  // namespace gnmid
