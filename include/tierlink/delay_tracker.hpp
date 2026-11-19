#ifndef TIERLINK__DELAY_TRACKER_HPP_
#define TIERLINK__DELAY_TRACKER_HPP_

#include "tierlink/rate_controller.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace tierlink
{

// Minimum over a sliding time window.
class WindowMin
{
public:
  explicit WindowMin(Nanos window) : window_(window) {}
  std::int64_t push(Nanos now, std::int64_t value);
  std::optional<std::int64_t> min() const;

private:
  Nanos window_;
  std::deque<std::pair<Nanos, std::int64_t>> samples_;
};

// Turns sent items and the receiver's acknowledgements into Feedback.
class DelayTracker
{
public:
  explicit DelayTracker(Nanos window) : one_way_(window), rtt_(window) {}
  // Ids increase in sending order.
  void on_sent(std::uint64_t id, std::uint64_t bytes, Nanos at);
  // Acknowledgements arrive in sending order, so an earlier item still unacknowledged was lost:
  // over UDP by the network, over TCP in Zenoh's queue.
  void on_ack(std::uint64_t id, std::int64_t received_remote_ns, Nanos now);
  Feedback feedback(Nanos now) const;
  // Over if the gradient showed overuse since the last call, else the current usage. A report
  // every 100 ms would otherwise miss an overuse that cleared in between.
  Usage take_usage();
  std::uint64_t sent_total() const { return sent_total_; }

private:
  struct Sent
  {
    std::uint64_t id;
    Nanos at;
    std::uint64_t bytes;
  };
  std::deque<Sent> unacked_;
  std::uint64_t in_flight_ = 0;
  std::uint64_t sent_total_ = 0;
  std::uint64_t acked_total_ = 0;
  std::uint64_t lost_total_ = 0;
  // Receivers' Unix time (kept in agreement by NTP) minus the sender's steady clock plus path delay;
  // the offset cancels against the minimum, whose window bounds clock drift.
  WindowMin one_way_;
  WindowMin rtt_;
  Nanos queue_delay_{0};
  Trendline trendline_;
  bool over_seen_ = false;
};

}  // namespace tierlink

#endif  // TIERLINK__DELAY_TRACKER_HPP_
