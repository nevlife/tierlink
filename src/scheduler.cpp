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
      const auto chunks = static_cast<std::uint16_t>(
        std::max<std::size_t>(1, (item.payload.size() + chunk_bytes_ - 1) / chunk_bytes_));
      partial_[id] = Partial{std::move(item), 0, 0, chunks};
    }
    Partial & p = *partial_[id];
    const std::size_t n = std::min(chunk_bytes_, p.item.payload.size() - p.offset);
    Piece piece{
      id,
      std::vector<std::uint8_t>(
        p.item.payload.begin() + static_cast<std::ptrdiff_t>(p.offset),
        p.item.payload.begin() + static_cast<std::ptrdiff_t>(p.offset + n)),
      p.item.keyframe, p.chunk, p.chunks};
    p.offset += n;
    if (++p.chunk == p.chunks) {
      partial_[id].reset();
    }
    tokens_bits_ -= static_cast<double>(piece.payload.size() * 8);
    return piece;
  }
  return std::nullopt;
}

}  // namespace tierlink
