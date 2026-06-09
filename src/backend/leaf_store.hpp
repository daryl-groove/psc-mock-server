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

using google::protobuf::RepeatedPtrField;
using gnmi::Update;

class LeafStore {
public:
    // A stored leaf: its value plus the time it was collected from the source.
    // collectedNs feeds Notification.timestamp directly (spec §2.1, §3.5.2.3);
    // emission time would misreport collection time once a writer fills the store.
    struct Leaf {
        gnmi::TypedValue val;
        int64_t          collectedNs;
    };

    // Path → Leaf, scoped to a query in snapshot(). Ordered for deterministic
    // iteration (stable test output, stable diff order).
    using Snapshot = std::map<std::string, Leaf>;

    // Result of comparing two snapshots. ON_CHANGE emits `updated` as Updates and
    // `removed` as Notification.delete paths (spec §3.5.2.3).
    struct Diff {
        std::vector<std::pair<std::string, Leaf>> updated;  // added or value-changed
        std::vector<std::string>                  removed;  // in prev, gone in cur
    };

    // ---- write side: background writer / hardware / tests ----
    void set(const std::string& xpath, double value,             int64_t collectedNs);
    void set(const std::string& xpath, const std::string& value, int64_t collectedNs);
    void set(const std::string& xpath, bool value,               int64_t collectedNs);
    void set(const std::string& xpath, int64_t value,            int64_t collectedNs);
    void set(const std::string& xpath, uint64_t value,           int64_t collectedNs);
    void remove(const std::string& xpath);

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
    void setValue(const std::string& xpath, gnmi::TypedValue val,
                  int64_t collectedNs);

    mutable std::shared_mutex   mu_;
    std::map<std::string, Leaf> leaves_;
};
