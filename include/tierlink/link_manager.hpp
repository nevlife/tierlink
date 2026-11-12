#ifndef TIERLINK__LINK_MANAGER_HPP_
#define TIERLINK__LINK_MANAGER_HPP_

#include "tierlink/rate_controller.hpp"
#include "tierlink/stream.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

// One estimate of the vehicle's uplink, shared by every process that sends on it. Each Uplink
// reports what it sent, what was acknowledged and the delay it sees on <prefix>/link/report/<tier>;
// the link manager turns the reports into one link rate and a budget per tier on
// <prefix>/link/budget/<tier>. Separate controllers per process would each read the same queue
// and cut or grow on their own.
namespace tierlink
{

struct LinkReport
{
  Tier tier;
  std::uint64_t sent_bps;
  std::uint64_t acked_bps;
  Nanos queue_delay;
  Nanos stall;
};

std::vector<std::uint8_t> encode_report(const LinkReport & r);
std::optional<LinkReport> decode_report(const std::vector<std::uint8_t> & b);

struct LinkManagerConfig
{
  std::uint64_t start_bps;
  std::uint64_t min_bps;
  std::uint64_t max_bps;
  // On congestion the link rate becomes this fraction of what was acknowledged.
  double factor;
  // Each increase adds the larger of step_bps and this fraction of the rate.
  double increase;
  std::uint64_t step_bps;
  Nanos hold;
  Nanos probe;
  Nanos congested_above;
  Nanos clear_below;
  // Above this delay monitoring drops to its floor while teleop runs.
  Nanos monitor_stop_above;
  std::uint64_t teleop_min_bps;
  std::uint64_t teleop_max_bps;
  // Kept for vehicle state (odometry, status) even under congestion.
  std::uint64_t monitor_floor_bps;
  // A tier whose last report is older than this counts as idle.
  Nanos report_timeout;
  // A tier counts only while something acknowledged it this recently: video with no operator
  // watching is never acknowledged, and its growing stall says nothing about the link. A tier
  // that was acknowledged and then stops is an outage and still counts.
  Nanos acked_within;
};

struct Budget
{
  std::uint64_t link_bps;
  std::array<std::uint64_t, 2> tier_bps;
};

class LinkManager
{
public:
  explicit LinkManager(const LinkManagerConfig & config);
  void on_report(const LinkReport & report, Nanos now);
  Budget update(Nanos now);

private:
  LinkManagerConfig config_;
  std::uint64_t link_bps_;
  std::array<std::optional<std::pair<Nanos, LinkReport>>, 2> latest_;
  std::array<std::optional<Nanos>, 2> last_acked_;
  std::optional<Nanos> last_cut_;
  std::optional<Nanos> clear_since_;
};

// The latest budget for one tier, written by a subscriber and read by Budgeted.
class BudgetSlot
{
public:
  void set(std::uint64_t bps, Nanos now);
  std::optional<std::pair<Nanos, std::uint64_t>> get() const;

private:
  mutable std::mutex mutex_;
  std::optional<std::pair<Nanos, std::uint64_t>> latest_;
};

// Follows the link manager's budget while it is fresh; without one (no manager, or it stopped) the
// own controller decides, so a process never sends unpaced.
class Budgeted : public RateController
{
public:
  Budgeted(
    std::shared_ptr<const BudgetSlot> slot, std::unique_ptr<RateController> fallback,
    Nanos expires_after);
  Decision update(Nanos now, const Feedback & feedback) override;

private:
  std::shared_ptr<const BudgetSlot> slot_;
  std::unique_ptr<RateController> fallback_;
  Nanos expires_after_;
};

}  // namespace tierlink

#endif  // TIERLINK__LINK_MANAGER_HPP_
