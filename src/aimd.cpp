#include "tierlink/rate_controller.hpp"

#include <algorithm>

namespace tierlink
{

Aimd::Aimd(const AimdConfig & config) : config_(config), bps_(config.start_bps)
{
}

Decision Aimd::update(Nanos now, const Feedback & feedback)
{
  const Nanos delay = std::max(feedback.queue_delay, feedback.stall);
  if (delay > config_.congested_above) {
    clear_since_.reset();
    if (!last_cut_ || now - *last_cut_ >= config_.hold) {
      last_cut_ = now;
      const auto cut = static_cast<std::uint64_t>(static_cast<double>(bps_) * config_.factor);
      bps_ = std::max(cut, config_.min_bps);
    }
  } else if (delay < config_.clear_below) {
    if (!clear_since_) {
      clear_since_ = now;
    }
    if (now - *clear_since_ >= config_.probe) {
      clear_since_ = now;
      bps_ = std::min(bps_ + config_.step_bps, config_.max_bps);
    }
  } else {
    clear_since_.reset();
  }
  return Decision{bps_, std::nullopt};
}

}  // namespace tierlink
