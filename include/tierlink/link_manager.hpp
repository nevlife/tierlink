#ifndef TIERLINK__LINK_MANAGER_HPP_
#define TIERLINK__LINK_MANAGER_HPP_

#include "tierlink/rate_controller.hpp"
#include "tierlink/stream.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

// One estimate of the vehicle's uplink from every process's reports, and a budget per tier:
// separate controllers per process would each read the same queue and cut or grow on their own.
namespace tierlink
{

struct LinkReport
{
  Tier tier;
  std::uint64_t sent_bps;
  std::uint64_t acked_bps;
  Nanos queue_delay;
  Nanos stall;
  std::uint64_t lost_bps = 0;
  // DelayTracker::take_usage over the report period.
  Usage usage = Usage::Normal;
};

std::vector<std::uint8_t> encode_report(const LinkReport & r);
std::optional<LinkReport> decode_report(const std::vector<std::uint8_t> & b);

struct LinkSignal
{
  Nanos delay{0};
  // Teleop's gradient while teleop is active, else monitoring's. Monitoring runs over TCP, whose
  // delay also grows in the vehicle's own send buffer, which the UDP video does not share.
  Usage usage = Usage::Normal;
  // Teleop's lost / (acked + lost): over UDP a loss is seen only there.
  double loss = 0.0;
  std::uint64_t sent_bps = 0;
  std::uint64_t acked_bps = 0;
};

class LinkEstimator
{
public:
  virtual ~LinkEstimator() = default;
  virtual std::uint64_t update(Nanos now, const LinkSignal & signal) = 0;
};

struct AimdLinkConfig
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
};

// Cuts when the delay passes a threshold, steps up after the link stayed clear for a probe period.
class AimdLink : public LinkEstimator
{
public:
  explicit AimdLink(const AimdLinkConfig & config);
  std::uint64_t update(Nanos now, const LinkSignal & signal) override;

private:
  AimdLinkConfig config_;
  std::uint64_t bps_;
  std::optional<Nanos> last_cut_;
  std::optional<Nanos> clear_since_;
};

struct GccLinkConfig
{
  std::uint64_t start_bps;
  std::uint64_t min_bps;
  std::uint64_t max_bps;
  // On overuse the rate becomes this fraction of what was acknowledged.
  double decrease;
  // Multiplicative growth per second, far from the last overuse.
  double increase_per_s;
  // Additive growth per second near the rate where the last overuse happened.
  std::uint64_t additive_bps_per_s;
  Nanos hold;
  // A queue this long counts as overuse even when the gradient is flat: a queue that stopped
  // growing still delays every frame.
  Nanos congested_above;
  // Loss above loss_high cuts by half the loss fraction; below loss_low the rate may grow.
  double loss_high;
  double loss_low;
};

// draft-ietf-rmcat-gcc: the delay gradient decides, loss caps it.
class GccLink : public LinkEstimator
{
public:
  explicit GccLink(const GccLinkConfig & config);
  std::uint64_t update(Nanos now, const LinkSignal & signal) override;

private:
  GccLinkConfig config_;
  double bps_;
  std::optional<Nanos> last_update_;
  std::optional<Nanos> last_cut_;
  std::optional<Nanos> last_loss_cut_;
  // Mean and variance of the acknowledged rate at overuse, normalized by the mean.
  std::optional<double> capacity_bps_;
  double capacity_var_ = 0.4;
};

struct SplitConfig
{
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
  // Teleop gets its peak over the window times the headroom; what it leaves goes to monitoring.
  double teleop_headroom = 1.25;
  Nanos teleop_usage_window{std::chrono::seconds(1)};
};

struct Budget
{
  std::uint64_t link_bps;
  std::array<std::uint64_t, 2> tier_bps;
};

class LinkManager
{
public:
  LinkManager(const SplitConfig & split, std::unique_ptr<LinkEstimator> estimator);
  void on_report(const LinkReport & report, Nanos now);
  Budget update(Nanos now);

private:
  SplitConfig split_;
  std::unique_ptr<LinkEstimator> estimator_;
  std::array<std::optional<std::pair<Nanos, LinkReport>>, 2> latest_;
  // Overuse reported since the last update, per tier.
  std::array<bool, 2> over_{false, false};
  std::array<std::optional<Nanos>, 2> last_acked_;
  std::deque<std::pair<Nanos, std::uint64_t>> teleop_sent_;
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
