#include "tierlink/link_manager.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace tierlink
{

namespace
{

void put_u64(std::vector<std::uint8_t> & out, std::uint64_t v)
{
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  }
}

std::uint64_t get_u64(const std::vector<std::uint8_t> & b, std::size_t at)
{
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(b[at + i]) << (8 * i);
  }
  return v;
}

constexpr std::size_t kReportSize = 1 + 4 * 8;

}  // namespace

std::vector<std::uint8_t> encode_report(const LinkReport & r)
{
  std::vector<std::uint8_t> out{static_cast<std::uint8_t>(r.tier)};
  put_u64(out, r.sent_bps);
  put_u64(out, r.acked_bps);
  put_u64(out, static_cast<std::uint64_t>(r.queue_delay.count()));
  put_u64(out, static_cast<std::uint64_t>(r.stall.count()));
  return out;
}

std::optional<LinkReport> decode_report(const std::vector<std::uint8_t> & b)
{
  if (b.size() != kReportSize || b[0] > static_cast<std::uint8_t>(Tier::Monitor)) {
    return std::nullopt;
  }
  return LinkReport{
    static_cast<Tier>(b[0]), get_u64(b, 1), get_u64(b, 9),
    Nanos(static_cast<std::int64_t>(get_u64(b, 17))),
    Nanos(static_cast<std::int64_t>(get_u64(b, 25)))};
}

LinkManager::LinkManager(const LinkManagerConfig & config)
: config_(config), link_bps_(config.start_bps)
{
}

void LinkManager::on_report(const LinkReport & report, Nanos now)
{
  const auto t = static_cast<std::size_t>(report.tier);
  latest_[t] = std::make_pair(now, report);
  if (report.acked_bps > 0) {
    last_acked_[t] = now;
  }
}

Budget LinkManager::update(Nanos now)
{
  Nanos delay{0};
  std::uint64_t sent = 0;
  std::uint64_t acked = 0;
  std::array<bool, 2> active{false, false};
  for (std::size_t t = 0; t < latest_.size(); ++t) {
    if (
      !latest_[t] || now - latest_[t]->first > config_.report_timeout || !last_acked_[t] ||
      now - *last_acked_[t] > config_.acked_within) {
      continue;
    }
    const LinkReport & r = latest_[t]->second;
    active[t] = true;
    delay = std::max({delay, r.queue_delay, r.stall});
    sent += r.sent_bps;
    acked += r.acked_bps;
  }

  if (delay > config_.congested_above) {
    clear_since_.reset();
    if (!last_cut_ || now - *last_cut_ >= config_.hold) {
      last_cut_ = now;
      // What the link delivered is the best measure of its capacity while it is queueing.
      const std::uint64_t delivered = acked > 0 ? std::min(acked, link_bps_) : link_bps_;
      link_bps_ = std::max(
        static_cast<std::uint64_t>(static_cast<double>(delivered) * config_.factor),
        config_.min_bps);
    }
  } else if (delay < config_.clear_below) {
    if (!clear_since_) {
      clear_since_ = now;
    }
    // Grow only while the senders use most of the rate: an idle link says nothing about capacity.
    if (now - *clear_since_ >= config_.probe && sent * 10 >= link_bps_ * 8) {
      clear_since_ = now;
      link_bps_ = std::min(link_bps_ + config_.step_bps, config_.max_bps);
    }
  } else {
    clear_since_.reset();
  }

  Budget b{link_bps_, {0, 0}};
  const auto teleop = static_cast<std::size_t>(Tier::Teleop);
  const auto monitor = static_cast<std::size_t>(Tier::Monitor);
  if (!active[teleop]) {
    b.tier_bps[teleop] = config_.teleop_min_bps;
    b.tier_bps[monitor] = link_bps_;
    return b;
  }
  const std::uint64_t room =
    link_bps_ > config_.monitor_floor_bps ? link_bps_ - config_.monitor_floor_bps : 0;
  b.tier_bps[teleop] = std::clamp(room, config_.teleop_min_bps, config_.teleop_max_bps);
  b.tier_bps[monitor] = delay > config_.monitor_stop_above || link_bps_ <= b.tier_bps[teleop]
                          ? config_.monitor_floor_bps
                          : std::max(link_bps_ - b.tier_bps[teleop], config_.monitor_floor_bps);
  return b;
}

void BudgetSlot::set(std::uint64_t bps, Nanos now)
{
  std::lock_guard<std::mutex> lock(mutex_);
  latest_ = std::make_pair(now, bps);
}

std::optional<std::pair<Nanos, std::uint64_t>> BudgetSlot::get() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_;
}

Budgeted::Budgeted(
  std::shared_ptr<const BudgetSlot> slot, std::unique_ptr<RateController> fallback,
  Nanos expires_after)
: slot_(std::move(slot)), fallback_(std::move(fallback)), expires_after_(expires_after)
{
}

Decision Budgeted::update(Nanos now, const Feedback & feedback)
{
  // The fallback keeps seeing feedback, so it starts from a current state when it takes over.
  const Decision own = fallback_->update(now, feedback);
  const auto budget = slot_->get();
  if (budget && now - budget->first <= expires_after_) {
    return Decision{budget->second, std::nullopt};
  }
  return own;
}

}  // namespace tierlink
