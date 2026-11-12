#include "tierlink/wire.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <utility>
#include <vector>

using tierlink::GapTracker;
using tierlink::Header;

TEST(Wire, HeaderAndAckRoundTrip)
{
  const Header h{42, 5, -7, true};
  EXPECT_EQ(Header::decode(h.encode()), h);
  EXPECT_EQ(tierlink::decode_ack(tierlink::encode_ack(9, 123)), std::make_pair(9ul, 123l));
  EXPECT_FALSE(Header::decode({0, 0, 0}));
  EXPECT_EQ(tierlink::decode_bps(tierlink::encode_bps(1'234'567)), 1'234'567u);
  EXPECT_FALSE(tierlink::decode_bps({1, 2}));
}

TEST(GapTracker, InOrderArrivalsRequestNothing)
{
  GapTracker g(std::chrono::milliseconds(250));
  EXPECT_EQ(g.on_arrival(0, true, std::chrono::milliseconds(0)).lost, 0u);
  const auto a = g.on_arrival(1, false, std::chrono::milliseconds(33));
  EXPECT_FALSE(a.request_keyframe);
  EXPECT_EQ(a.lost, 0u);
}

TEST(GapTracker, AGapRequestsUntilAKeyframeArrives)
{
  GapTracker g(std::chrono::milliseconds(250));
  g.on_arrival(0, true, std::chrono::milliseconds(0));
  const auto a = g.on_arrival(3, false, std::chrono::milliseconds(100));
  EXPECT_EQ(a.lost, 2u);
  EXPECT_TRUE(a.request_keyframe);
  EXPECT_FALSE(g.on_arrival(4, false, std::chrono::milliseconds(200)).request_keyframe);
  EXPECT_TRUE(g.on_arrival(5, false, std::chrono::milliseconds(350)).request_keyframe);
  EXPECT_FALSE(g.on_arrival(6, true, std::chrono::milliseconds(400)).request_keyframe);
  EXPECT_FALSE(g.on_arrival(7, false, std::chrono::milliseconds(800)).request_keyframe);
}

TEST(Wire, PiecesRoundTripAndReassembleInAnyOrder)
{
  const Header h{7, 3, -5, true, 1, 3};
  EXPECT_EQ(Header::decode(h.encode()), h);
  EXPECT_FALSE(Header::decode(Header{7, 3, -5, true, 3, 3}.encode()));
  tierlink::Reassembler r;
  EXPECT_FALSE(r.add(Header{1, 3, 0, false, 2, 3}, {5}));
  EXPECT_FALSE(r.add(Header{2, 3, 0, false, 0, 3}, {1, 2}));
  const auto item = r.add(Header{3, 3, 0, false, 1, 3}, {3, 4});
  ASSERT_TRUE(item);
  EXPECT_EQ(*item, (std::vector<std::uint8_t>{1, 2, 3, 4, 5}));
  EXPECT_EQ(*r.add(Header{4, 4, 0, false, 0, 1}, {9}), std::vector<std::uint8_t>{9});
}

TEST(Wire, ANewerItemDropsAnUnfinishedOne)
{
  tierlink::Reassembler r;
  EXPECT_FALSE(r.add(Header{1, 3, 0, false, 0, 2}, {1}));
  EXPECT_FALSE(r.add(Header{2, 4, 0, false, 0, 2}, {2}));
  EXPECT_FALSE(r.add(Header{3, 3, 0, false, 1, 2}, {1}));
  EXPECT_EQ(*r.add(Header{4, 4, 0, false, 1, 2}, {3}), (std::vector<std::uint8_t>{2, 3}));
}
