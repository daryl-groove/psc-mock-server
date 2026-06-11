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

// The provider's config schema, declared here at build time: a path under any of
// these subtrees is `config true` (writable) and carries LeafType::Config. This is
// an explicit server declaration — NOT inferred from the literal segment "config"
// appearing in a path — so path naming stays free. Everything else is State.
const char* CONFIG_SUBTREES[] = {
    "/system/config",
    "/system/ntp",
};

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
    applyStamped(seed);
}

void SystemConfigProvider::fill(RepeatedPtrField<Update>* list,
                                const std::string& xpath) const {
    store_.collect(xpath, list);
}

LeafType SystemConfigProvider::schemaType(const std::string& xpath) const {
    const std::string p = stripPathQuotes(xpath);
    for (const char* root : CONFIG_SUBTREES)
        if (isPathPrefix(root, p)) return LeafType::Config;
    return LeafType::State;   // owned but not config (would be applied config)
}

bool SystemConfigProvider::writable(const std::string& xpath) const {
    return schemaType(xpath) == LeafType::Config;   // config true ⟺ writable
}

void SystemConfigProvider::applyStamped(const WriteBatch& batch) {
    // The single point where a leaf's LeafType is decided: bind each created leaf's
    // type from this provider's schema, then commit under the store's one lock.
    WriteBatch stamped;
    for (auto op : batch.ops()) {
        if (op.kind == WriteOp::Kind::Set) op.type = schemaType(op.xpath);
        stamped.add(op);
    }
    store_.commit(stamped);
}

bool SystemConfigProvider::applyBatch(const WriteBatch& batch) {
    // Registry routed these to us and Set validated each path as writable.
    applyStamped(batch);
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
