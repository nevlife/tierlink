#include "tierlink/trendline.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>

using tierlink::Trendline;
using tierlink::Usage;

namespace
{

constexpr std::int64_t kMs = 1'000'000;
// The receiver clock is unrelated to the sender's.
constexpr std::int64_t kOffset = 123'456 * kMs;

// One packet every 10 ms for `count` packets; each arrives `growth_ms` later than the one before
// on top of a 30 ms path.
Usage run(Trendline & t, int first, int count, double growth_ms, double & queue_ms)
{
  Usage u = Usage::Normal;
  for (int i = first; i < first + count; ++i) {
    queue_ms = std::max(0.0, queue_ms + growth_ms);
    const std::int64_t sent = i * 10 * kMs;
    u = t.on_packet(sent, kOffset + sent + 30 * kMs + static_cast<std::int64_t>(queue_ms * kMs));
  }
  return u;
}

}  // namespace

TEST(Trendline, AConstantDelayIsNormal)
{
  Trendline t;
  double q = 0.0;
  EXPECT_EQ(run(t, 0, 200, 0.0, q), Usage::Normal);
}

TEST(Trendline, AGrowingQueueIsOveruseWithinAFewHundredMilliseconds)
{
  Trendline t;
  double q = 0.0;
  run(t, 0, 100, 0.0, q);
  // 1 ms more per 10 ms: the link carries 10 % less than is sent.
  int packets = 0;
  while (t.usage() != Usage::Over && packets < 100) {
    run(t, 100 + packets, 1, 1.0, q);
    ++packets;
  }
  EXPECT_EQ(t.usage(), Usage::Over);
  EXPECT_LT(q, 40.0);
}

TEST(Trendline, ADrainingQueueIsUnderuse)
{
  Trendline t;
  double q = 200.0;
  run(t, 0, 50, 0.0, q);
  EXPECT_EQ(run(t, 50, 40, -2.0, q), Usage::Under);
}
