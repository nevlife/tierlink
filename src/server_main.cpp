#include "tierlink/control.hpp"
#include "tierlink/zenoh_link.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The server: a Zenoh router for vehicles and operators, the receiver of both tiers, and the
// issuer of teleop session epochs. Prints per-stream counts every second and can write a
// stream's payloads to a file (an H.265 stream is then playable as is).
//
// tierlink_server --listen tcp/0.0.0.0:7447 --prefix scv/r01 [--dump front_cam=/tmp/front.h265]

namespace
{

volatile std::sig_atomic_t g_stop = 0;

struct Counts
{
  std::uint64_t items = 0;
  std::uint64_t bytes = 0;
  std::uint64_t lost = 0;
  std::uint64_t keyframes = 0;
};

struct Args
{
  std::string listen;
  std::string prefix;
  std::map<std::string, std::string> dumps;
};

Args parse(int argc, char ** argv)
{
  Args a;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    const std::string value = argv[i + 1];
    if (flag == "--listen") {
      a.listen = value;
    } else if (flag == "--prefix") {
      a.prefix = value;
    } else if (flag == "--dump" && value.find('=') != std::string::npos) {
      a.dumps[value.substr(0, value.find('='))] = value.substr(value.find('=') + 1);
    }
  }
  return a;
}

}  // namespace

int main(int argc, char ** argv)
{
  const Args args = parse(argc, argv);
  if (args.listen.empty() || args.prefix.empty()) {
    std::fprintf(
      stderr, "usage: %s --listen <endpoint> --prefix <prefix> [--dump s=file]\n", argv[0]);
    return 2;
  }
  std::signal(SIGINT, [](int) { g_stop = 1; });
  std::signal(SIGTERM, [](int) { g_stop = 1; });

  auto session =
    zenoh::Session::open(tierlink::make_config("router", {}, {args.listen}, std::nullopt));
  std::mutex mutex;
  std::map<std::string, Counts> counts;
  std::map<std::string, std::ofstream> files;
  for (const auto & [stream, path] : args.dumps) {
    files.emplace(stream, std::ofstream(path, std::ios::binary));
  }
  tierlink::EpochIssuer issuer(session, args.prefix);
  tierlink::Receiver receiver(
    session, args.prefix, std::chrono::milliseconds(250), [&](tierlink::Received r) {
      std::lock_guard<std::mutex> lock(mutex);
      auto & c = counts[std::string(tierlink::tier_name(r.tier)) + "/" + r.stream];
      ++c.items;
      c.bytes += r.payload.size();
      c.lost += r.lost;
      c.keyframes += r.keyframe ? 1 : 0;
      const auto f = files.find(r.stream);
      if (f != files.end()) {
        f->second.write(reinterpret_cast<const char *>(r.payload.data()), r.payload.size());
        f->second.flush();
      }
    });
  std::printf("tierlink_server listening on %s for %s\n", args.listen.c_str(), args.prefix.c_str());
  while (g_stop == 0) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::lock_guard<std::mutex> lock(mutex);
    for (auto & [name, c] : counts) {
      std::printf(
        "%s: %lu items, %.0f kbit/s, %lu lost, %lu keyframes\n", name.c_str(), c.items,
        static_cast<double>(c.bytes) * 8 / 1000, c.lost, c.keyframes);
      c = Counts{};
    }
    std::fflush(stdout);
  }
  return 0;
}
