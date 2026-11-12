#ifndef TIERLINK__ZENOH_LINK_HPP_
#define TIERLINK__ZENOH_LINK_HPP_

#include "tierlink/delay_tracker.hpp"
#include "tierlink/link_manager.hpp"
#include "tierlink/scheduler.hpp"
#include "tierlink/stream.hpp"
#include "tierlink/wire.hpp"
#include "zenoh.hxx"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// Keys are <prefix>/<tier>/data/<stream>, <prefix>/<tier>/ack and <prefix>/<tier>/keyframe. Frame
// streams are published best effort: behind a router with a UDP link (`?rel=0`) they take UDP, so
// a lost packet never holds later frames; everything else stays reliable (TCP).
namespace tierlink
{

// Zenoh's own queue holds data the scheduler can no longer drop; by default up to two 64 KiB
// batches per priority, about a second at 1 Mbit/s. A teleop session keeps it short.
struct Tuning
{
  std::uint16_t batch_size;
  std::uint8_t queue_batches;
};

// Link lease in milliseconds; keep-alives go out at a quarter of it.
inline constexpr int kLeaseMs = 1000;

// Starting values until measured on a narrow link.
inline constexpr Tuning kShortQueue{16384, 1};

// Liveliness token of one process: <prefix>/alive/<name>, gone when its link is.
std::string alive_key(const std::string & prefix, const std::string & name);

// Talks only to the given endpoints: no scouting, so a local ROS Zenoh router on the vehicle is
// never joined and ROS traffic cannot leak onto the link.
zenoh::Config make_config(
  const std::string & mode, const std::vector<std::string> & connect,
  const std::vector<std::string> & listen, std::optional<Tuning> tuning);

std::string_view tier_name(Tier tier);
Nanos steady_now();

struct UplinkConfig
{
  std::string prefix;
  Tier tier;
  std::vector<StreamSpec> streams;
  // How often the controller is consulted and pending data is sent.
  Nanos tick;
  Nanos burst;
  Nanos delay_window;
  // How often the Uplink reports to the link manager on <prefix>/link/report/<tier>.
  Nanos report_period;
  // Items larger than this go out in pieces of this size (Scheduler).
  std::size_t chunk_bytes;
};

std::string report_key(const std::string & prefix, Tier tier);
std::string budget_key(const std::string & prefix, Tier tier);

// Writes the link manager's budget for one tier into a slot that Budgeted reads.
class BudgetSubscriber
{
public:
  BudgetSubscriber(
    const zenoh::Session & session, const std::string & prefix, Tier tier,
    std::shared_ptr<BudgetSlot> slot);

private:
  std::optional<zenoh::Subscriber<void>> subscriber_;
};

// One tier's sending side: its streams, its controller and its own session.
class Uplink
{
public:
  Uplink(
    const zenoh::Session & session, UplinkConfig config,
    std::unique_ptr<RateController> controller);
  ~Uplink();
  Uplink(const Uplink &) = delete;
  Uplink & operator=(const Uplink &) = delete;

  std::optional<std::size_t> stream_id(std::string_view name) const;
  Pushed push(std::size_t id, std::vector<std::uint8_t> payload, bool keyframe);
  // True once after this stream dropped a frame or the receiver asked for a keyframe; the encoder
  // checks it before each frame.
  bool take_keyframe_request(std::size_t id);
  // An upper bound from outside, such as the allowance the teleop process publishes.
  void set_cap(std::optional<std::uint64_t> bps);
  std::pair<std::optional<Decision>, Feedback> status() const;

private:
  struct Outgoing
  {
    std::size_t id;
    Header header;
    std::vector<std::uint8_t> payload;
  };
  void run(Nanos tick, Nanos report_period);
  std::vector<Outgoing> take_batch();
  LinkReport report(Nanos now);

  // Pushes come from node callbacks, acknowledgements from Zenoh threads, sending from run().
  mutable std::mutex mutex_;
  Scheduler scheduler_;
  DelayTracker tracker_;
  std::unique_ptr<RateController> controller_;
  std::optional<std::uint64_t> cap_bps_;
  std::optional<Decision> decision_;
  Tier tier_;
  // (time, sent bytes, acked bytes) over the last second, for the report's rates.
  std::deque<std::tuple<Nanos, std::uint64_t, std::uint64_t>> totals_;
  std::uint64_t next_seq_ = 0;
  std::vector<std::uint64_t> stream_seq_;
  // The stream_seq of the item whose pieces are going out.
  std::vector<std::uint64_t> item_seq_;
  std::vector<bool> keyframe_requested_;

  std::vector<zenoh::Publisher> publishers_;
  zenoh::Publisher report_pub_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  // Declared last, so they are undeclared first and no callback outlives the state.
  std::optional<zenoh::Subscriber<void>> acks_;
  std::optional<zenoh::Subscriber<void>> keyframe_requests_;
};

struct Received
{
  Tier tier;
  std::string stream;
  std::uint64_t seq;
  bool keyframe;
  // Items of this stream that the transport dropped just before this one.
  std::uint64_t lost;
  std::vector<std::uint8_t> payload;
};

// The server side: delivers every stream of both tiers, acknowledges each sample and asks for a
// keyframe when a stream has a gap.
class Receiver
{
public:
  Receiver(
    const zenoh::Session & session, const std::string & prefix, Nanos keyframe_repeat,
    std::function<void(Received)> on_received);
  // For a decoder that fails without a visible gap.
  void request_keyframe(Tier tier, const std::string & stream);

private:
  struct PerTier
  {
    PerTier(const zenoh::Session & session, const std::string & base);
    zenoh::Publisher acks;
    zenoh::Publisher keyframe_requests;
    // Zenoh may call the subscriber from more than one thread.
    std::mutex mutex;
    std::unordered_map<std::string, GapTracker> gaps;
    std::unordered_map<std::string, Reassembler> pieces;
    std::optional<zenoh::Subscriber<void>> subscriber;
  };
  std::array<std::unique_ptr<PerTier>, 2> tiers_;
};

}  // namespace tierlink

#endif  // TIERLINK__ZENOH_LINK_HPP_
