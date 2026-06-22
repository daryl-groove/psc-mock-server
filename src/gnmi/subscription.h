/*
 * Push-seam protocol endpoint (P1/P2, S2). The core dispatches change events to an
 * ILeafSink after a commit (src/core/leaf_sink.hpp); SubscriptionHub is that sink on
 * the protocol side. On a change it wakes the Subscribe streams a change touches, so
 * their ON_CHANGE loop emits immediately instead of at the next poll tick.
 *
 * S2 scope (decided with user, docs/push-impl-checklist.md): this is the cross-thread
 * wake substrate only. The woken loop still snapshot+diffs (reusing the existing
 * builder); consuming the batch payload + a changeSeq watermark is the S3 refinement
 * (Phase 3). Routing is a linear scan over live streams (backlog "Push layer" records
 * the query-path-trie upgrade and its trigger).
 */

#ifndef _GNMI_SUBSCRIPTION_H
#define _GNMI_SUBSCRIPTION_H

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include "canonical_path.hpp"
#include "leaf_sink.hpp"

namespace impl {

class StreamWaker;

// True if `batch` touches any of `queries` (a stream's ON_CHANGE query subtrees): a
// changed/added leaf selected by a query, or a removed prefix overlapping a query (a
// leaf deleted under it, or a branch deleted at/above it). Matching uses
// `core::selects`, so a bare key-omitted list query (e.g. /components/component) DOES
// match its keyed entries — a per-slot hot-plug wakes a list-level ON_CHANGE stream
// immediately (P4), the same fan-out the Backend applies at setup. Conservative by
// design — a false positive only costs a harmless re-diff (the snapshot+diff on wake
// is the source of truth), so an imprecise match never loses a change, only its
// instantaneity.
bool changeTouchesQueries(const std::vector<gnmid::core::CanonicalPath>& queries,
                          const gnmid::core::ChangeBatch& batch);

// One per server: an ILeafSink wired into the core registry. A core commit /
// structural op (on a writer thread — a Set RPC or a provider's sensor driver) calls
// onChange, which scans the live streams and wakes those a change touches.
class SubscriptionHub : public gnmid::core::ILeafSink {
public:
  void onChange(std::shared_ptr<const gnmid::core::ChangeBatch> batch) noexcept override;

  // Register/deregister a live stream (called by StreamWaker's ctor/dtor). Lock order
  // is ALWAYS hub -> waker: onChange takes the hub lock, then a waker lock to signal;
  // a stream blocked in StreamWaker::waitUntil holds only its own lock and never the
  // hub lock, so the two can't deadlock.
  void add(StreamWaker* w);
  void remove(StreamWaker* w) noexcept;

private:
  std::mutex                mu_;
  std::vector<StreamWaker*> wakers_;  // non-owning; each removes itself in its dtor
};

// Per-Subscribe-stream wake handle. Lives on the Subscribe RPC thread's stack and
// registers with the hub for its lifetime (RAII). The Subscribe loop blocks in
// waitUntil(); the hub (a writer thread) calls wake() when a change touches one of
// this stream's query paths. queries_ is fixed at construction and published to the
// hub only after it is set, so the hub reads it without taking this waker's lock.
class StreamWaker {
public:
  StreamWaker(SubscriptionHub& hub, std::vector<gnmid::core::CanonicalPath> queries);
  ~StreamWaker();
  StreamWaker(const StreamWaker&)            = delete;
  StreamWaker& operator=(const StreamWaker&) = delete;

  const std::vector<gnmid::core::CanonicalPath>& queries() const noexcept { return queries_; }

  // Hub side (writer thread): mark pending and signal the loop.
  void wake() noexcept;

  // Loop side (own thread): block until woken, `deadline` passes, or a wake is already
  // pending; consume the pending flag before returning. No wake is lost — pending_ is
  // set under the same lock the predicate re-checks it under.
  template <class Clock, class Dur>
  void waitUntil(std::chrono::time_point<Clock, Dur> deadline) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_until(lk, deadline, [&] { return pending_; });
    pending_ = false;
  }

private:
  SubscriptionHub&                              hub_;
  const std::vector<gnmid::core::CanonicalPath> queries_;
  std::mutex                                    mu_;
  std::condition_variable                       cv_;
  bool                                          pending_ = false;
};

}  // namespace impl

#endif  // _GNMI_SUBSCRIPTION_H
