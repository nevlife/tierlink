// tierlink_bench: the traffic of one vehicle, for comparing link controllers under netem.
//
// tierlink_bench recv --params <file> --listen <endpoint>... --seconds <n>
//   The server: acks both tiers and prints one line per item that arrived.
// tierlink_bench send --params <file> --connect <edge endpoint> --seconds <n> [--monitor aimd|ledbat]
//   The vehicle's processes behind tierlink_edge: H.265-like video on the teleop tier sized to its
//   budget (15 fps, a keyframe 4 frames large when one is requested), a point cloud (50 kB, 2 Hz,
//   Latest) and telemetry (200 B, 10 Hz, Reliable) on the monitor tier, and the arm state
//   (60 B, 10 Hz) as a control message.
//
// Every payload starts with the sender's system clock in nanoseconds; run both on one host.
// Telemetry is declared before the point cloud, so the scheduler sends it first within the tier.
// Lines: "<kind> <t s> <delay ms> <bytes> <lost items> <pieces rebuilt>", kind v (video),
// k (keyframe), p (point cloud), m (telemetry), s (state).
#include "tierlink/control.hpp"
#include "tierlink/link_manager.hpp"
#include "tierlink/params.hpp"
#include "tierlink/zenoh_link.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

using std::chrono::milliseconds;

std::int64_t wall_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

std::vector<std::uint8_t> stamped(std::size_t size)
{
  std::vector<std::uint8_t> b(std::max<std::size_t>(size, 8));
  const auto now = static_cast<std::uint64_t>(wall_ns());
  for (int i = 0; i < 8; ++i) {
    b[i] = static_cast<std::uint8_t>(now >> (8 * i));
  }
  return b;
}

double delay_ms(const std::vector<std::uint8_t> & b)
{
  std::int64_t sent = 0;
  for (int i = 0; i < 8 && b.size() >= 8; ++i) {
    sent |= static_cast<std::int64_t>(b[i]) << (8 * i);
  }
  return static_cast<double>(wall_ns() - sent) / 1e6;
}

struct Args
{
  std::string role;
  std::string params;
  std::vector<std::string> listen;
  std::vector<std::string> connect;
  int seconds = 0;
  std::string monitor = "aimd";
};

Args parse(int argc, char ** argv)
{
  Args a;
  a.role = argc > 1 ? argv[1] : "";
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string f = argv[i];
    const std::string v = argv[i + 1];
    if (f == "--params") {
      a.params = v;
    } else if (f == "--listen") {
      a.listen.push_back(v);
    } else if (f == "--connect") {
      a.connect.push_back(v);
    } else if (f == "--seconds") {
      a.seconds = std::stoi(v);
    } else if (f == "--monitor") {
      a.monitor = v;
    }
  }
  return a;
}

int recv(const Args & a, const tierlink::Params & p)
{
  auto config = tierlink::zenoh_config(p.link, "router", {}, a.listen);
  config.insert_json5("transport/unicast/max_links", "4");
  auto session = zenoh::Session::open(std::move(config));
  std::mutex mutex;
  const auto start = std::chrono::steady_clock::now();
  std::printf("start %.6f\n", static_cast<double>(wall_ns()) / 1e9);
  const auto line = [&](char kind, const std::vector<std::uint8_t> & b, std::uint64_t lost,
                        std::size_t recovered) {
    const double d = delay_ms(b);
    std::lock_guard<std::mutex> lock(mutex);
    std::printf(
      "%c %.3f %.1f %zu %lu %zu\n", kind,
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), d, b.size(),
      lost, recovered);
  };
  tierlink::Receiver rx(session, p.edge.prefix, milliseconds(250), [&](tierlink::Received r) {
    const char kind = r.stream == "front_cam" ? (r.keyframe ? 'k' : 'v')
                      : r.stream == "pcd"     ? 'p'
                                              : 'm';
    line(kind, r.payload, r.lost, r.recovered);
  });
  tierlink::ControlSubscriber state(
    session, p.edge.prefix, tierlink::Tier::Teleop, "state",
    [&](const std::vector<std::uint8_t> & b) { line('s', b, 0, 0); });
  std::this_thread::sleep_for(std::chrono::seconds(a.seconds));
  std::fflush(stdout);
  return 0;
}

