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

namespace
{

constexpr tierlink::LedbatConfig kLedbat{
  std::chrono::milliseconds(25), 1.0, 1000, 2000, 10'000, 100'000, 5'000'000,
  std::chrono::milliseconds(100), std::chrono::milliseconds(500)};

Feedback acked(std::uint64_t total, std::uint64_t in_flight, int delay_ms, std::uint64_t lost = 0)
{
  Feedback f;
  f.acked_bytes = total;
  f.in_flight_bytes = in_flight;
  f.queue_delay = std::chrono::milliseconds(delay_ms);
  f.lost_bytes = lost;
  return f;
}

}  // namespace

TEST(Ledbat, GrowsBelowTheTargetAndShrinksAboveIt)
{
  tierlink::Ledbat l(kLedbat);
  // A full window acknowledged with no queue: one segment more.
  auto d = l.update(Nanos{0}, acked(10'000, 10'000, 0));
  EXPECT_EQ(d.max_in_flight_bytes, 11'000u);
  // Twice the target: one segment less per window.
  d = l.update(std::chrono::milliseconds(10), acked(21'000, 11'000, 50));
  EXPECT_EQ(d.max_in_flight_bytes, 10'000u);
}

TEST(Ledbat, LossHalvesOncePerHold)
{
  tierlink::Ledbat l(kLedbat);
  EXPECT_EQ(l.update(Nanos{0}, acked(0, 10'000, 0, 1000)).max_in_flight_bytes, 5000u);
  EXPECT_EQ(
    l.update(std::chrono::milliseconds(50), acked(0, 10'000, 0, 2000)).max_in_flight_bytes, 5000u);
  EXPECT_EQ(
    l.update(std::chrono::milliseconds(100), acked(0, 10'000, 0, 3000)).max_in_flight_bytes, 2500u);
}

TEST(Ledbat, AnAppLimitedSenderDoesNotGrowItsWindow)
{
  tierlink::Ledbat l(kLedbat);
  EXPECT_EQ(l.update(Nanos{0}, acked(1000, 0, 0)).max_in_flight_bytes, 2000u);
}
