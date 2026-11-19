#include "tierlink/link_manager.hpp"

#include <algorithm>
#include <cmath>
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

constexpr std::size_t kReportSize = 1 + 5 * 8 + 1;

}  // namespace

std::vector<std::uint8_t> encode_report(const LinkReport & r)
{
  std::vector<std::uint8_t> out{static_cast<std::uint8_t>(r.tier)};
  put_u64(out, r.sent_bps);
  put_u64(out, r.acked_bps);
  put_u64(out, static_cast<std::uint64_t>(r.queue_delay.count()));
  put_u64(out, static_cast<std::uint64_t>(r.stall.count()));
  put_u64(out, r.lost_bps);
  out.push_back(static_cast<std::uint8_t>(r.usage));
  return out;
}

std::optional<LinkReport> decode_report(const std::vector<std::uint8_t> & b)
{
  if (
    b.size() != kReportSize || b[0] > static_cast<std::uint8_t>(Tier::Monitor) ||
    b[kReportSize - 1] > static_cast<std::uint8_t>(Usage::Under)) {
    return std::nullopt;
  }
  return LinkReport{
    static_cast<Tier>(b[0]),
    get_u64(b, 1),
    get_u64(b, 9),
    Nanos(static_cast<std::int64_t>(get_u64(b, 17))),
    Nanos(static_cast<std::int64_t>(get_u64(b, 25))),
    get_u64(b, 33),
    static_cast<Usage>(b[kReportSize - 1])};
}

AimdLink::AimdLink(const AimdLinkConfig & config) : config_(config), bps_(config.start_bps)
{
}

std::uint64_t AimdLink::update(Nanos now, const LinkSignal & signal)
{
  if (signal.delay > config_.congested_above) {
    clear_since_.reset();
    if (!last_cut_ || now - *last_cut_ >= config_.hold) {
      last_cut_ = now;
      // What the link delivered is the best measure of its capacity while it is queueing.
      const std::uint64_t delivered =
        signal.acked_bps > 0 ? std::min(signal.acked_bps, bps_) : bps_;
      bps_ = std::max(
        static_cast<std::uint64_t>(static_cast<double>(delivered) * config_.factor),
        config_.min_bps);
    }
  } else if (signal.delay < config_.clear_below) {
    if (!clear_since_) {
      clear_since_ = now;
    }
    // Grow only while the senders use most of the rate: an idle link says nothing about capacity.
    if (now - *clear_since_ >= config_.probe && signal.sent_bps * 10 >= bps_ * 8) {
      clear_since_ = now;
      const auto grow = std::max(
        config_.step_bps, static_cast<std::uint64_t>(static_cast<double>(bps_) * config_.increase));
      bps_ = std::min(bps_ + grow, config_.max_bps);
    }
  } else {
    clear_since_.reset();
  }
  return bps_;
}

GccLink::GccLink(const GccLinkConfig & config)
: config_(config), bps_(static_cast<double>(config.start_bps))
{
}

