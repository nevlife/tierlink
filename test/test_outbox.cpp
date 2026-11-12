#include "tierlink/stream.hpp"

#include <gtest/gtest.h>

#include <chrono>

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
  spec.max_delay = std::chrono::milliseconds(200);
  Outbox o(spec);
  EXPECT_EQ(push(o, 1, true, std::chrono::milliseconds(0)), Pushed::Queued);
  EXPECT_EQ(push(o, 2, false, std::chrono::milliseconds(66)), Pushed::Queued);
  EXPECT_EQ(push(o, 3, false, std::chrono::milliseconds(133)), Pushed::Queued);
  EXPECT_EQ(pop(o), 1);
  EXPECT_EQ(push(o, 4, false, std::chrono::milliseconds(300)), Pushed::KeyframeNeeded);
  EXPECT_TRUE(o.empty());
  EXPECT_EQ(push(o, 5, false, std::chrono::milliseconds(333)), Pushed::Discarded);
  EXPECT_EQ(push(o, 6, false, std::chrono::milliseconds(366)), Pushed::Discarded);
  EXPECT_EQ(push(o, 7, true, std::chrono::milliseconds(400)), Pushed::Queued);
  EXPECT_EQ(push(o, 8, false, std::chrono::milliseconds(433)), Pushed::Queued);
  EXPECT_EQ(push(o, 9, true, std::chrono::milliseconds(466)), Pushed::Replaced);
  EXPECT_EQ(pop(o), 9);
}

TEST(Outbox, StatelessFramesDropOnlyWhatIsTooOld)
{
  tierlink::StreamSpec spec{"pcd", tierlink::Tier::Monitor, Delivery::Frame, false, 16};
  spec.max_delay = std::chrono::milliseconds(100);
  Outbox o(spec);
  push(o, 1, false, std::chrono::milliseconds(0));
  EXPECT_EQ(push(o, 2, false, std::chrono::milliseconds(50)), Pushed::Queued);
  EXPECT_EQ(push(o, 3, false, std::chrono::milliseconds(120)), Pushed::Replaced);
  EXPECT_EQ(pop(o), 2);
  EXPECT_EQ(pop(o), 3);
}
