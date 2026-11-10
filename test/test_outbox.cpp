#include "tierlink/stream.hpp"

#include <gtest/gtest.h>

#include <chrono>

using namespace std::chrono_literals;
using tierlink::Delivery;
using tierlink::Item;
using Nanos = std::chrono::nanoseconds;
using tierlink::Outbox;
using tierlink::Pushed;

namespace
{

Outbox outbox(Delivery delivery, bool inter_frame)
{
  return Outbox(tierlink::StreamSpec{"s", tierlink::Tier::Monitor, delivery, inter_frame, 2});
}

Pushed push(Outbox & o, std::uint8_t byte, bool keyframe, Nanos now = Nanos{0})
{
  return o.push(Item{{byte}, keyframe}, now);
}

int pop(Outbox & o)
{
  auto item = o.pop();
  return item ? item->payload[0] : -1;
}

}  // namespace

TEST(Outbox, LatestKeepsTheNewest)
{
  auto o = outbox(Delivery::Latest, false);
  EXPECT_EQ(push(o, 1, false), Pushed::Queued);
  EXPECT_EQ(push(o, 2, false), Pushed::Replaced);
  EXPECT_EQ(pop(o), 2);
  EXPECT_EQ(pop(o), -1);
}

TEST(Outbox, ReliableKeepsOrderAndDropsTheOldestWhenFull)
{
  auto o = outbox(Delivery::Reliable, false);
  push(o, 1, false);
  push(o, 2, false);
  EXPECT_EQ(push(o, 3, false), Pushed::Replaced);
  EXPECT_EQ(pop(o), 2);
  EXPECT_EQ(pop(o), 3);
}

TEST(Outbox, StatelessFramesReplaceFreely)
{
  auto o = outbox(Delivery::Frame, false);
  push(o, 1, false);
  EXPECT_EQ(push(o, 2, false), Pushed::Replaced);
  EXPECT_EQ(pop(o), 2);
}

TEST(Outbox, ADroppedInterFrameWaitsForAKeyframe)
{
  auto o = outbox(Delivery::Frame, true);
  EXPECT_EQ(push(o, 1, true), Pushed::Queued);
  EXPECT_EQ(push(o, 2, false), Pushed::KeyframeNeeded);
  EXPECT_EQ(pop(o), -1);
  EXPECT_EQ(push(o, 3, false), Pushed::Discarded);
  EXPECT_EQ(push(o, 4, true), Pushed::Queued);
  EXPECT_EQ(push(o, 5, false), Pushed::KeyframeNeeded);
  EXPECT_EQ(push(o, 6, true), Pushed::Queued);
  auto item = o.pop();
  ASSERT_TRUE(item);
  EXPECT_TRUE(item->keyframe);
  EXPECT_EQ(push(o, 7, false), Pushed::Queued);
  EXPECT_EQ(pop(o), 7);
}

TEST(Outbox, InterFramesQueueUpToTheirMaximumDelay)
{
  tierlink::StreamSpec spec{"cam", tierlink::Tier::Teleop, Delivery::Frame, true, 16};
  spec.max_delay = 200ms;
  Outbox o(spec);
  EXPECT_EQ(push(o, 1, true, 0ms), Pushed::Queued);
  EXPECT_EQ(push(o, 2, false, 66ms), Pushed::Queued);
  EXPECT_EQ(push(o, 3, false, 133ms), Pushed::Queued);
  EXPECT_EQ(pop(o), 1);
  EXPECT_EQ(push(o, 4, false, 300ms), Pushed::KeyframeNeeded);
  EXPECT_TRUE(o.empty());
  EXPECT_EQ(push(o, 5, false, 333ms), Pushed::Discarded);
  EXPECT_EQ(push(o, 6, false, 366ms), Pushed::Discarded);
  EXPECT_EQ(push(o, 7, true, 400ms), Pushed::Queued);
  EXPECT_EQ(push(o, 8, false, 433ms), Pushed::Queued);
  EXPECT_EQ(push(o, 9, true, 466ms), Pushed::Replaced);
  EXPECT_EQ(pop(o), 9);
}

TEST(Outbox, StatelessFramesDropOnlyWhatIsTooOld)
{
  tierlink::StreamSpec spec{"pcd", tierlink::Tier::Monitor, Delivery::Frame, false, 16};
  spec.max_delay = 100ms;
  Outbox o(spec);
  push(o, 1, false, 0ms);
  EXPECT_EQ(push(o, 2, false, 50ms), Pushed::Queued);
  EXPECT_EQ(push(o, 3, false, 120ms), Pushed::Replaced);
  EXPECT_EQ(pop(o), 2);
  EXPECT_EQ(pop(o), 3);
}
