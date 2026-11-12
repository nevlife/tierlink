#include "tierlink/scheduler.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

using tierlink::Item;
using tierlink::Scheduler;
using tierlink::Tier;

namespace
{

tierlink::StreamSpec spec(const char * name, Tier tier)
{
  return tierlink::StreamSpec{name, tier, tierlink::Delivery::Reliable, false, 16};
}

Scheduler scheduler(std::uint64_t bps)
{
  return Scheduler(
    {spec("odom", Tier::Monitor), spec("video", Tier::Teleop)}, bps,
    std::chrono::milliseconds(100));
}

Item item(std::uint8_t byte, std::size_t len)
{
  return Item{std::vector<std::uint8_t>(len, byte), false};
}

}  // namespace

TEST(Scheduler, TeleopGoesBeforeMonitor)
{
  auto s = scheduler(1'000'000);
  s.next(std::chrono::milliseconds(0));
  s.push(0, item(0, 10), std::chrono::milliseconds(0));
  s.push(1, item(1, 10), std::chrono::milliseconds(0));
  EXPECT_EQ(s.next(std::chrono::milliseconds(10))->first, 1u);
  EXPECT_EQ(s.next(std::chrono::milliseconds(10))->first, 0u);
  EXPECT_FALSE(s.next(std::chrono::milliseconds(10)));
}

TEST(Scheduler, SpendingPastTheBudgetWaitsForRefill)
{
  // 80 kbit/s: one 1000-byte item costs 100 ms of budget.
  auto s = scheduler(80'000);
  s.next(std::chrono::milliseconds(0));
  for (int i = 0; i < 3; ++i) {
    s.push(0, item(0, 1000), std::chrono::milliseconds(0));
  }
  EXPECT_TRUE(s.next(std::chrono::milliseconds(50)));
  EXPECT_FALSE(s.next(std::chrono::milliseconds(60)));
  EXPECT_TRUE(s.next(std::chrono::milliseconds(150)));
}

TEST(Scheduler, IdleTimeCarriesOverOnlyOneBurst)
{
  auto s = scheduler(80'000);
  s.next(std::chrono::milliseconds(0));
  for (int i = 0; i < 3; ++i) {
    s.push(0, item(0, 1000), std::chrono::milliseconds(0));
  }
  EXPECT_TRUE(s.next(std::chrono::seconds(10)));
  EXPECT_FALSE(s.next(std::chrono::seconds(10)));
}

TEST(Scheduler, AZeroBudgetSendsNothing)
{
  auto s = scheduler(0);
  s.push(1, item(1, 1), std::chrono::milliseconds(0));
  EXPECT_FALSE(s.next(std::chrono::milliseconds(1)));
}
