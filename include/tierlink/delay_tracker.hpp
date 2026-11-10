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
  // The transport is ordered, so an acknowledgement also covers every earlier item.
  void on_ack(std::uint64_t id, std::int64_t received_remote_ns, Nanos now);
  Feedback feedback(Nanos now) const;

private:
  struct Sent
  {
    std::uint64_t id;
    Nanos at;
    std::uint64_t bytes;
  };
  std::deque<Sent> unacked_;
  std::uint64_t in_flight_ = 0;
  // Receiver clock minus sender clock plus path delay; the unknown offset cancels against the
  // minimum, so the clocks need no synchronization. The window bounds clock drift.
  WindowMin one_way_;
  WindowMin rtt_;
  Nanos queue_delay_{0};
};

}  // namespace tierlink

#endif  // TIERLINK__DELAY_TRACKER_HPP_
