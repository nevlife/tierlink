#include "tierlink/rate_controller.hpp"

#include <gtest/gtest.h>

#include <chrono>

using tierlink::Aimd;
using tierlink::Feedback;
using tierlink::Nanos;

namespace
{

Aimd aimd()
{
  return Aimd(tierlink::AimdConfig{
    1'000'000, 300'000, 2'000'000, 100'000, 0.8, std::chrono::milliseconds(300),
    std::chrono::seconds(1), std::chrono::milliseconds(100), std::chrono::milliseconds(30)});
}

Feedback delay(Nanos d)
{
  return Feedback{d, Nanos{0}, 0};
}

std::uint64_t rate(Aimd & a, Nanos at, const Feedback & f)
{
  return a.update(at, f).target_bps;
}

}  // namespace

TEST(Aimd, CutsOncePerHold)
{
  auto a = aimd();
  EXPECT_EQ(rate(a, std::chrono::milliseconds(0), delay(std::chrono::milliseconds(150))), 800'000u);
  EXPECT_EQ(
    rate(a, std::chrono::milliseconds(100), delay(std::chrono::milliseconds(150))), 800'000u);
  EXPECT_EQ(
    rate(a, std::chrono::milliseconds(300), delay(std::chrono::milliseconds(150))), 640'000u);
}

TEST(Aimd, AStallCountsAsCongestion)
{
  auto a = aimd();
  EXPECT_EQ(
    rate(a, std::chrono::milliseconds(0), Feedback{Nanos{0}, std::chrono::milliseconds(400), 0}),
    800'000u);
}

TEST(Aimd, IncreasesAfterEachClearProbe)
{
  auto a = aimd();
  EXPECT_EQ(
    rate(a, std::chrono::milliseconds(0), delay(std::chrono::milliseconds(10))), 1'000'000u);
  EXPECT_EQ(
    rate(a, std::chrono::milliseconds(999), delay(std::chrono::milliseconds(10))), 1'000'000u);
  EXPECT_EQ(
    rate(a, std::chrono::milliseconds(1000), delay(std::chrono::milliseconds(10))), 1'100'000u);
  EXPECT_EQ(
    rate(a, std::chrono::milliseconds(1500), delay(std::chrono::milliseconds(50))), 1'100'000u);
  EXPECT_EQ(
    rate(a, std::chrono::milliseconds(2000), delay(std::chrono::milliseconds(10))), 1'100'000u);
  EXPECT_EQ(
    rate(a, std::chrono::milliseconds(3000), delay(std::chrono::milliseconds(10))), 1'200'000u);
}

TEST(Aimd, StaysWithinBounds)
{
  auto a = aimd();
  for (int i = 0; i < 20; ++i) {
    rate(a, i * std::chrono::milliseconds(300), delay(std::chrono::milliseconds(500)));
  }
  EXPECT_EQ(
    rate(a, std::chrono::milliseconds(6000), delay(std::chrono::milliseconds(500))), 300'000u);
  for (int i = 0; i < 30; ++i) {
    rate(
      a, std::chrono::milliseconds(7000) + i * std::chrono::milliseconds(1000),
      delay(std::chrono::milliseconds(0)));
  }
  EXPECT_EQ(
    rate(a, std::chrono::milliseconds(40000), delay(std::chrono::milliseconds(0))), 2'000'000u);
}

TEST(Fixed, IgnoresFeedback)
{
  tierlink::Fixed f(5'000);
  EXPECT_EQ(
    f.update(std::chrono::milliseconds(0), delay(std::chrono::seconds(1))).target_bps, 5'000u);
}
