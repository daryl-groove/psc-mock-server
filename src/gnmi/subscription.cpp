#include "subscription.h"

#include <algorithm>

namespace impl {

using gnmid::core::CanonicalPath;
using gnmid::core::ChangeBatch;
using gnmid::core::selects;

bool changeTouchesQueries(const std::vector<CanonicalPath>& queries,
                          const ChangeBatch& batch) {
  // `selects` (not isUnderPrefix) so a bare key-omitted list query
  // (/components/component) matches its keyed entries — a per-slot insert/remove then
  // wakes a list-level ON_CHANGE stream immediately (P4), the same fan-out the Backend
  // already applies at setup. Conservative: a false positive only costs a re-diff.
  auto underAnyQuery = [&](const CanonicalPath& p) {
    for (const auto& q : queries)
      if (selects(q.str(), p.str())) return true;  // changed leaf p selected by query q
    return false;
  };
  for (const auto& c : batch.changed)
    if (c.path && underAnyQuery(*c.path)) return true;
  for (const auto& c : batch.added)
    if (c.path && underAnyQuery(*c.path)) return true;
  for (const auto& r : batch.removedPrefixes)
    for (const auto& q : queries)
      // leaf deleted under q, or a branch deleted at/above q (bidirectional); the
      // key-omitted fan-out applies both ways.
      if (selects(q.str(), r.str()) || selects(r.str(), q.str())) return true;
  return false;
}

void SubscriptionHub::onChange(std::shared_ptr<const ChangeBatch> batch) noexcept {
  if (!batch) return;
  std::lock_guard<std::mutex> hubLk(mu_);
  for (StreamWaker* w : wakers_)
    if (changeTouchesQueries(w->queries(), *batch)) w->wake();
}

void SubscriptionHub::add(StreamWaker* w) {
  std::lock_guard<std::mutex> lk(mu_);
  wakers_.push_back(w);
}

void SubscriptionHub::remove(StreamWaker* w) noexcept {
  std::lock_guard<std::mutex> lk(mu_);
  wakers_.erase(std::remove(wakers_.begin(), wakers_.end(), w), wakers_.end());
}

StreamWaker::StreamWaker(SubscriptionHub& hub, std::vector<CanonicalPath> queries)
    : hub_(hub), queries_(std::move(queries)) {
  hub_.add(this);  // published only after queries_ is set → the hub's lock-free read is safe
}

StreamWaker::~StreamWaker() {
  // After remove() returns, the hub holds no pointer to this waker (and any in-flight
  // onChange that saw it has finished signalling, since both take the hub lock), so
  // it is safe to destroy mu_/cv_.
  hub_.remove(this);
}

void StreamWaker::wake() noexcept {
  {
    std::lock_guard<std::mutex> lk(mu_);
    pending_ = true;
  }
  cv_.notify_one();
}

}  // namespace impl
