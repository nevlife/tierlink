#include "tierlink/trendline.hpp"

#include <algorithm>
#include <cmath>

namespace tierlink
{

namespace
{

// The values of libwebrtc's TrendlineEstimator and InterArrival.
constexpr std::int64_t kGroupNs = 5'000'000;
constexpr std::size_t kWindow = 20;
constexpr double kSmoothing = 0.9;
constexpr double kGain = 4.0;
constexpr std::size_t kMinDeltas = 60;
constexpr double kUpMs = 0.0087;
constexpr double kDownMs = 0.039;
constexpr double kOveruseMs = 10.0;
constexpr double kMaxAdaptOffsetMs = 15.0;
constexpr double kMaxAdaptStepMs = 100.0;

std::optional<double> slope(const std::deque<std::pair<double, double>> & points)
{
  double sx = 0.0;
  double sy = 0.0;
  for (const auto & [x, y] : points) {
    sx += x;
    sy += y;
  }
  const double mx = sx / static_cast<double>(points.size());
  const double my = sy / static_cast<double>(points.size());
  double num = 0.0;
  double den = 0.0;
  for (const auto & [x, y] : points) {
    num += (x - mx) * (y - my);
    den += (x - mx) * (x - mx);
  }
  if (den == 0.0) {
    return std::nullopt;
  }
  return num / den;
}

}  // namespace

Usage Trendline::on_packet(std::int64_t sent_ns, std::int64_t arrived_ns)
{
  if (!current_) {
    current_ = Group{sent_ns, sent_ns, arrived_ns};
    return usage_;
  }
  if (sent_ns < current_->first_sent) {
    return usage_;
  }
  if (sent_ns - current_->first_sent <= kGroupNs) {
    current_->last_sent = std::max(current_->last_sent, sent_ns);
    current_->last_arrived = std::max(current_->last_arrived, arrived_ns);
    return usage_;
  }
  if (previous_) {
    const double send_delta = static_cast<double>(current_->last_sent - previous_->last_sent) / 1e6;
    const double arrival_delta =
      static_cast<double>(current_->last_arrived - previous_->last_arrived) / 1e6;
    on_delta(
      arrival_delta - send_delta, send_delta, static_cast<double>(current_->last_arrived) / 1e6);
  }
  previous_ = current_;
  current_ = Group{sent_ns, sent_ns, arrived_ns};
  return usage_;
}

void Trendline::on_delta(double delta_ms, double send_delta_ms, double arrived_ms)
{
  if (!first_arrived_ms_) {
    first_arrived_ms_ = arrived_ms;
  }
  deltas_ = std::min<std::size_t>(deltas_ + 1, 1000);
  accumulated_ms_ += delta_ms;
  smoothed_ms_ = kSmoothing * smoothed_ms_ + (1.0 - kSmoothing) * accumulated_ms_;
  history_.emplace_back(arrived_ms - *first_arrived_ms_, smoothed_ms_);
  if (history_.size() > kWindow) {
    history_.pop_front();
  }
  if (history_.size() == kWindow) {
    trend_ = slope(history_).value_or(trend_);
  }
  detect(trend_, send_delta_ms, arrived_ms);
}

void Trendline::detect(double trend, double send_delta_ms, double now_ms)
{
  if (deltas_ < 2) {
    usage_ = Usage::Normal;
    return;
  }
  const double modified = static_cast<double>(std::min(deltas_, kMinDeltas)) * trend * kGain;
  if (modified > threshold_ms_) {
    over_ms_ = over_ms_ < 0.0 ? send_delta_ms / 2.0 : over_ms_ + send_delta_ms;
    ++over_count_;
    // Overuse needs the trend to stay up for a while and not be falling already.
    if (over_ms_ > kOveruseMs && over_count_ > 1 && trend >= previous_trend_) {
      over_ms_ = 0.0;
      over_count_ = 0;
      usage_ = Usage::Over;
    }
  } else if (modified < -threshold_ms_) {
    over_ms_ = -1.0;
    over_count_ = 0;
    usage_ = Usage::Under;
  } else {
    over_ms_ = -1.0;
    over_count_ = 0;
    usage_ = Usage::Normal;
  }
  previous_trend_ = trend;
  adapt_threshold(modified, now_ms);
}

void Trendline::adapt_threshold(double modified_trend, double now_ms)
{
  if (!last_adapt_ms_) {
    last_adapt_ms_ = now_ms;
  }
  const double m = std::fabs(modified_trend);
  // A spike far above the threshold (a handover, a burst) must not drag it up.
  if (m > threshold_ms_ + kMaxAdaptOffsetMs) {
    last_adapt_ms_ = now_ms;
    return;
  }
  // The threshold rises slowly towards a lasting trend so that a competing TCP flow does not
  // starve this one, and falls faster.
  const double k = m < threshold_ms_ ? kDownMs : kUpMs;
  const double dt = std::min(now_ms - *last_adapt_ms_, kMaxAdaptStepMs);
  threshold_ms_ = std::clamp(threshold_ms_ + k * (m - threshold_ms_) * dt, 6.0, 600.0);
  last_adapt_ms_ = now_ms;
}

}  // namespace tierlink
