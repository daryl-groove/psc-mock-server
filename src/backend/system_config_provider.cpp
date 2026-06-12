#include "system_config_provider.hpp"

#include <string>

namespace {

// Flat scalar config leaves (openconfig-system). All string-valued, per-leaf,
// NON-atomic. Seeded at construction; thereafter they change only via gNMI Set.
struct ConfigLeaf {
    const char* path;
    const char* value;
};

const ConfigLeaf DEFAULTS[] = {
    { "/system/config/hostname",     "psc-mock"       },
    { "/system/config/login-banner", ""               },
    { "/system/config/motd-banner",  "ORv3 PSC mock"  },
};

// A config leaf the schema declares but does NOT seed: writable and typed Config,
// yet absent until a client Sets it (the schema outlives any value). Demonstrates
// "create a declared-but-unset config leaf"; an UNdeclared path stays non-writable.
const char* TIMEZONE_NAME = "/system/config/timezone-name";

// An NTP server record is an atomic container: address (key), port, version,
// iburst, association-type form one logical record delivered together.
const char* NTP_CONFIG =
    "/system/ntp/servers/server[address=10.0.0.1]/config";

} // namespace

std::vector<StoreBackedProvider::DeclaredLeaf>
SystemConfigProvider::declareLeaves() const {
    const int64_t now = get_time_nanosec();
    std::vector<DeclaredLeaf> decls;

    for (const auto& c : DEFAULTS)
        decls.push_back(DeclaredLeaf{ c.path, LeafType::Config,
                                      typedValue(c.value), now });

    decls.push_back(DeclaredLeaf{ TIMEZONE_NAME, LeafType::Config,
                                  std::nullopt, now });

    // The NTP record — mixed-value leaves under one atomic container, all Config.
    const std::string ntp = NTP_CONFIG;
    decls.push_back(DeclaredLeaf{ ntp + "/address", LeafType::Config,
                                  typedValue("10.0.0.1"), now });
    decls.push_back(DeclaredLeaf{ ntp + "/port", LeafType::Config,
                                  typedValue(uint64_t{123}), now });
    decls.push_back(DeclaredLeaf{ ntp + "/version", LeafType::Config,
                                  typedValue(uint64_t{4}), now });
    decls.push_back(DeclaredLeaf{ ntp + "/iburst", LeafType::Config,
                                  typedValue(true), now });
    decls.push_back(DeclaredLeaf{ ntp + "/association-type", LeafType::Config,
                                  typedValue("SERVER"), now });
    return decls;
}

SystemConfigProvider::SystemConfigProvider() {
    initLeaves();
}

bool SystemConfigProvider::applyBatch(const WriteBatch& batch) {
    // Registry routed these to us and Set validated each path as writable.
    commitStamped(batch);
    return true;
}

std::optional<std::string>
SystemConfigProvider::atomicPrefix(const std::string& xpath) const {
    const std::string p = stripPathQuotes(xpath);
    static const std::string root = "/system/ntp/servers/server[";
    if (p.compare(0, root.size(), root) != 0) return std::nullopt;

    // The atomic boundary is the server entry's `.../config` container.
    static const std::string marker = "/config";
    if (size_t c = p.find(marker + "/"); c != std::string::npos)
        return p.substr(0, c + marker.size());          // ".../config/<leaf>"
    if (p.size() >= marker.size() &&
        p.compare(p.size() - marker.size(), marker.size(), marker) == 0)
        return p;                                        // the container itself
    return std::nullopt;
}
