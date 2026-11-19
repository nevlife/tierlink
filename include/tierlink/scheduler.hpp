#ifndef TIERLINK__SCHEDULER_HPP_
#define TIERLINK__SCHEDULER_HPP_

#include "tierlink/rate_controller.hpp"
#include "tierlink/stream.hpp"
#include "tierlink/wire.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace tierlink
{

// One piece of an item: the whole item, one chunk of a larger one, or a parity piece
// (chunk >= chunks).
struct Piece
{
  std::size_t id;
  std::vector<std::uint8_t> payload;
  bool keyframe;
  std::uint16_t chunk;
  std::uint16_t chunks;
  std::uint8_t fec_group = 0;
};

// Sends pending data within a bit budget: teleop streams before monitor streams, and within a
// tier in declaration order. An item larger than `chunk_bytes` goes out piece by piece, each
// paced by the budget, so a large monitor item (a point cloud) neither floods the link nor holds
// the teleop frames that arrive meanwhile.
class Scheduler
{
public:
  // `burst` bounds unused budget carried over, so an idle stream cannot later flood the link.
  Scheduler(
    std::vector<StreamSpec> specs, std::uint64_t budget_bps, Nanos burst, std::size_t chunk_bytes);

  std::optional<std::size_t> id(std::string_view name) const;
  // Parity bytes per payload byte of this stream's recent items, so a sender can leave room for
  // them: a 3-piece frame with fec_group 4 still gets one parity piece, a third more.
  double parity_ratio(std::size_t id) const { return parity_ratio_[id]; }
  const StreamSpec & spec(std::size_t id) const { return specs_[id]; }
  std::size_t size() const { return specs_.size(); }
  void set_budget(std::uint64_t bps) { budget_bps_ = bps; }
  Pushed push(std::size_t id, Item item, Nanos now)
  {
    return outboxes_[id].push(std::move(item), now);
  }

  // A piece larger than the tokens left is still sent and leaves the bucket in debt, so the debt is
  // at most one chunk.
  std::optional<Piece> next(Nanos now);

private:
  std::vector<StreamSpec> specs_;
  std::vector<Outbox> outboxes_;
  struct Partial
  {
    Item item;
    std::size_t offset;
    std::uint16_t chunk;
    std::uint16_t chunks;
    Parity parity;
    std::uint16_t parity_sent = 0;
    std::size_t parity_bytes = 0;
  };
  Piece take_piece(std::size_t id);
  std::size_t chunk_of(std::size_t id) const
  {
    return specs_[id].chunk_bytes != 0 ? specs_[id].chunk_bytes : chunk_bytes_;
  }
  std::vector<std::optional<Partial>> partial_;
  std::vector<double> parity_ratio_;
  std::size_t chunk_bytes_;
  std::vector<std::size_t> order_;
  std::uint64_t budget_bps_;
  Nanos burst_;
  double tokens_bits_ = 0.0;
  std::optional<Nanos> last_refill_;
};

}  // namespace tierlink

#endif  // TIERLINK__SCHEDULER_HPP_
