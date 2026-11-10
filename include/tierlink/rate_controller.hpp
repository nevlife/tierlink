#ifndef TIERLINK__RATE_CONTROLLER_HPP_
#define TIERLINK__RATE_CONTROLLER_HPP_

#include <chrono>
#include <cstdint>
#include <optional>

namespace tierlink
{

// Monotonic time of this host, and durations, in nanoseconds.
using Nanos = std::chrono::nanoseconds;

struct Feedback
{
  // One-way delay above its windowed minimum, at the latest acknowledgement.
  Nanos queue_delay{0};
  // Age of the oldest unacked item beyond the minimum round trip; grows while acks stop.
  Nanos stall{0};
  std::uint64_t in_flight_bytes = 0;
};

struct Decision
{
  std::uint64_t target_bps = 0;
  // Window-based controllers hold items while this many bytes are unacked.
  std::optional<std::uint64_t> max_in_flight_bytes;
};

class RateController
{
public:
  virtual ~RateController() = default;
  // Called on a timer, so a stall is seen even when no acknowledgement arrives.
  virtual Decision update(Nanos now, const Feedback & feedback) = 0;
};

class Fixed : public RateController
{
public:
  explicit Fixed(std::uint64_t bps) : bps_(bps) {}
  Decision update(Nanos, const Feedback &) override { return Decision{bps_, std::nullopt}; }

private:
  std::uint64_t bps_;
};

struct AimdConfig
{
  std::uint64_t start_bps;
  std::uint64_t min_bps;
  std::uint64_t max_bps;
  std::uint64_t step_bps;
  double factor;
  // One cut per hold: the encoder needs time before the queue reflects it.
  Nanos hold;
  // How long the link must stay clear before each increase.
  Nanos probe;
  Nanos congested_above;
  Nanos clear_below;
};

// Multiplicative decrease on congestion, additive increase while clear.
class Aimd : public RateController
{
public:
  explicit Aimd(const AimdConfig & config);
  Decision update(Nanos now, const Feedback & feedback) override;

private:
  AimdConfig config_;
  std::uint64_t bps_;
  std::optional<Nanos> last_cut_;
  std::optional<Nanos> clear_since_;
};

}  // namespace tierlink

#endif  // TIERLINK__RATE_CONTROLLER_HPP_
