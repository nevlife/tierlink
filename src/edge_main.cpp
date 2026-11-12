#include "tierlink/link_manager.hpp"
#include "tierlink/zenoh_link.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The vehicle's side of the link: a Zenoh router that the vehicle's processes connect to on this
// host and that holds the links to the server, and the link manager that turns the processes'
// reports into one link rate and a budget per tier. With a TCP and a UDP endpoint (?rel=1 and
// ?rel=0), reliable traffic takes TCP and best-effort frames take UDP.
//
// tierlink_edge --prefix scv/r01 --listen tcp/127.0.0.1:7447
//   --connect tcp/<server>:<port>?rel=1 [--connect udp/<server>:<port>?rel=0]

namespace
{

volatile std::sig_atomic_t g_stop = 0;

using std::chrono::milliseconds;

// The teleop encoder's range (teleop_video's rate.*) and the delay thresholds of its controller.
constexpr tierlink::LinkManagerConfig kManager{
  500'000,
  150'000,
  4'000'000,
  0.85,
  0.1,
  100'000,
  milliseconds(300),
  milliseconds(1000),
  milliseconds(100),
  milliseconds(30),
  milliseconds(50),
  150'000,
  2'000'000,
  30'000,
  milliseconds(500),
  milliseconds(3000)};

constexpr milliseconds kBudgetPeriod{100};

}  // namespace

int main(int argc, char ** argv)
{
  std::string prefix;
  std::vector<std::string> listen;
  std::vector<std::string> connect;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    if (flag == "--prefix") {
      prefix = argv[i + 1];
    } else if (flag == "--listen") {
      listen.emplace_back(argv[i + 1]);
    } else if (flag == "--connect") {
      connect.emplace_back(argv[i + 1]);
    }
  }
  if (prefix.empty() || listen.empty() || connect.empty()) {
    std::fprintf(
      stderr, "usage: %s --prefix <prefix> --listen <endpoint> --connect <endpoint>...\n", argv[0]);
    return 2;
  }
  std::signal(SIGINT, [](int) { g_stop = 1; });
  std::signal(SIGTERM, [](int) { g_stop = 1; });

  auto config = tierlink::make_config("router", connect, listen, std::nullopt);
  // A router opens every endpoint; one TCP and one UDP link to the server form one transport.
  config.insert_json5("transport/unicast/max_links", "4");
  // One UDP datagram per packet: a fragmented datagram is lost with any of its fragments.
  config.insert_json5("transport/link/tx/batch_size", "1400");
  auto session = zenoh::Session::open(std::move(config));

  std::mutex mutex;
  tierlink::LinkManager manager(kManager);
  auto reports = session.declare_subscriber(
    zenoh::KeyExpr(prefix + "/link/report/*"),
    [&](zenoh::Sample & sample) {
      const auto r = tierlink::decode_report(sample.get_payload().as_vector());
      if (r) {
        std::lock_guard<std::mutex> lock(mutex);
        manager.on_report(*r, tierlink::steady_now());
      }
    },
    zenoh::closures::none);
  std::vector<zenoh::Publisher> budgets;
  for (const auto tier : {tierlink::Tier::Teleop, tierlink::Tier::Monitor}) {
    budgets.push_back(
      session.declare_publisher(zenoh::KeyExpr(tierlink::budget_key(prefix, tier))));
  }

  std::printf("tierlink_edge for %s\n", prefix.c_str());
  auto wake = std::chrono::steady_clock::now();
  int tick = 0;
  while (g_stop == 0) {
    wake += kBudgetPeriod;
    std::this_thread::sleep_until(wake);
    tierlink::Budget b{};
    {
      std::lock_guard<std::mutex> lock(mutex);
      b = manager.update(tierlink::steady_now());
    }
    for (std::size_t t = 0; t < budgets.size(); ++t) {
      budgets[t].put(zenoh::Bytes(tierlink::encode_bps(b.tier_bps[t])));
    }
    if (++tick % 10 == 0) {
      std::printf(
        "link %lu kbit/s: teleop %lu, monitor %lu\n", b.link_bps / 1000, b.tier_bps[0] / 1000,
        b.tier_bps[1] / 1000);
      std::fflush(stdout);
    }
  }
  return 0;
}
