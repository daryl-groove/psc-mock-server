/*
 * LeafStore — thread-safe path → value store backing IDataProvider.
 *
 * Separates the two concerns the old compute-on-the-fly fill() conflated:
 *   - "does this path exist?"   → membership in the store
 *   - "what is its value now?"  → the stored value, updated out-of-band
 *
 * A background writer (hardware poller or simulator) pushes values via set();
 * fill() reads them via collect(). Holding state between reads is what makes
 * ON_CHANGE possible: snapshot() + diff() report what actually changed.
 *
 * The store owns gNMI subtree-match semantics (quote/key normalisation) so that
 * providers do not each reimplement path matching.
 */

#pragma once

#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include <gnmi.grpc.pb.h>
#include <utils/utils.h>

#include "data_provider.hpp"   // Leaf, Snapshot — the shared provider read model

using google::protobuf::RepeatedPtrField;
using gnmi::Update;

class LeafStore {
public:
    // The stored leaf and snapshot types are the provider read model, owned by
    // data_provider.hpp so the Subscribe interface does not depend on LeafStore.
    // A Leaf carries its collection timestamp because Notification.timestamp MUST
    // reflect collection / event time, not emission time (spec §2.1, §3.5.2.3).
    using Leaf     = ::Leaf;
    using Snapshot = ::Snapshot;

    // Result of comparing two snapshots. ON_CHANGE emits `updated` as Updates and
    // `removed` as Notification.delete paths (spec §3.5.2.3).
    struct Diff {
        std::vector<std::pair<std::string, Leaf>> updated;  // added or value-changed
        std::vector<std::string>                  removed;  // in prev, gone in cur
    };

    // ---- write side: the single mutating primitive ----
    // Every writer — gNMI Set, the sensor simulator, config seeding — assembles a
    // WriteBatch and commits it here. commit() applies all ops under one unique
    // lock, so a reader's snapshot() either precedes or follows the whole batch,
    // never a half-applied record. There is intentionally no per-leaf set(): that
    // would reopen a lock-per-write path that could tear a multi-leaf record.
    void commit(const WriteBatch& batch);

    // ---- read side: const, shared-locked ----
    std::optional<Leaf> get(const std::string& xpath) const;

    // Append an Update for every stored leaf selected by queryXpath (subtree
    // match). Returns true if at least one Update was appended.
    bool collect(const std::string& queryXpath,
                 RepeatedPtrField<Update>* list) const;

    // Snapshot of every leaf selected by queryXpath, for ON_CHANGE subscribers.
    Snapshot snapshot(const std::string& queryXpath) const;

    // Pure value comparison — no lock, no state, so it is trivially testable.
    // Timestamps are ignored: only a value change counts as a change.
    static Diff diff(const Snapshot& prev, const Snapshot& cur);

private:
    mutable std::shared_mutex   mu_;
    std::map<std::string, Leaf> leaves_;
};
