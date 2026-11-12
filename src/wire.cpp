#include "tierlink/wire.hpp"

#include <utility>
#include <vector>

namespace tierlink
{

namespace
{

void put_u64(std::vector<std::uint8_t> & out, std::uint64_t v)
{
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  }
}

std::uint64_t get_u64(const std::vector<std::uint8_t> & in, std::size_t at)
{
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(in[at + i]) << (8 * i);
  }
  return v;
}

}  // namespace

std::vector<std::uint8_t> Header::encode() const
{
  std::vector<std::uint8_t> out;
  out.reserve(kSize);
  put_u64(out, seq);
  put_u64(out, stream_seq);
  put_u64(out, static_cast<std::uint64_t>(sent_ns));
  out.push_back(keyframe ? 1 : 0);
  return out;
}

std::optional<Header> Header::decode(const std::vector<std::uint8_t> & bytes)
{
  if (bytes.size() != kSize) {
    return std::nullopt;
  }
  return Header{
    get_u64(bytes, 0), get_u64(bytes, 8), static_cast<std::int64_t>(get_u64(bytes, 16)),
    bytes[24] != 0};
}

std::vector<std::uint8_t> encode_ack(std::uint64_t seq, std::int64_t received_ns)
{
  std::vector<std::uint8_t> out;
  out.reserve(16);
  put_u64(out, seq);
  put_u64(out, static_cast<std::uint64_t>(received_ns));
  return out;
}

std::optional<std::pair<std::uint64_t, std::int64_t>> decode_ack(
  const std::vector<std::uint8_t> & bytes)
{
  if (bytes.size() != 16) {
    return std::nullopt;
  }
  return std::make_pair(get_u64(bytes, 0), static_cast<std::int64_t>(get_u64(bytes, 8)));
}

std::vector<std::uint8_t> encode_bps(std::uint64_t bps)
{
  std::vector<std::uint8_t> out;
  put_u64(out, bps);
  return out;
}

std::optional<std::uint64_t> decode_bps(const std::vector<std::uint8_t> & bytes)
{
  if (bytes.size() != 8) {
    return std::nullopt;
  }
  return get_u64(bytes, 0);
}

Arrival GapTracker::on_arrival(std::uint64_t stream_seq, bool keyframe, Nanos now)
{
  const std::uint64_t lost = next_ && stream_seq > *next_ ? stream_seq - *next_ : 0;
  next_ = stream_seq + 1;
  if (keyframe) {
    awaiting_ = false;
    return Arrival{lost, false};
  }
  if (lost > 0 && !awaiting_) {
    awaiting_ = true;
    last_request_.reset();
  }
  const bool request = awaiting_ && (!last_request_ || now - *last_request_ >= repeat_);
  if (request) {
    last_request_ = now;
  }
  return Arrival{lost, request};
}

}  // namespace tierlink
