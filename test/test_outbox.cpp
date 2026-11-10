#include "tierlink/stream.hpp"

#include <gtest/gtest.h>

using tierlink::Delivery;
using tierlink::Item;
using tierlink::Outbox;
using tierlink::Pushed;

namespace
{

Outbox outbox(Delivery delivery, bool inter_frame)
{
  return Outbox(tierlink::StreamSpec{"s", tierlink::Tier::Monitor, delivery, inter_frame, 2});
}

Pushed push(Outbox & o, std::uint8_t byte, bool keyframe)
{
  return o.push(Item{{byte}, keyframe});
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
