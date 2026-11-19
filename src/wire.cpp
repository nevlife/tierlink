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
  for (const std::uint16_t v : {chunk, chunks}) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
  }
  out.push_back(fec_group);
  return out;
}

std::optional<Header> Header::decode(const std::vector<std::uint8_t> & bytes)
{
  if (bytes.size() != kSize) {
    return std::nullopt;
  }
  const auto u16 = [&bytes](std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8));
  };
  const Header h{
    get_u64(bytes, 0),
    get_u64(bytes, 8),
    static_cast<std::int64_t>(get_u64(bytes, 16)),
    bytes[24] != 0,
    u16(25),
    u16(27),
    bytes[29]};
  if (h.chunks == 0 || h.chunk >= h.chunks + parity_count(h.chunks, h.fec_group)) {
    return std::nullopt;
  }
  return h;
}

std::uint16_t parity_count(std::uint16_t chunks, std::uint8_t fec_group)
{
  return fec_group == 0 ? 0 : static_cast<std::uint16_t>((chunks + fec_group - 1) / fec_group);
}

void Parity::add(const std::vector<std::uint8_t> & piece)
{
  added_ = true;
  bytes_[0] ^= static_cast<std::uint8_t>(piece.size());
  bytes_[1] ^= static_cast<std::uint8_t>(piece.size() >> 8);
  if (bytes_.size() < piece.size() + 2) {
    bytes_.resize(piece.size() + 2, 0);
  }
  for (std::size_t i = 0; i < piece.size(); ++i) {
    bytes_[i + 2] ^= piece[i];
  }
}

std::vector<std::uint8_t> Parity::take()
{
  std::vector<std::uint8_t> out{0, 0};
  out.swap(bytes_);
  added_ = false;
  return out;
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

std::optional<Assembled> Reassembler::add(const Header & header, std::vector<std::uint8_t> piece)
{
  if (newest_ && header.stream_seq < *newest_) {
    return std::nullopt;
  }
  // A piece of the item just delivered, such as the parity after its last data piece: a
  // one-piece group's parity is the piece itself and would deliver the item twice.
  if (done_ && newest_ == header.stream_seq) {
    return std::nullopt;
  }
  if (header.chunks == 1 && header.fec_group == 0) {
    newest_ = header.stream_seq;
    done_ = true;
    return Assembled{std::move(piece), 0};
  }
  if (
    newest_ != header.stream_seq || pieces_.size() != header.chunks ||
    fec_group_ != header.fec_group) {
    newest_ = header.stream_seq;
    fec_group_ = header.fec_group;
    pieces_.assign(header.chunks, {});
    present_.assign(header.chunks, false);
    parity_.assign(parity_count(header.chunks, header.fec_group), {});
    received_ = 0;
    recovered_ = 0;
    done_ = false;
  }
  if (done_ || piece.empty()) {
    return std::nullopt;
  }
  std::size_t group = 0;
  if (header.chunk >= header.chunks) {
    group = header.chunk - header.chunks;
    if (!parity_[group].empty()) {
      return std::nullopt;
    }
    parity_[group] = std::move(piece);
  } else {
    if (present_[header.chunk]) {
      return std::nullopt;
    }
    pieces_[header.chunk] = std::move(piece);
    present_[header.chunk] = true;
    ++received_;
    group = fec_group_ == 0 ? 0 : header.chunk / fec_group_;
  }
  if (fec_group_ != 0) {
    recover(group);
  }
  if (received_ < pieces_.size()) {
    return std::nullopt;
  }
  done_ = true;
  Assembled out{{}, recovered_};
  for (auto & p : pieces_) {
    out.item.insert(out.item.end(), p.begin(), p.end());
  }
  pieces_.clear();
  parity_.clear();
  return out;
}

void Reassembler::recover(std::size_t group)
{
  if (parity_[group].empty()) {
    return;
  }
  const std::size_t first = group * fec_group_;
  const std::size_t last = std::min(first + fec_group_, pieces_.size());
  std::optional<std::size_t> missing;
  Parity rest;
  for (std::size_t i = first; i < last; ++i) {
    if (!present_[i]) {
      if (missing) {
        return;
      }
      missing = i;
    } else {
      rest.add(pieces_[i]);
    }
  }
  if (!missing) {
    return;
  }
  const std::vector<std::uint8_t> others = rest.take();
  const std::vector<std::uint8_t> & parity = parity_[group];
  if (parity.size() < 2) {
    return;
  }
  const std::size_t size = (parity[0] ^ others[0]) | ((parity[1] ^ others[1]) << 8);
  if (size + 2 > parity.size()) {
    return;
  }
  auto & out = pieces_[*missing];
  out.resize(size);
  for (std::size_t k = 0; k < size; ++k) {
    out[k] = parity[k + 2] ^ (k + 2 < others.size() ? others[k + 2] : 0);
  }
  present_[*missing] = true;
  ++received_;
  ++recovered_;
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
