#include "tierlink/teleop_messages.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace teleop = tierlink::teleop;

TEST(TeleopMessages, RoundTripAndRefuseTruncatedOrForeignVersions)
{
  const teleop::MotionCommand m{
    "r01", "s1", 10, 3, teleop::UnixNs{-5}, std::chrono::milliseconds(200), 0.5, -0.25, true};
  const auto back = teleop::decode_motion(teleop::encode(m));
  ASSERT_TRUE(back);
  EXPECT_EQ(back->session_id, "s1");
  EXPECT_EQ(back->sequence, 3u);
  EXPECT_EQ(back->sent.ns, -5);
  EXPECT_EQ(back->valid_for, std::chrono::milliseconds(200));
  EXPECT_EQ(back->angular_rps, -0.25);
  auto bytes = teleop::encode(m);
  bytes.pop_back();
  EXPECT_FALSE(teleop::decode_motion(bytes));
  bytes = teleop::encode(m);
  bytes[0] = 99;
  EXPECT_FALSE(teleop::decode_motion(bytes));
  EXPECT_FALSE(teleop::decode_arm(teleop::encode(m)));
}

TEST(TeleopMessages, StateAndHopRoundTrip)
{
  const auto s = teleop::decode_state(teleop::encode(teleop::ArmState{"r01", "s1", 7, true, ""}));
  ASSERT_TRUE(s);
  EXPECT_TRUE(s->armed);
  EXPECT_EQ(s->epoch, 7u);
  const auto rtt = teleop::decode_hop(teleop::encode_hop(std::chrono::milliseconds(42)));
  ASSERT_TRUE(rtt);
  EXPECT_EQ(*rtt, std::chrono::milliseconds(42));
}

// The bytes that operator/src/wire.rs writes and reads, so the two copies cannot drift apart.
TEST(TeleopMessages, LayoutMatchesTheOperator)
{
  const std::vector<std::uint8_t> arm{1, 3, 0, 'r', '0', '1', 1, 0, 's', 7, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(teleop::encode(teleop::ArmRequest{"r01", "s", 7}), arm);
  const std::vector<std::uint8_t> state{1, 3, 0, 'r', '0', '1', 1, 0, 's', 7, 0, 0, 0,
                                        0, 0, 0, 0, 1, 0, 0};
  EXPECT_EQ(teleop::encode(teleop::ArmState{"r01", "s", 7, true, ""}), state);
}
