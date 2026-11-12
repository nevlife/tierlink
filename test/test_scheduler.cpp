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
    {spec("odom", Tier::Monitor), spec("video", Tier::Teleop)}, bps, std::chrono::milliseconds(100),
    4096);
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
  EXPECT_EQ(s.next(std::chrono::milliseconds(10))->id, 1u);
  EXPECT_EQ(s.next(std::chrono::milliseconds(10))->id, 0u);
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

TEST(Scheduler, ALargeItemGoesOutInPiecesAndTeleopGoesBetweenThem)
{
  auto s = scheduler(1'000'000);
  s.next(std::chrono::milliseconds(0));
  s.push(0, item(7, 10'000), std::chrono::milliseconds(0));
  auto p = s.next(std::chrono::milliseconds(100));
  ASSERT_TRUE(p);
  EXPECT_EQ(p->id, 0u);
  EXPECT_EQ(p->chunk, 0u);
  EXPECT_EQ(p->chunks, 3u);
  EXPECT_EQ(p->payload.size(), 4096u);
  s.push(1, item(1, 100), std::chrono::milliseconds(100));
  p = s.next(std::chrono::milliseconds(100));
  ASSERT_TRUE(p);
  EXPECT_EQ(p->id, 1u);
  EXPECT_EQ(p->chunks, 1u);
  p = s.next(std::chrono::milliseconds(100));
  ASSERT_TRUE(p);
  EXPECT_EQ(p->chunk, 1u);
  p = s.next(std::chrono::milliseconds(100));
  ASSERT_TRUE(p);
  EXPECT_EQ(p->chunk, 2u);
  EXPECT_EQ(p->payload.size(), 10'000u - 2 * 4096u);
  EXPECT_FALSE(s.next(std::chrono::milliseconds(100)));
}
