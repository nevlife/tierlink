#include "tierlink/scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <numeric>
#include <utility>
#include <vector>

namespace tierlink
{

Scheduler::Scheduler(
  std::vector<StreamSpec> specs, std::uint64_t budget_bps, Nanos burst, std::size_t chunk_bytes)
: specs_(std::move(specs)),
  partial_(specs_.size()),
  parity_ratio_(specs_.size(), 0.0),
  chunk_bytes_(chunk_bytes),
  order_(specs_.size()),
  budget_bps_(budget_bps),
  burst_(burst)
{
  outboxes_.reserve(specs_.size());
  for (const auto & s : specs_) {
    outboxes_.emplace_back(s);
  }
  std::iota(order_.begin(), order_.end(), 0);
  std::stable_sort(order_.begin(), order_.end(), [this](std::size_t a, std::size_t b) {
    return specs_[a].tier < specs_[b].tier;
  });
}

std::optional<std::size_t> Scheduler::id(std::string_view name) const
{
  for (std::size_t i = 0; i < specs_.size(); ++i) {
    if (specs_[i].name == name) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<Piece> Scheduler::next(Nanos now)
{
  using Seconds = std::chrono::duration<double>;
  const auto rate = static_cast<double>(budget_bps_);
  const double elapsed = last_refill_ ? Seconds(now - *last_refill_).count() : 0.0;
  last_refill_ = now;
  tokens_bits_ = std::min(tokens_bits_ + rate * elapsed, rate * Seconds(burst_).count());
  if (tokens_bits_ <= 0.0) {
    return std::nullopt;
  }
  for (const std::size_t id : order_) {
    if (!partial_[id]) {
      if (outboxes_[id].empty()) {
        continue;
      }
      Item item = *outboxes_[id].pop();
      const std::size_t chunk = chunk_of(id);
      const auto chunks = static_cast<std::uint16_t>(
        std::max<std::size_t>(1, (item.payload.size() + chunk - 1) / chunk));
      partial_[id] = Partial{std::move(item), 0, 0, chunks, {}, 0, 0};
    }
    Piece piece = take_piece(id);
    tokens_bits_ -= static_cast<double>(piece.payload.size() * 8);
    return piece;
  }
  return std::nullopt;
}

Piece Scheduler::take_piece(std::size_t id)
{
  Partial & p = *partial_[id];
  const std::uint8_t g = specs_[id].fec_group;
  const auto finish = [&](Piece piece) {
    const bool parity_left = g != 0 && p.parity_sent < parity_count(p.chunks, g);
    if (p.chunk == p.chunks && !parity_left) {
      if (g != 0 && !p.item.payload.empty()) {
        const double r =
          static_cast<double>(p.parity_bytes) / static_cast<double>(p.item.payload.size());
        parity_ratio_[id] = 0.9 * parity_ratio_[id] + 0.1 * r;
      }
      partial_[id].reset();
    }
    return piece;
  };
  // A group's parity goes out right after its last piece, so a loss is repaired within the frame.
  const bool group_done = g != 0 && !p.parity.empty() && (p.chunk % g == 0 || p.chunk == p.chunks);
  if (group_done) {
    Piece parity{
      id, p.parity.take(), p.item.keyframe,
      static_cast<std::uint16_t>(p.chunks + p.parity_sent), p.chunks, g};
    ++p.parity_sent;
    p.parity_bytes += parity.payload.size();
    return finish(std::move(parity));
  }
  const std::size_t n = std::min(chunk_of(id), p.item.payload.size() - p.offset);
  Piece piece{
    id,
    std::vector<std::uint8_t>(
      p.item.payload.begin() + static_cast<std::ptrdiff_t>(p.offset),
      p.item.payload.begin() + static_cast<std::ptrdiff_t>(p.offset + n)),
    p.item.keyframe,
    p.chunk,
    p.chunks,
    g};
  p.offset += n;
  ++p.chunk;
  if (g != 0) {
    p.parity.add(piece.payload);
  }
  return finish(std::move(piece));
}

}  // namespace tierlink
