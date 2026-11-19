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
  EXPECT_EQ(item->item, (std::vector<std::uint8_t>{1, 2, 3, 4, 5}));
  EXPECT_EQ(r.add(Header{4, 4, 0, false, 0, 1}, {9})->item, std::vector<std::uint8_t>{9});
}

TEST(Wire, ANewerItemDropsAnUnfinishedOne)
{
  tierlink::Reassembler r;
  EXPECT_FALSE(r.add(Header{1, 3, 0, false, 0, 2}, {1}));
  EXPECT_FALSE(r.add(Header{2, 4, 0, false, 0, 2}, {2}));
  EXPECT_FALSE(r.add(Header{3, 3, 0, false, 1, 2}, {1}));
  EXPECT_EQ(r.add(Header{4, 4, 0, false, 1, 2}, {3})->item, (std::vector<std::uint8_t>{2, 3}));
}

namespace
{

// Five pieces of an item in FEC groups of two: parities after pieces 1, 3 and 4.
struct Sent
{
  Header header;
  std::vector<std::uint8_t> payload;
};

std::vector<Sent> with_parity()
{
  const std::vector<std::vector<std::uint8_t>> data{{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12},
                                                    {13}};
  std::vector<Sent> out;
  tierlink::Parity parity;
  std::uint16_t next_parity = 5;
  for (std::uint16_t i = 0; i < data.size(); ++i) {
    out.push_back(Sent{Header{i, 0, 0, false, i, 5, 2}, data[i]});
    parity.add(data[i]);
    if (i % 2 == 1 || i == 4) {
      out.push_back(Sent{Header{i, 0, 0, false, next_parity++, 5, 2}, parity.take()});
    }
  }
  return out;
}

}  // namespace

TEST(Wire, AParityPieceRebuildsAnyOneLostPieceOfItsGroup)
{
  const std::vector<std::uint8_t> whole{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
  const auto sent = with_parity();
  for (std::size_t lost = 0; lost < sent.size(); ++lost) {
    tierlink::Reassembler r;
    std::optional<tierlink::Assembled> got;
    for (std::size_t i = 0; i < sent.size(); ++i) {
      if (i != lost) {
        auto a = r.add(sent[i].header, sent[i].payload);
        got = a ? a : got;
      }
    }
    ASSERT_TRUE(got) << "lost piece " << lost;
    EXPECT_EQ(got->item, whole);
    EXPECT_EQ(got->recovered, sent[lost].header.chunk < 5 ? 1u : 0u);
  }
}

TEST(Wire, AnItemIsDeliveredOnceEvenWhenItsParityComesLast)
{
  tierlink::Parity p;
  p.add({7, 8});
  const auto parity = p.take();
  tierlink::Reassembler r;
  EXPECT_TRUE(r.add(Header{1, 4, 0, false, 0, 1, 4}, {7, 8}));
  EXPECT_FALSE(r.add(Header{2, 4, 0, false, 1, 1, 4}, parity));
  EXPECT_FALSE(r.add(Header{3, 4, 0, false, 0, 1, 4}, {7, 8}));
  EXPECT_TRUE(r.add(Header{4, 5, 0, false, 0, 1, 4}, {9}));
}

TEST(Wire, TwoLostPiecesOfOneGroupLoseTheItem)
{
  const auto sent = with_parity();
  tierlink::Reassembler r;
  for (std::size_t i = 2; i < sent.size(); ++i) {
    EXPECT_FALSE(r.add(sent[i].header, sent[i].payload));
  }
}

TEST(Wire, AParityHeaderRoundTripsAndItsIndexIsChecked)
{
  const Header h{7, 3, -5, false, 7, 5, 2};
  EXPECT_EQ(Header::decode(h.encode()), h);
  EXPECT_FALSE(Header::decode(Header{7, 3, -5, false, 8, 5, 2}.encode()));
}
