/*
 * SystemConfigProvider — system configuration (openconfig-system).
 *
 * Owns the writable /system config domain. Unlike the sensor provider, config
 * is event-driven and writable: there is NO background simulator — values
 * change only when a client issues gNMI Set. That makes this the deterministic
 * ON_CHANGE source (Set update -> Update, Set delete -> delete) and the natural,
 * semantically-correct home for the write path (config is `config true`, unlike
 * read-only sensor `state`).
 *
 * A StoreBackedProvider: the base owns the store, the declared schema, reads, and
 * writability. This subclass adds only what diverges — the config leaves it
 * declares, the write side (applyBatch), atomic NTP containers, and ON_CHANGE.
 *
 * Two shapes of config coexist here, which is the point:
 *   /system/config/...                          flat scalars (hostname, banners)
 *                                               — per-leaf, NON-atomic.
 *   /system/ntp/servers/server[address=X]/config/...
 *                                               an ATOMIC container: the whole
 *                                               NTP server record is delivered
 *                                               as one atomic Notification
 *                                               (spec §2.1.1 — telemetry-atomic
 *                                               style record), so changing one
 *                                               field re-sends the whole record
 *                                               and an omitted field is
 *                                               implicitly deleted.
 *
 * See docs/onchange-delivery-and-source-binding.md §6.1 / §7 (atomic framing).
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "store_backed_provider.hpp"

class SystemConfigProvider final : public StoreBackedProvider {
public:
    SystemConfigProvider();                       // seeds default config leaves
    ~SystemConfigProvider() override = default;

    // Config is event-driven, so a TARGET_DEFINED subscription should stream on
    // change rather than sample. This is the first provider to prefer ON_CHANGE.
    gnmi::SubscriptionMode preferredMode(const std::string&) const override {
        return gnmi::ON_CHANGE;
    }

    // ---- write side ----
    // Set already validated each path as writable (config true). Binding each
    // leaf's type from the schema and mutating store_ is what the existing
    // poll+diff loop turns into an ON_CHANGE Update / delete — no new trigger.
    bool applyBatch(const WriteBatch& batch) override;

    // ---- atomic containers ----
    // NTP server records (/system/ntp/servers/server[address=X]/config) are
    // atomic; the flat /system/config scalars are not. Returns the owning
    // server's `.../config` container prefix, or nullopt.
    std::optional<std::string> atomicPrefix(const std::string& xpath) const override;

protected:
    // The provider's config schema, declared at one site (all LeafType::Config).
    std::vector<DeclaredLeaf> declareLeaves() const override;
};
