#include "tierlink/scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <numeric>

namespace tierlink
{

Scheduler::Scheduler(std::vector<StreamSpec> specs, std::uint64_t budget_bps, Nanos burst)
: specs_(std::move(specs)), order_(specs_.size()), budget_bps_(budget_bps), burst_(burst)
{
  outboxes_.reserve(specs_.size());
  for (const auto & s : specs_) {
    outboxes_.emplace_back(s);
  }
  std::iota(order_.begin(), order_.end(), 0);
  std::stable_sort(order_.begin(), order_.end(), [this](std::size_t a, std::size_t b) {
    return specs_[a].tier < specs_[b].tier;
  });
}

std::optional<std::size_t> Scheduler::id(std::string_view name) const
{
  for (std::size_t i = 0; i < specs_.size(); ++i) {
    if (specs_[i].name == name) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::pair<std::size_t, Item>> Scheduler::next(Nanos now)
{
  using Seconds = std::chrono::duration<double>;
  const auto rate = static_cast<double>(budget_bps_);
  const double elapsed = last_refill_ ? Seconds(now - *last_refill_).count() : 0.0;
  last_refill_ = now;
  tokens_bits_ = std::min(tokens_bits_ + rate * elapsed, rate * Seconds(burst_).count());
  if (tokens_bits_ <= 0.0) {
    return std::nullopt;
  }
  for (const std::size_t id : order_) {
    if (outboxes_[id].empty()) {
      continue;
    }
    std::optional<Item> item = outboxes_[id].pop();
    tokens_bits_ -= static_cast<double>(item->payload.size() * 8);
    return std::make_pair(id, std::move(*item));
  }
  return std::nullopt;
}

std::uint64_t TierPolicy::monitor_allowance(
  const Decision & link, std::uint64_t teleop_bps, const Feedback & feedback) const
{
  if (std::max(feedback.queue_delay, feedback.stall) > monitor_stop_above) {
    return 0;
  }
  const auto share =
    static_cast<std::uint64_t>(static_cast<double>(link.target_bps) * monitor_share);
  const std::uint64_t left = link.target_bps > teleop_bps ? link.target_bps - teleop_bps : 0;
  return std::min(share, left);
}

std::optional<std::uint64_t> Allowance::cap(Nanos now) const
{
  if (latest_ && now - latest_->first <= expires_after_) {
    return latest_->second;
  }
  return std::nullopt;
}

}  // namespace tierlink