// teleop_video's and fleet_telemetry's own controller (rate.* in their parameters).
std::unique_ptr<tierlink::RateController> aimd()
{
  return std::make_unique<tierlink::Aimd>(tierlink::AimdConfig{
    500'000, 150'000, 2'000'000, 100'000, 0.8, milliseconds(300), milliseconds(1000),
    milliseconds(100), milliseconds(30)});
}

std::unique_ptr<tierlink::RateController> monitor_controller(const Args & a, std::size_t chunk)
{
  if (a.monitor == "ledbat") {
    return std::make_unique<tierlink::Ledbat>(tierlink::LedbatConfig{
      milliseconds(25), 1.0, chunk, 2 * chunk, 8 * chunk, 256 * chunk, 4'000'000,
      milliseconds(100), milliseconds(500)});
  }
  return aimd();
}

int send(const Args & a, const tierlink::Params & p)
{
  auto session =
    zenoh::Session::open(tierlink::zenoh_config(p.link, "client", a.connect, {}));
  const auto & prefix = p.edge.prefix;
  const auto uplink = [&](tierlink::Tier tier, std::vector<tierlink::StreamSpec> specs,
                          std::shared_ptr<tierlink::BudgetSlot> slot,
                          std::unique_ptr<tierlink::RateController> own, std::size_t chunk) {
    return std::make_unique<tierlink::Uplink>(
      session,
      tierlink::UplinkConfig{
        prefix, tier, std::move(specs), milliseconds(5), milliseconds(100), std::chrono::seconds(10),
        milliseconds(100), chunk},
      std::make_unique<tierlink::Budgeted>(slot, std::move(own), milliseconds(500)));
  };
  auto vslot = std::make_shared<tierlink::BudgetSlot>();
  auto mslot = std::make_shared<tierlink::BudgetSlot>();
  tierlink::StreamSpec video{
    "front_cam", tierlink::Tier::Teleop, tierlink::Delivery::Frame, true, 1};
  video.max_delay = milliseconds(250);
  video.fec_group = p.link.fec_group;
  auto vup = uplink(tierlink::Tier::Teleop, {video}, vslot, aimd(), p.link.frame_chunk_bytes);
  auto mup = uplink(
    tierlink::Tier::Monitor,
    {{"telemetry", tierlink::Tier::Monitor, tierlink::Delivery::Reliable, false, 64},
     {"pcd", tierlink::Tier::Monitor, tierlink::Delivery::Latest, false, 1}},
    mslot, monitor_controller(a, p.link.chunk_bytes), p.link.chunk_bytes);
  tierlink::BudgetSubscriber vsub(session, prefix, tierlink::Tier::Teleop, vslot);
  tierlink::BudgetSubscriber msub(session, prefix, tierlink::Tier::Monitor, mslot);
  tierlink::ControlPublisher state(session, prefix, tierlink::Tier::Teleop, "state");

  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(a.seconds);
  auto wake = std::chrono::steady_clock::now();
  bool first = true;
  for (int tick = 0; std::chrono::steady_clock::now() < end; ++tick) {
    // 30 ticks per second: video every second tick, the rest by their rates.
    wake += std::chrono::microseconds(33'333);
    std::this_thread::sleep_until(wake);
    if (tick % 2 == 0) {
      const auto [decision, feedback] = vup->status();
      // The encoder gets 90 % of the teleop rate less the parity, as teleop_video.
      const auto bps = static_cast<std::uint64_t>(
        static_cast<double>(decision ? decision->target_bps : 500'000) * 0.9 /
        (1.0 + vup->parity_ratio(0)));
      const bool key = first || vup->take_keyframe_request(0);
      first = false;
      vup->push(0, stamped(bps / 15 / 8 * (key ? 4 : 1)), key);
    }
    if (tick % 15 == 0) {
      mup->push(1, stamped(50'000), false);
    }
    if (tick % 3 == 0) {
      mup->push(0, stamped(200), false);
      state.send(stamped(60));
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char ** argv)
{
  const Args a = parse(argc, argv);
  if ((a.role != "recv" && a.role != "send") || a.params.empty() || a.seconds <= 0) {
    std::fprintf(
      stderr,
      "usage: %s recv|send --params <file> --listen|--connect <endpoint>... --seconds <n> "
      "[--monitor aimd|ledbat]\n",
      argv[0]);
    return 2;
  }
  const tierlink::Params p = tierlink::load_params(a.params);
  return a.role == "recv" ? recv(a, p) : send(a, p);
}
