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

// An NTP server record is an atomic container: address (key), port, version,
// iburst, association-type form one logical record delivered together.
const char* NTP_CONFIG =
    "/system/ntp/servers/server[address=10.0.0.1]/config";

// A `config true` path is writable. We own a broad /system prefix but only model
// config containers, so gate writes on a /config segment: a write to a
// hypothetical read-only /system/.../state leaf is refused (INVALID_ARGUMENT).
bool isConfigPath(const std::string& p) {
    if (p.find("/config/") != std::string::npos) return true;
    static const std::string tail = "/config";
    return p.size() >= tail.size() &&
           p.compare(p.size() - tail.size(), tail.size(), tail) == 0;
}

} // namespace

SystemConfigProvider::SystemConfigProvider() {
    const int64_t now = get_time_nanosec();
    WriteBatch seed;
    for (const auto& c : DEFAULTS)
        // Wrap in std::string: a bare const char* would bind to set(bool,...).
        seed.set(c.path, std::string(c.value), now);

    // Seed one NTP server record — mixed-type leaves under an atomic container.
    // Seeding the whole record in one commit keeps it coherent from the start.
    const std::string ntp = NTP_CONFIG;
    seed.set(ntp + "/address",          std::string("10.0.0.1"), now);
    seed.set(ntp + "/port",             uint64_t{123},           now);
    seed.set(ntp + "/version",          uint64_t{4},             now);
    seed.set(ntp + "/iburst",           true,                    now);
    seed.set(ntp + "/association-type", std::string("SERVER"),   now);
    store_.commit(seed);
}

void SystemConfigProvider::fill(RepeatedPtrField<Update>* list,
                                const std::string& xpath) const {
    store_.collect(xpath, list);
}

bool SystemConfigProvider::writable(const std::string& xpath) const {
    return isConfigPath(stripPathQuotes(xpath));
}

bool SystemConfigProvider::applyBatch(const WriteBatch& batch) {
    // The registry has already routed these ops to us and Set validated each path
    // as writable; apply the whole transaction under the store's single lock.
    store_.commit(batch);
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
