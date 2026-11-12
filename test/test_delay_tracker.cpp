#include "tierlink/delay_tracker.hpp"

#include <gtest/gtest.h>

#include <chrono>

using tierlink::DelayTracker;
using tierlink::Nanos;

namespace
{

// The receiver clock runs 5 s ahead; only differences matter.
constexpr std::int64_t kOffset = 5'000'000'000;
constexpr std::int64_t ms(std::int64_t v)
{
  return v * 1'000'000;
}

}  // namespace

TEST(DelayTracker, QueueDelayIsOneWayDelayAboveItsMinimum)
{
  DelayTracker t(std::chrono::seconds(10));
  t.on_sent(0, 1000, std::chrono::milliseconds(0));
  t.on_ack(0, kOffset + ms(20), std::chrono::milliseconds(40));
  EXPECT_EQ(t.feedback(std::chrono::milliseconds(40)).queue_delay, Nanos{0});
  t.on_sent(1, 1000, std::chrono::milliseconds(100));
  t.on_ack(1, kOffset + ms(170), std::chrono::milliseconds(190));
  EXPECT_EQ(t.feedback(std::chrono::milliseconds(190)).queue_delay, std::chrono::milliseconds(50));
}

TEST(DelayTracker, AnAckCoversEarlierItems)
{
  DelayTracker t(std::chrono::seconds(10));
  for (int id = 0; id < 3; ++id) {
    t.on_sent(id, 500, id * std::chrono::milliseconds(1));
  }
  EXPECT_EQ(t.feedback(std::chrono::milliseconds(3)).in_flight_bytes, 1500u);
  t.on_ack(1, kOffset + ms(30), std::chrono::milliseconds(40));
  EXPECT_EQ(t.feedback(std::chrono::milliseconds(40)).in_flight_bytes, 500u);
}

TEST(DelayTracker, StallGrowsWhileAcksStop)
{
  DelayTracker t(std::chrono::seconds(10));
  t.on_sent(0, 1000, std::chrono::milliseconds(0));
  t.on_ack(0, kOffset + ms(20), std::chrono::milliseconds(40));
  t.on_sent(1, 1000, std::chrono::milliseconds(100));
  EXPECT_EQ(t.feedback(std::chrono::milliseconds(140)).stall, Nanos{0});
  EXPECT_EQ(t.feedback(std::chrono::milliseconds(640)).stall, std::chrono::milliseconds(500));
  t.on_ack(1, kOffset + ms(640), std::chrono::milliseconds(660));
  EXPECT_EQ(t.feedback(std::chrono::milliseconds(660)).stall, Nanos{0});
}

TEST(DelayTracker, OldMinimumLeavesTheWindow)
{
  DelayTracker t(std::chrono::milliseconds(500));
  t.on_sent(0, 1, std::chrono::milliseconds(0));
  t.on_ack(0, kOffset + ms(10), std::chrono::milliseconds(20));
  t.on_sent(1, 1, std::chrono::milliseconds(1000));
  t.on_ack(1, kOffset + ms(1030), std::chrono::milliseconds(1040));
  EXPECT_EQ(t.feedback(std::chrono::milliseconds(1040)).queue_delay, Nanos{0});
}
