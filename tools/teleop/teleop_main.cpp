#include "tierlink/control.hpp"
#include "tierlink/teleop_messages.hpp"
#include "tierlink/zenoh_link.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

// A debugging operator without video: takes a session epoch from the server, arms the vehicle,
// drives at a fixed speed for some seconds and stops, printing every arm state it receives.
//
// tierlink_teleop <endpoint> <prefix> <robot id> <linear m/s> <seconds>

namespace teleop = tierlink::teleop;

int main(int argc, char ** argv)
{
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;
  using std::chrono::nanoseconds;
  using std::chrono::seconds;
  using std::chrono::steady_clock;
  using std::chrono::system_clock;
  if (argc != 6) {
    std::fprintf(stderr, "usage: %s <endpoint> <prefix> <robot id> <m/s> <seconds>\n", argv[0]);
    return 2;
  }
  const std::string prefix = argv[2];
  const std::string robot = argv[3];
  const double speed = std::stod(argv[4]);
  const auto duration = seconds(std::stoi(argv[5]));
  auto session = zenoh::Session::open(tierlink::make_config("client", {argv[1]}, {}, std::nullopt));
  const auto t = tierlink::Tier::Teleop;
  std::atomic<bool> armed{false};
  tierlink::ControlSubscriber state(
    session, prefix, t, "state", [&armed](const std::vector<std::uint8_t> & m) {
      const auto s = teleop::decode_state(m);
      if (s) {
        armed = s->armed;
        std::printf(
          "state: %s epoch %lu %s\n", s->armed ? "armed" : "disarmed", s->epoch, s->reason.c_str());
      }
    });
  tierlink::ControlPublisher arm(session, prefix, t, "arm");
  tierlink::ControlPublisher motion(session, prefix, t, "motion");
  tierlink::ControlPublisher stop(session, prefix, t, "stop");
  const auto grant = tierlink::request_epoch(session, prefix, 2000);
  if (!grant) {
    std::fprintf(stderr, "no session epoch from the server\n");
    return 1;
  }
  std::printf("session %s epoch %lu\n", grant->session_id.c_str(), grant->epoch);
  for (int i = 0; i < 20 && !armed; ++i) {
    arm.send(
      teleop::encode(teleop::ArmRequest{robot, grant->session_id, grant->epoch}));
    std::this_thread::sleep_for(milliseconds(100));
  }
  if (!armed) {
    std::fprintf(stderr, "the vehicle did not arm\n");
    return 1;
  }
  const auto end = steady_clock::now() + duration;
  for (std::uint64_t seq = 0; steady_clock::now() < end; ++seq) {
    const auto now = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    motion.send(teleop::encode(teleop::MotionCommand{
      robot, grant->session_id, grant->epoch, seq, teleop::UnixNs{now}, milliseconds(200),
      speed, 0.0, true}));
    std::this_thread::sleep_for(milliseconds(50));
  }
  stop.send(
    teleop::encode(teleop::StopCommand{robot, grant->session_id, grant->epoch}));
  std::this_thread::sleep_for(milliseconds(300));
  return 0;
}
