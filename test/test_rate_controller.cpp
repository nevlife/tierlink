#include "tierlink/rate_controller.hpp"

#include <gtest/gtest.h>

#include <chrono>

using namespace std::chrono_literals;
using tierlink::Aimd;
using tierlink::Feedback;
using tierlink::Nanos;

namespace
{

Aimd aimd()
{
  return Aimd(
    tierlink::AimdConfig{1'000'000, 300'000, 2'000'000, 100'000, 0.8, 300ms, 1s, 100ms, 30ms});
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
  EXPECT_EQ(rate(a, 0ms, delay(150ms)), 800'000u);
  EXPECT_EQ(rate(a, 100ms, delay(150ms)), 800'000u);
  EXPECT_EQ(rate(a, 300ms, delay(150ms)), 640'000u);
}

TEST(Aimd, AStallCountsAsCongestion)
{
  auto a = aimd();
  EXPECT_EQ(rate(a, 0ms, Feedback{Nanos{0}, 400ms, 0}), 800'000u);
}

TEST(Aimd, IncreasesAfterEachClearProbe)
{
  auto a = aimd();
  EXPECT_EQ(rate(a, 0ms, delay(10ms)), 1'000'000u);
  EXPECT_EQ(rate(a, 999ms, delay(10ms)), 1'000'000u);
  EXPECT_EQ(rate(a, 1000ms, delay(10ms)), 1'100'000u);
  EXPECT_EQ(rate(a, 1500ms, delay(50ms)), 1'100'000u);
  EXPECT_EQ(rate(a, 2000ms, delay(10ms)), 1'100'000u);
  EXPECT_EQ(rate(a, 3000ms, delay(10ms)), 1'200'000u);
}

TEST(Aimd, StaysWithinBounds)
{
  auto a = aimd();
  for (int i = 0; i < 20; ++i) {
    rate(a, i * 300ms, delay(500ms));
  }
  EXPECT_EQ(rate(a, 6000ms, delay(500ms)), 300'000u);
  for (int i = 0; i < 30; ++i) {
    rate(a, 7000ms + i * 1000ms, delay(0ms));
  }
  EXPECT_EQ(rate(a, 40000ms, delay(0ms)), 2'000'000u);
}

TEST(Fixed, IgnoresFeedback)
{
  tierlink::Fixed f(5'000);
  EXPECT_EQ(f.update(0ms, delay(1s)).target_bps, 5'000u);
}