std::uint64_t GccLink::update(Nanos now, const LinkSignal & signal)
{
  const double dt =
    last_update_ ? std::chrono::duration<double>(now - *last_update_).count() : 0.0;
  last_update_ = now;
  const auto acked = static_cast<double>(signal.acked_bps);
  const bool over = signal.usage == Usage::Over || signal.delay > config_.congested_above;
  if (over) {
    if (!last_cut_ || now - *last_cut_ >= config_.hold) {
      last_cut_ = now;
      if (acked > 0.0) {
        // The rate at which overuse starts is the best estimate of the capacity; growth slows
        // near it.
        if (!capacity_bps_) {
          capacity_bps_ = acked;
        } else {
          *capacity_bps_ = 0.95 * *capacity_bps_ + 0.05 * acked;
          capacity_var_ = 0.95 * capacity_var_ + 0.05 * (*capacity_bps_ - acked) *
                                                   (*capacity_bps_ - acked) /
                                                   std::max(*capacity_bps_, 1.0);
        }
        bps_ = std::min(bps_, config_.decrease * acked);
      } else {
        bps_ *= config_.decrease;
      }
    }
  } else if (signal.usage == Usage::Normal && signal.loss < config_.loss_low) {
    if (capacity_bps_) {
      const double std_bps = std::sqrt(capacity_var_ * *capacity_bps_);
      // Far above the old capacity: the link got better, so grow fast again.
      if (acked > *capacity_bps_ + 3.0 * std_bps) {
        capacity_bps_.reset();
      }
    }
    const double grown =
      capacity_bps_ ? bps_ + static_cast<double>(config_.additive_bps_per_s) * dt
                    : bps_ * std::pow(1.0 + config_.increase_per_s, dt);
    // Never far above what was delivered: an app-limited sender says nothing about capacity.
    bps_ = std::max(bps_, std::min(grown, 1.5 * acked + 10'000.0));
  }
  if (
    signal.loss > config_.loss_high &&
    (!last_loss_cut_ || now - *last_loss_cut_ >= config_.hold)) {
    last_loss_cut_ = now;
    bps_ *= 1.0 - 0.5 * signal.loss;
  }
  bps_ = std::clamp(
    bps_, static_cast<double>(config_.min_bps), static_cast<double>(config_.max_bps));
  return static_cast<std::uint64_t>(bps_);
}

LinkManager::LinkManager(const SplitConfig & split, std::unique_ptr<LinkEstimator> estimator)
: split_(split), estimator_(std::move(estimator))
{
}

void LinkManager::on_report(const LinkReport & report, Nanos now)
{
  const auto t = static_cast<std::size_t>(report.tier);
  latest_[t] = std::make_pair(now, report);
  over_[t] = over_[t] || report.usage == Usage::Over;
  if (report.acked_bps > 0) {
    last_acked_[t] = now;
  }
}

Budget LinkManager::update(Nanos now)
{
  const auto teleop = static_cast<std::size_t>(Tier::Teleop);
  const auto monitor = static_cast<std::size_t>(Tier::Monitor);
  LinkSignal signal;
  std::array<bool, 2> active{false, false};
  for (std::size_t t = 0; t < latest_.size(); ++t) {
    if (
      !latest_[t] || now - latest_[t]->first > split_.report_timeout || !last_acked_[t] ||
      now - *last_acked_[t] > split_.acked_within) {
      continue;
    }
    const LinkReport & r = latest_[t]->second;
    active[t] = true;
    signal.delay = std::max({signal.delay, r.queue_delay, r.stall});
    signal.sent_bps += r.sent_bps;
    signal.acked_bps += r.acked_bps;
  }
  const std::size_t lead = active[teleop] ? teleop : monitor;
  if (active[lead]) {
    const LinkReport & r = latest_[lead]->second;
    signal.usage = over_[lead] ? Usage::Over : r.usage;
  }
  if (active[teleop]) {
    const LinkReport & r = latest_[teleop]->second;
    const auto seen = r.acked_bps + r.lost_bps;
    signal.loss = seen > 0 ? static_cast<double>(r.lost_bps) / static_cast<double>(seen) : 0.0;
  }
  over_ = {false, false};
  const std::uint64_t link = estimator_->update(now, signal);

  Budget b{link, {0, 0}};
  if (!active[teleop]) {
    b.tier_bps[teleop] = split_.teleop_min_bps;
    b.tier_bps[monitor] = link;
    return b;
  }
  teleop_sent_.emplace_back(now, latest_[teleop]->second.sent_bps);
  while (now - teleop_sent_.front().first > split_.teleop_usage_window) {
    teleop_sent_.pop_front();
  }
  std::uint64_t peak = 0;
  for (const auto & s : teleop_sent_) {
    peak = std::max(peak, s.second);
  }
  const auto demand = static_cast<std::uint64_t>(static_cast<double>(peak) * split_.teleop_headroom);
  const std::uint64_t room =
    link > split_.monitor_floor_bps ? link - split_.monitor_floor_bps : 0;
  b.tier_bps[teleop] =
    std::clamp(std::min(room, demand), split_.teleop_min_bps, split_.teleop_max_bps);
  b.tier_bps[monitor] = signal.delay > split_.monitor_stop_above || link <= b.tier_bps[teleop]
                          ? split_.monitor_floor_bps
                          : std::max(link - b.tier_bps[teleop], split_.monitor_floor_bps);
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
  // The budget sets the rate; a window from the own controller (Ledbat) still holds.
  if (budget && now - budget->first <= expires_after_) {
    return Decision{budget->second, own.max_in_flight_bytes};
  }
  return own;
}

}  // namespace tierlink
