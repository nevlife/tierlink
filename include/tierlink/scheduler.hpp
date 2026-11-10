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

// Sends pending data within a bit budget: teleop streams before monitor streams, and within a
// tier in declaration order.
class Scheduler
{
public:
  // `burst` bounds unused budget carried over, so an idle stream cannot later flood the link.
  Scheduler(std::vector<StreamSpec> specs, std::uint64_t budget_bps, Nanos burst);

  std::optional<std::size_t> id(std::string_view name) const;
  const StreamSpec & spec(std::size_t id) const { return specs_[id]; }
  std::size_t size() const { return specs_.size(); }
  void set_budget(std::uint64_t bps) { budget_bps_ = bps; }
  Pushed push(std::size_t id, Item item, Nanos now)
  {
    return outboxes_[id].push(std::move(item), now);
  }

  // An item larger than the tokens left is still sent and leaves the bucket in debt, so a large
  // frame is delayed by its own size instead of waiting forever.
  std::optional<std::pair<std::size_t, Item>> next(Nanos now);

private:
  std::vector<StreamSpec> specs_;
  std::vector<Outbox> outboxes_;
  std::vector<std::size_t> order_;
  std::uint64_t budget_bps_;
  Nanos burst_;
  double tokens_bits_ = 0.0;
  std::optional<Nanos> last_refill_;
};

// How the teleop process limits monitoring while it drives.
struct TierPolicy
{
  // Fraction of the link budget monitoring may use while teleop is active.
  double monitor_share;
  // Above this queue delay monitoring stops entirely, before teleop is cut.
  Nanos monitor_stop_above;

  std::uint64_t monitor_allowance(
    const Decision & link, std::uint64_t teleop_bps, const Feedback & feedback) const;
};

inline constexpr const char * kAllowanceName = "allowance";

// The last published allowance; once it expires, monitoring runs on its own controller.
class Allowance
{
public:
  explicit Allowance(Nanos expires_after) : expires_after_(expires_after) {}
  void update(std::uint64_t bps, Nanos now) { latest_ = std::make_pair(now, bps); }
  // The cap for the monitoring Uplink (set_cap), or nothing once the allowance expired.
  std::optional<std::uint64_t> cap(Nanos now) const;

private:
  Nanos expires_after_;
  std::optional<std::pair<Nanos, std::uint64_t>> latest_;
};

}  // namespace tierlink

#endif  // TIERLINK__SCHEDULER_HPP_
