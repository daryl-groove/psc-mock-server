#include "system_config_provider.hpp"

#include <string>

namespace {

// Default config leaves (openconfig-system). All string-valued. Seeded at
// construction; thereafter they change only via gNMI Set (Piece B).
struct ConfigLeaf {
    const char* path;
    const char* value;
};

const ConfigLeaf DEFAULTS[] = {
    { "/system/config/hostname",     "psc-mock"       },
    { "/system/config/login-banner", ""               },
    { "/system/config/motd-banner",  "ORv3 PSC mock"  },
};

} // namespace

SystemConfigProvider::SystemConfigProvider() {
    const int64_t now = get_time_nanosec();
    for (const auto& c : DEFAULTS)
        // Wrap in std::string: a bare const char* would bind to set(bool,...).
        store_.set(c.path, std::string(c.value), now);
}

void SystemConfigProvider::fill(RepeatedPtrField<Update>* list,
                                const std::string& xpath) const {
    store_.collect(xpath, list);
}

bool SystemConfigProvider::applyUpdate(const std::string& xpath,
                                       const gnmi::TypedValue& val,
                                       int64_t ts) {
    store_.set(xpath, val, ts);
    return true;
}

bool SystemConfigProvider::applyDelete(const std::string& xpath) {
    store_.remove(xpath);
    return true;
}
