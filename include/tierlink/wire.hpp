#ifndef TIERLINK__WIRE_HPP_
#define TIERLINK__WIRE_HPP_

#include "tierlink/rate_controller.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace tierlink
{

// Little-endian. Data carries its header in the Zenoh attachment, so the payload stays exactly
// what the codec produced.
struct Header
{
  static constexpr std::size_t kSize = 25;
  // Per session, increasing in sending order across all streams of the tier.
  std::uint64_t seq;
  // Per stream, one per published item, so the receiver sees what the transport dropped.
  std::uint64_t stream_seq;
  std::int64_t sent_ns;
  bool keyframe;

  std::vector<std::uint8_t> encode() const;
  static std::optional<Header> decode(const std::vector<std::uint8_t> & bytes);
  bool operator==(const Header & o) const
  {
    return seq == o.seq && stream_seq == o.stream_seq && sent_ns == o.sent_ns &&
           keyframe == o.keyframe;
  }
};

// The sequence acknowledged and the receiver's clock when it arrived.
std::vector<std::uint8_t> encode_ack(std::uint64_t seq, std::int64_t received_ns);
std::optional<std::pair<std::uint64_t, std::int64_t>> decode_ack(
  const std::vector<std::uint8_t> & bytes);

struct Arrival
{
  // Items published but never received before this one.
  std::uint64_t lost;
  bool request_keyframe;
};

// Per-stream receive state. A gap means the transport dropped items; for an inter-frame stream
// the picture stays broken until a keyframe, so one is requested, and requested again on later
// arrivals until it comes, in case the request itself is lost.
class GapTracker
{
public:
  explicit GapTracker(Nanos repeat) : repeat_(repeat) {}
  Arrival on_arrival(std::uint64_t stream_seq, bool keyframe, Nanos now);

private:
  Nanos repeat_;
  std::optional<std::uint64_t> next_;
  bool awaiting_ = false;
  std::optional<Nanos> last_request_;
};

}  // namespace tierlink

#endif  // TIERLINK__WIRE_HPP_
