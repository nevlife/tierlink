#include "tierlink/scheduler.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

using namespace std::chrono_literals;
using tierlink::Decision;
using tierlink::Feedback;
using tierlink::Item;
using tierlink::Nanos;
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
  return Scheduler({spec("odom", Tier::Monitor), spec("video", Tier::Teleop)}, bps, 100ms);
}

Item item(std::uint8_t byte, std::size_t len)
{
  return Item{std::vector<std::uint8_t>(len, byte), false};
}

Decision link(std::uint64_t bps)
{
  return Decision{bps, std::nullopt};
}

Feedback delay(Nanos d)
{
  return Feedback{d, Nanos{0}, 0};
}

}  // namespace

TEST(Scheduler, TeleopGoesBeforeMonitor)
{
  auto s = scheduler(1'000'000);
  s.next(0ms);
  s.push(0, item(0, 10), 0ms);
  s.push(1, item(1, 10), 0ms);
  EXPECT_EQ(s.next(10ms)->first, 1u);
  EXPECT_EQ(s.next(10ms)->first, 0u);
  EXPECT_FALSE(s.next(10ms));
}

TEST(Scheduler, SpendingPastTheBudgetWaitsForRefill)
{
  // 80 kbit/s: one 1000-byte item costs 100 ms of budget.
  auto s = scheduler(80'000);
  s.next(0ms);
  for (int i = 0; i < 3; ++i) {
    s.push(0, item(0, 1000), 0ms);
  }
  EXPECT_TRUE(s.next(50ms));
  EXPECT_FALSE(s.next(60ms));
  EXPECT_TRUE(s.next(150ms));
}

TEST(Scheduler, IdleTimeCarriesOverOnlyOneBurst)
{
  auto s = scheduler(80'000);
  s.next(0ms);
  for (int i = 0; i < 3; ++i) {
    s.push(0, item(0, 1000), 0ms);
  }
  EXPECT_TRUE(s.next(10s));
  EXPECT_FALSE(s.next(10s));
}

TEST(Scheduler, AZeroBudgetSendsNothing)
{
  auto s = scheduler(0);
  s.push(1, item(1, 1), 0ms);
  EXPECT_FALSE(s.next(1ms));
}

TEST(TierPolicy, MonitoringGetsAShareOfWhatTeleopLeaves)
{
  const tierlink::TierPolicy p{0.1, 50ms};
  EXPECT_EQ(p.monitor_allowance(link(1'000'000), 500'000, delay(0ms)), 100'000u);
  EXPECT_EQ(p.monitor_allowance(link(1'000'000), 950'000, delay(0ms)), 50'000u);
  EXPECT_EQ(p.monitor_allowance(link(1'000'000), 0, delay(80ms)), 0u);
}

TEST(Allowance, AnExpiredAllowanceFallsBackToTheOwnController)
{
  tierlink::Allowance a(500ms);
  EXPECT_FALSE(a.cap(0ms));
  a.update(100'000, 0ms);
  EXPECT_EQ(a.cap(400ms), 100'000u);
  EXPECT_FALSE(a.cap(600ms));
}
