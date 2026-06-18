#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "canonical_path.hpp"
#include "gnmi.pb.h"
#include "leaf_id.hpp"

namespace gnmid::core {

// One changed/added leaf, captured at COMMIT (R1 — enriched payload). Both handles
// are shared and refcount-safe: the path (D16 / L=B) and the value (D17, COW) are
// zero-copy and outlive the writer scope, so the consumer never re-locks the core
// and nothing dangles after dispatch — even if the leaf is detached meanwhile.
struct LeafChange {
    LeafId                                  id;
    std::shared_ptr<const CanonicalPath>    path;                 // L=B shared handle
    uint64_t                                changeSeq   = 0;      // D14 change token
    int64_t                                 collectedNs = 0;      // wire-timestamp source (D14)
    std::shared_ptr<const gnmi::TypedValue> value;                // D17 COW handle; nullptr = unset
};

// One unified change event across the core→protocol seam (P3 Fork 2, R2). A
// value-only commit leaves `added`/`removedPrefixes` empty; a structural add leaves
// `changed` empty; etc. Mapping of the dispatch sources:
//   - ValueWriter commit            -> changed
//   - registerLeaf / attachSubtree  -> added
//   - detachSubtree / unregisterLeaf-> removedPrefixes  (branch-level, §3.5.2.3)
// `unregisterGroup` deliberately emits NOTHING (D6 carve-out): ungroup != delete.
struct ChangeBatch {
    std::vector<LeafChange>    changed;
    std::vector<LeafChange>    added;
    std::vector<CanonicalPath> removedPrefixes;

    bool empty() const noexcept {
        return changed.empty() && added.empty() && removedPrefixes.empty();
    }
};

// Core→protocol push seam (P1). The registry holds an OPTIONAL ILeafSink*; on a
// commit / structural op it RELEASES the exclusive lock and THEN dispatches a
// shared_ptr<const ChangeBatch>, so the sink never runs under the write lock and
// fan-out to many subscriptions (P2) is a refcount bump, not a copy.
//
// onChange is `noexcept` BY CONTRACT: it is invoked from the ValueWriter destructor
// (and from a post-unlock structural dispatch), where an escaping exception would
// std::terminate. Its real job (P2) is "enqueue the batch + wake a sender", which
// does not throw; the registry additionally guards the call defensively.
class ILeafSink {
public:
    virtual ~ILeafSink() = default;
    virtual void onChange(std::shared_ptr<const ChangeBatch> batch) noexcept = 0;
};

}  // namespace gnmid::core
