#include "tierlink/delay_tracker.hpp"

#include <gtest/gtest.h>

#include <chrono>

using namespace std::chrono_literals;
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
  DelayTracker t(10s);
  t.on_sent(0, 1000, 0ms);
  t.on_ack(0, kOffset + ms(20), 40ms);
  EXPECT_EQ(t.feedback(40ms).queue_delay, Nanos{0});
  t.on_sent(1, 1000, 100ms);
  t.on_ack(1, kOffset + ms(170), 190ms);
  EXPECT_EQ(t.feedback(190ms).queue_delay, 50ms);
}

TEST(DelayTracker, AnAckCoversEarlierItems)
{
  DelayTracker t(10s);
  for (int id = 0; id < 3; ++id) {
    t.on_sent(id, 500, id * 1ms);
  }
  EXPECT_EQ(t.feedback(3ms).in_flight_bytes, 1500u);
  t.on_ack(1, kOffset + ms(30), 40ms);
  EXPECT_EQ(t.feedback(40ms).in_flight_bytes, 500u);
}

TEST(DelayTracker, StallGrowsWhileAcksStop)
{
  DelayTracker t(10s);
  t.on_sent(0, 1000, 0ms);
  t.on_ack(0, kOffset + ms(20), 40ms);
  t.on_sent(1, 1000, 100ms);
  EXPECT_EQ(t.feedback(140ms).stall, Nanos{0});
  EXPECT_EQ(t.feedback(640ms).stall, 500ms);
  t.on_ack(1, kOffset + ms(640), 660ms);
  EXPECT_EQ(t.feedback(660ms).stall, Nanos{0});
}

TEST(DelayTracker, OldMinimumLeavesTheWindow)
{
  DelayTracker t(500ms);
  t.on_sent(0, 1, 0ms);
  t.on_ack(0, kOffset + ms(10), 20ms);
  t.on_sent(1, 1, 1000ms);
  t.on_ack(1, kOffset + ms(1030), 1040ms);
  EXPECT_EQ(t.feedback(1040ms).queue_delay, Nanos{0});
}
