#include "tierlink/zenoh_link.hpp"

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using tierlink::Delivery;
using tierlink::Tier;

namespace
{

int free_port()
{
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);
  close(fd);
  return ntohs(addr.sin_port);
}

struct Inbox
{
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<tierlink::Received> items;

  std::optional<tierlink::Received> wait()
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, std::chrono::seconds(5), [this] { return !items.empty(); })) {
      return std::nullopt;
    }
    auto r = std::move(items.front());
    items.erase(items.begin());
    return r;
  }
};

}  // namespace

TEST(ZenohLink, FramesArriveAcksReturnAndKeyframeRequestsReachTheEncoder)
{
  const std::string endpoint = "tcp/127.0.0.1:" + std::to_string(free_port());
  auto server = zenoh::Session::open(tierlink::make_config("peer", {}, {endpoint}, std::nullopt));
  auto vehicle =
    zenoh::Session::open(tierlink::make_config("client", {endpoint}, {}, tierlink::kShortQueue));
  Inbox inbox;
  tierlink::Receiver receiver(
    server, "test/r01", std::chrono::milliseconds(250), [&inbox](tierlink::Received r) {
      std::lock_guard<std::mutex> lock(inbox.mutex);
      inbox.items.push_back(std::move(r));
      inbox.cv.notify_one();
    });
  tierlink::Uplink uplink(
    vehicle,
    tierlink::UplinkConfig{
      "test/r01",
      Tier::Teleop,
      {{"front_cam", Tier::Teleop, Delivery::Frame, true, 16},
       {"cmd", Tier::Teleop, Delivery::Latest, false, 16}},
      std::chrono::milliseconds(5),
      std::chrono::milliseconds(100),
      std::chrono::seconds(10),
      std::chrono::milliseconds(100),
      4096},
    std::make_unique<tierlink::Fixed>(10'000'000));
  // Declarations reach the other side asynchronously.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  const auto cam = uplink.stream_id("front_cam");
  ASSERT_TRUE(cam);
  uplink.push(*cam, std::vector<std::uint8_t>(2000, 7), true);
  const auto got = inbox.wait();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->tier, Tier::Teleop);
  EXPECT_EQ(got->stream, "front_cam");
  EXPECT_TRUE(got->keyframe);
  EXPECT_EQ(got->lost, 0u);
  EXPECT_EQ(got->payload, std::vector<std::uint8_t>(2000, 7));

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto [decision, feedback] = uplink.status();
  ASSERT_TRUE(decision);
  EXPECT_EQ(decision->target_bps, 10'000'000u);
  EXPECT_EQ(feedback.in_flight_bytes, 0u);

  EXPECT_FALSE(uplink.take_keyframe_request(*cam));
  receiver.request_keyframe(Tier::Teleop, "front_cam");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(uplink.take_keyframe_request(*cam));
  EXPECT_FALSE(uplink.take_keyframe_request(*cam));
}

TEST(ZenohLink, UplinksReportAndFollowTheBudgetThroughTheRouter)
{
  const std::string endpoint = "tcp/127.0.0.1:" + std::to_string(free_port());
  auto edge = zenoh::Session::open(tierlink::make_config("router", {}, {endpoint}, std::nullopt));
  auto vehicle =
    zenoh::Session::open(tierlink::make_config("client", {endpoint}, {}, std::nullopt));
  std::mutex mutex;
  std::condition_variable cv;
  std::optional<tierlink::LinkReport> got;
  auto reports = edge.declare_subscriber(
    zenoh::KeyExpr("test/r01/link/report/*"),
    [&](zenoh::Sample & sample) {
      std::lock_guard<std::mutex> lock(mutex);
      got = tierlink::decode_report(sample.get_payload().as_vector());
      cv.notify_one();
    },
    zenoh::closures::none);
  auto slot = std::make_shared<tierlink::BudgetSlot>();
  tierlink::BudgetSubscriber budget_sub(vehicle, "test/r01", Tier::Monitor, slot);
  tierlink::Uplink uplink(
    vehicle,
    tierlink::UplinkConfig{
      "test/r01",
      Tier::Monitor,
      {{"odom", Tier::Monitor, Delivery::Latest, false, 16}},
      std::chrono::milliseconds(5),
      std::chrono::milliseconds(100),
      std::chrono::seconds(10),
      std::chrono::milliseconds(50),
      4096},
    std::make_unique<tierlink::Budgeted>(
      slot, std::make_unique<tierlink::Fixed>(100'000), std::chrono::milliseconds(500)));
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return got.has_value(); }));
    EXPECT_EQ(got->tier, Tier::Monitor);
  }
  auto budget =
    edge.declare_publisher(zenoh::KeyExpr(tierlink::budget_key("test/r01", Tier::Monitor)));
  for (int i = 0; i < 20 && slot->get() == std::nullopt; ++i) {
    budget.put(zenoh::Bytes(tierlink::encode_bps(700'000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto decision = uplink.status().first;
  ASSERT_TRUE(decision);
  EXPECT_EQ(decision->target_bps, 700'000u);
}
