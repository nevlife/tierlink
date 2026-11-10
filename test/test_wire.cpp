#include "tierlink/wire.hpp"

#include <gtest/gtest.h>

#include <chrono>

using namespace std::chrono_literals;
using tierlink::GapTracker;
using tierlink::Header;

TEST(Wire, HeaderAndAckRoundTrip)
{
  const Header h{42, 5, -7, true};
  EXPECT_EQ(Header::decode(h.encode()), h);
  EXPECT_EQ(tierlink::decode_ack(tierlink::encode_ack(9, 123)), std::make_pair(9ul, 123l));
  EXPECT_FALSE(Header::decode({0, 0, 0}));
}

TEST(GapTracker, InOrderArrivalsRequestNothing)
{
  GapTracker g(250ms);
  EXPECT_EQ(g.on_arrival(0, true, 0ms).lost, 0u);
  const auto a = g.on_arrival(1, false, 33ms);
  EXPECT_FALSE(a.request_keyframe);
  EXPECT_EQ(a.lost, 0u);
}

TEST(GapTracker, AGapRequestsUntilAKeyframeArrives)
{
  GapTracker g(250ms);
  g.on_arrival(0, true, 0ms);
  const auto a = g.on_arrival(3, false, 100ms);
  EXPECT_EQ(a.lost, 2u);
  EXPECT_TRUE(a.request_keyframe);
  EXPECT_FALSE(g.on_arrival(4, false, 200ms).request_keyframe);
  EXPECT_TRUE(g.on_arrival(5, false, 350ms).request_keyframe);
  EXPECT_FALSE(g.on_arrival(6, true, 400ms).request_keyframe);
  EXPECT_FALSE(g.on_arrival(7, false, 800ms).request_keyframe);
}
