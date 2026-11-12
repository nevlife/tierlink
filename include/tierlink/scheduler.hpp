#ifndef TIERLINK__SCHEDULER_HPP_
#define TIERLINK__SCHEDULER_HPP_

#include "tierlink/rate_controller.hpp"
#include "tierlink/stream.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace tierlink
{

// One piece of an item: the whole item, or one chunk of a larger one.
struct Piece
{
  std::size_t id;
  std::vector<std::uint8_t> payload;
  bool keyframe;
  std::uint16_t chunk;
  std::uint16_t chunks;
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
  };
  std::vector<std::optional<Partial>> partial_;
  std::size_t chunk_bytes_;
  std::vector<std::size_t> order_;
  std::uint64_t budget_bps_;
  Nanos burst_;
  double tokens_bits_ = 0.0;
  std::optional<Nanos> last_refill_;
};

}  // namespace tierlink

#endif  // TIERLINK__SCHEDULER_HPP_
