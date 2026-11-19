#include "tierlink/rate_controller.hpp"

#include <algorithm>

namespace tierlink
{

Ledbat::Ledbat(const LedbatConfig & config)
: config_(config), window_(static_cast<double>(config.start_window_bytes))
{
}

Decision Ledbat::update(Nanos now, const Feedback & feedback)
{
  const auto newly = static_cast<double>(feedback.acked_bytes - acked_seen_);
  const bool lost = feedback.lost_bytes > lost_seen_;
  acked_seen_ = feedback.acked_bytes;
  lost_seen_ = feedback.lost_bytes;
  const auto segment = static_cast<double>(config_.segment_bytes);
  if (lost || feedback.stall > config_.stall_above) {
    if (!last_halving_ || now - *last_halving_ >= config_.hold) {
      last_halving_ = now;
      window_ /= 2.0;
    }
  } else if (newly > 0.0) {
    const double target = static_cast<double>(config_.target.count());
    const double off = std::clamp(
      (target - static_cast<double>(feedback.queue_delay.count())) / target, -1.0, 1.0);
    window_ += config_.gain * off * newly * segment / window_;
    // An app-limited sender must not build up a window it never tested.
    window_ = std::min(
      window_, static_cast<double>(feedback.in_flight_bytes) + newly + segment);
  }
  window_ = std::clamp(
    window_, static_cast<double>(config_.min_window_bytes),
    static_cast<double>(config_.max_window_bytes));
  return Decision{config_.rate_bps, static_cast<std::uint64_t>(window_)};
}

}  // namespace tierlink
