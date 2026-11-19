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
  static constexpr std::size_t kSize = 30;
  // Per session, increasing in sending order across all streams of the tier.
  std::uint64_t seq;
  // Per stream, one per published item, so the receiver sees what the transport dropped.
  std::uint64_t stream_seq;
  std::int64_t sent_ns;
  bool keyframe;
  // An item larger than the chunk size goes out as `chunks` pieces under one stream_seq.
  std::uint16_t chunk = 0;
  std::uint16_t chunks = 1;
  // chunk >= chunks: parity piece j of data chunks [j * fec_group, (j + 1) * fec_group).
  std::uint8_t fec_group = 0;

  std::vector<std::uint8_t> encode() const;
  static std::optional<Header> decode(const std::vector<std::uint8_t> & bytes);
  bool operator==(const Header & o) const
  {
    return seq == o.seq && stream_seq == o.stream_seq && sent_ns == o.sent_ns &&
           keyframe == o.keyframe && chunk == o.chunk && chunks == o.chunks &&
           fec_group == o.fec_group;
  }
};

std::uint16_t parity_count(std::uint16_t chunks, std::uint8_t fec_group);

// The XOR of the pieces' sizes (2 bytes), then of their bytes padded to the longest: rebuilds any
// one piece of the group from the others.
class Parity
{
public:
  void add(const std::vector<std::uint8_t> & piece);
  bool empty() const { return !added_; }
  std::vector<std::uint8_t> take();

private:
  std::vector<std::uint8_t> bytes_{0, 0};
  bool added_ = false;
};

// The sequence acknowledged and when it arrived, in Unix ns: every receiver of a tier (operators,
// the server, a restarted one) stamps the same clock, so the sender's delay minimum holds across them.
std::vector<std::uint8_t> encode_ack(std::uint64_t seq, std::int64_t received_ns);
std::optional<std::pair<std::uint64_t, std::int64_t>> decode_ack(
  const std::vector<std::uint8_t> & bytes);

std::vector<std::uint8_t> encode_bps(std::uint64_t bps);
std::optional<std::uint64_t> decode_bps(const std::vector<std::uint8_t> & bytes);

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

struct Assembled
{
  std::vector<std::uint8_t> item;
  std::size_t recovered;
};

// Puts one stream's pieces back together, rebuilding one lost piece per FEC group. A newer item
// drops an unfinished older one, and a late piece of an older item is ignored.
class Reassembler
{
public:
  std::optional<Assembled> add(const Header & header, std::vector<std::uint8_t> piece);

private:
  void recover(std::size_t group);

  std::optional<std::uint64_t> newest_;
  std::uint8_t fec_group_ = 0;
  std::vector<std::vector<std::uint8_t>> pieces_;
  std::vector<bool> present_;
  std::vector<std::vector<std::uint8_t>> parity_;
  std::size_t received_ = 0;
  std::size_t recovered_ = 0;
  bool done_ = false;
};

}  // namespace tierlink

#endif  // TIERLINK__WIRE_HPP_
