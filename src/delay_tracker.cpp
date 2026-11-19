#include "tierlink/delay_tracker.hpp"

#include <algorithm>

namespace tierlink
{

std::int64_t WindowMin::push(Nanos now, std::int64_t value)
{
  while (!samples_.empty() && samples_.back().second >= value) {
    samples_.pop_back();
  }
  samples_.emplace_back(now, value);
  while (now - samples_.front().first > window_) {
    samples_.pop_front();
  }
  return samples_.front().second;
}

std::optional<std::int64_t> WindowMin::min() const
{
  if (samples_.empty()) {
    return std::nullopt;
  }
  return samples_.front().second;
}

void DelayTracker::on_sent(std::uint64_t id, std::uint64_t bytes, Nanos at)
{
  unacked_.push_back(Sent{id, at, bytes});
  in_flight_ += bytes;
  sent_total_ += bytes;
}

void DelayTracker::on_ack(std::uint64_t id, std::int64_t received_remote_ns, Nanos now)
{
  std::optional<Nanos> sent_at;
  while (!unacked_.empty() && unacked_.front().id <= id) {
    in_flight_ -= unacked_.front().bytes;
    if (unacked_.front().id == id) {
      acked_total_ += unacked_.front().bytes;
      sent_at = unacked_.front().at;
    } else {
      lost_total_ += unacked_.front().bytes;
    }
    unacked_.pop_front();
  }
  if (!sent_at) {
    return;
  }
  const std::int64_t one_way = received_remote_ns - sent_at->count();
  const std::int64_t min = one_way_.push(now, one_way);
  queue_delay_ = Nanos(std::max<std::int64_t>(one_way - min, 0));
  rtt_.push(now, (now - *sent_at).count());
  if (trendline_.on_packet(sent_at->count(), received_remote_ns) == Usage::Over) {
    over_seen_ = true;
  }
}

Usage DelayTracker::take_usage()
{
  const Usage u = over_seen_ ? Usage::Over : trendline_.usage();
  over_seen_ = false;
  return u;
}

Feedback DelayTracker::feedback(Nanos now) const
{
  Nanos stall{0};
  if (!unacked_.empty()) {
    const Nanos base(rtt_.min().value_or(0));
    stall = std::max(now - unacked_.front().at - base, Nanos{0});
  }
  return Feedback{
    queue_delay_, stall,       in_flight_, trendline_.usage(), acked_total_, lost_total_,
    Nanos(rtt_.min().value_or(0))};
}

}  // namespace tierlink
