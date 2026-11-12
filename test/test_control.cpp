#include "tierlink/control.hpp"
#include "tierlink/zenoh_link.hpp"

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

}  // namespace

TEST(EpochCounter, OnlyGrows)
{
  tierlink::EpochCounter c;
  EXPECT_EQ(c.next(1000), 1000u);
  EXPECT_EQ(c.next(1000), 1001u);
  EXPECT_EQ(c.next(900), 1002u);
  EXPECT_EQ(c.next(5000), 5000u);
}

TEST(Control, MessagesReachTheOtherClientThroughTheRouterAndEpochsGrow)
{
  const std::string endpoint = "tcp/127.0.0.1:" + std::to_string(free_port());
  auto router = zenoh::Session::open(tierlink::make_config("router", {}, {endpoint}, std::nullopt));
  auto vehicle =
    zenoh::Session::open(tierlink::make_config("client", {endpoint}, {}, std::nullopt));
  auto operator_side =
    zenoh::Session::open(tierlink::make_config("client", {endpoint}, {}, std::nullopt));
  tierlink::EpochIssuer issuer(router, "test/r01");

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::vector<std::uint8_t>> got;
  tierlink::ControlSubscriber sub(
    vehicle, "test/r01", Tier::Teleop, "motion", [&](const std::vector<std::uint8_t> & m) {
      std::lock_guard<std::mutex> lock(mutex);
      got.push_back(m);
      cv.notify_one();
    });
  tierlink::ControlPublisher pub(operator_side, "test/r01", Tier::Teleop, "motion");
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  pub.send({1, 2, 3});
  std::unique_lock<std::mutex> lock(mutex);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return !got.empty(); }));
  EXPECT_EQ(got[0], (std::vector<std::uint8_t>{1, 2, 3}));
  lock.unlock();

  const auto a = tierlink::request_epoch(operator_side, "test/r01", 2000);
  const auto b = tierlink::request_epoch(operator_side, "test/r01", 2000);
  ASSERT_TRUE(a && b);
  EXPECT_LT(a->epoch, b->epoch);
  EXPECT_NE(a->session_id, b->session_id);
  EXPECT_EQ(a->session_id.size(), 32u);
  EXPECT_FALSE(tierlink::request_epoch(operator_side, "test/none", 300));
}
