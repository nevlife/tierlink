#include "tierlink/link_manager.hpp"
#include "tierlink/params.hpp"
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
// tierlink_edge --params <file.yaml> [--prefix p] [--listen e]... [--connect e]... [--estimator x]
// The flags replace the file's edge.prefix, edge.listen, edge.connect and edge.estimator.

namespace
{

volatile std::sig_atomic_t g_stop = 0;

}  // namespace

int main(int argc, char ** argv)
{
  std::string path;
  std::vector<std::string> listen;
  std::vector<std::string> connect;
  std::string prefix;
  std::string estimator;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    if (flag == "--params") {
      path = argv[i + 1];
    } else if (flag == "--prefix") {
      prefix = argv[i + 1];
    } else if (flag == "--listen") {
      listen.emplace_back(argv[i + 1]);
    } else if (flag == "--connect") {
      connect.emplace_back(argv[i + 1]);
    } else if (flag == "--estimator") {
      estimator = argv[i + 1];
    }
  }
  if (path.empty()) {
    std::fprintf(
      stderr,
      "usage: %s --params <file.yaml> [--prefix p] [--listen e]... [--connect e]... "
      "[--estimator aimd|gcc]\n",
      argv[0]);
    return 2;
  }
  tierlink::Params params = tierlink::load_params(path);
  auto & edge = params.edge;
  edge.prefix = prefix.empty() ? edge.prefix : prefix;
  edge.listen = listen.empty() ? edge.listen : listen;
  edge.connect = connect.empty() ? edge.connect : connect;
  if (!estimator.empty()) {
    edge.estimator = estimator;
  }
  std::signal(SIGINT, [](int) { g_stop = 1; });
  std::signal(SIGTERM, [](int) { g_stop = 1; });

  auto config = tierlink::zenoh_config(params.link, "router", edge.connect, edge.listen);
  // A router opens every endpoint; one TCP and one UDP link to the server form one transport.
  config.insert_json5("transport/unicast/max_links", "4");
  auto session = zenoh::Session::open(std::move(config));

  std::mutex mutex;
  tierlink::LinkManager manager(edge.split, tierlink::make_estimator(edge));
  auto reports = session.declare_subscriber(
    zenoh::KeyExpr(edge.prefix + "/link/report/*"),
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
      session.declare_publisher(zenoh::KeyExpr(tierlink::budget_key(edge.prefix, tier))));
  }

  std::printf("tierlink_edge for %s, estimator %s\n", edge.prefix.c_str(), edge.estimator.c_str());
  const auto start = std::chrono::steady_clock::now();
  auto wake = start;
  while (g_stop == 0) {
    wake += edge.period;
    std::this_thread::sleep_until(wake);
    tierlink::Budget b{};
    {
      std::lock_guard<std::mutex> lock(mutex);
      b = manager.update(tierlink::steady_now());
    }
    for (std::size_t t = 0; t < budgets.size(); ++t) {
      budgets[t].put(zenoh::Bytes(tierlink::encode_bps(b.tier_bps[t])));
    }
    std::printf(
      "t=%.1f link %lu kbit/s: teleop %lu, monitor %lu\n",
      std::chrono::duration<double>(wake - start).count(), b.link_bps / 1000,
      b.tier_bps[0] / 1000, b.tier_bps[1] / 1000);
    std::fflush(stdout);
  }
  return 0;
}
