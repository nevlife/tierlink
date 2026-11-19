#ifndef TIERLINK__TRENDLINE_HPP_
#define TIERLINK__TRENDLINE_HPP_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace tierlink
{

// What the delay gradient says about the bottleneck queue.
enum class Usage : std::uint8_t { Normal, Over, Under };

// GCC's overuse detector (draft-ietf-rmcat-gcc, the trendline filter of libwebrtc). It reacts to
// the queue starting to grow, before the delay itself is large, so a rate drop is seen within a
// few hundred milliseconds instead of after a fixed delay threshold.
class Trendline
{
public:
  // Times in nanoseconds; arrivals are on the receiver's clock, which needs no synchronization
  // because only differences between arrivals are used.
  Usage on_packet(std::int64_t sent_ns, std::int64_t arrived_ns);
  Usage usage() const { return usage_; }

private:
  struct Group
  {
    std::int64_t first_sent;
    std::int64_t last_sent;
    std::int64_t last_arrived;
  };
  void on_delta(double delta_ms, double send_delta_ms, double arrived_ms);
  void detect(double trend, double send_delta_ms, double now_ms);
  void adapt_threshold(double modified_trend, double now_ms);

  std::optional<Group> current_;
  std::optional<Group> previous_;
  std::optional<double> first_arrived_ms_;
  std::size_t deltas_ = 0;
  double accumulated_ms_ = 0.0;
  double smoothed_ms_ = 0.0;
  std::deque<std::pair<double, double>> history_;
  double trend_ = 0.0;
  double previous_trend_ = 0.0;
  double threshold_ms_ = 12.5;
  std::optional<double> last_adapt_ms_;
  double over_ms_ = -1.0;
  int over_count_ = 0;
  Usage usage_ = Usage::Normal;
};

}  // namespace tierlink

#endif  // TIERLINK__TRENDLINE_HPP_
