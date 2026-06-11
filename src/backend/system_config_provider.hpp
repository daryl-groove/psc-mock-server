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

#include "data_provider.hpp"
#include "leaf_store.hpp"

class SystemConfigProvider final : public IDataProvider {
public:
    SystemConfigProvider();                       // seeds default config leaves
    ~SystemConfigProvider() override = default;

    void fill(RepeatedPtrField<Update>* list,
              const std::string& xpath) const override;

    Snapshot snapshot(const std::string& xpath) const override {
        // Leaves already carry their schema type (stamped at creation); just serve.
        return store_.snapshot(xpath);
    }

    // Config is event-driven, so a TARGET_DEFINED subscription should stream on
    // change rather than sample. This is the first provider to prefer ON_CHANGE.
    gnmi::SubscriptionMode preferredMode(const std::string&) const override {
        return gnmi::ON_CHANGE;
    }

    // ---- write side ----
    // Only `config true` leaves are writable. We own a broad /system prefix, so
    // restrict writes to config containers (a write to a hypothetical read-only
    // /system/.../state leaf is refused → INVALID_ARGUMENT). Mutating store_ via
    // Set is what the existing poll+diff loop turns into an ON_CHANGE Update /
    // delete — no new trigger needed.
    bool writable(const std::string& xpath) const override;
    bool applyBatch(const WriteBatch& batch) override;

    // ---- atomic containers ----
    // NTP server records (/system/ntp/servers/server[address=X]/config) are
    // atomic; the flat /system/config scalars are not. Returns the owning
    // server's `.../config` container prefix, or nullopt.
    std::optional<std::string> atomicPrefix(const std::string& xpath) const override;

private:
    // Single source of truth for both writability and a leaf's LeafType: the schema
    // type of a path, from this provider's declared config subtrees.
    LeafType schemaType(const std::string& xpath) const;
    // Stamp each created leaf's type from schemaType, then commit the batch.
    void applyStamped(const WriteBatch& batch);

    LeafStore store_;
};
