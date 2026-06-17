#include "backend/system_config_provider.hpp"

#include <string>

#include "backend/backend.hpp"
#include "backend/gnmi_value.hpp"

namespace gnmid {

namespace {

// Flat scalar config leaves (openconfig-system). All string-valued, per-leaf,
// NON-atomic. Seeded at construction; thereafter they change only via gNMI Set.
struct ConfigLeaf {
    const char* path;
    const char* value;
};

const ConfigLeaf DEFAULTS[] = {
    { "/system/config/hostname",     "psc-mock"      },
    { "/system/config/login-banner", ""              },
    { "/system/config/motd-banner",  "ORv3 PSC mock" },
};

// A config leaf the schema declares but does NOT seed: writable + typed Config,
// yet absent until a client Sets it (the schema outlives any value).
const char* TIMEZONE_NAME = "/system/config/timezone-name";

// An NTP server record: address (key), port, version, iburst, association-type
// form one logical atomic record delivered together.
const std::string NTP = "/system/ntp/servers/server[address=10.0.0.1]/config";

}  // namespace

SystemConfigProvider::SystemConfigProvider(Backend& be) : Provider(be) {
    using core::LeafType;

    for (const auto& c : DEFAULTS)
        be_.declareLeaf(c.path, LeafType::Config, typedValue(std::string(c.value)));

    be_.declareLeaf(TIMEZONE_NAME, LeafType::Config);   // declared, unset

    // The NTP record — an atomic group declared BEFORE its leaves so they
    // auto-assign to it (D3). Mixed-value leaves, all Config.
    be_.declareGroup(NTP, /*atomic=*/true);
    be_.declareLeaf(NTP + "/address",          LeafType::Config, typedValue(std::string("10.0.0.1")));
    be_.declareLeaf(NTP + "/port",             LeafType::Config, typedValue(uint64_t{123}));
    be_.declareLeaf(NTP + "/version",          LeafType::Config, typedValue(uint64_t{4}));
    be_.declareLeaf(NTP + "/iburst",           LeafType::Config, typedValue(true));
    be_.declareLeaf(NTP + "/association-type", LeafType::Config, typedValue(std::string("SERVER")));
}

}  // namespace gnmid
