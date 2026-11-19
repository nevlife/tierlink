#include "tierlink/link_manager.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <utility>

using std::chrono::milliseconds;
using tierlink::Budgeted;
using tierlink::BudgetSlot;
using tierlink::LinkManager;
using tierlink::LinkReport;
using tierlink::Nanos;
using tierlink::Tier;

namespace
{

constexpr tierlink::AimdLinkConfig kAimd{
  1'000'000,        500'000,           4'000'000,         0.85,
  0.1,              100'000,           milliseconds(300), milliseconds(1000),
  milliseconds(100), milliseconds(30)};

constexpr tierlink::SplitConfig kSplit{
  milliseconds(50), 150'000, 2'000'000, 30'000, milliseconds(500), milliseconds(3000)};

constexpr tierlink::GccLinkConfig kGcc{
  1'000'000, 150'000, 4'000'000, 0.85, 0.08, 60'000, milliseconds(300), milliseconds(100),
  0.10,      0.02};

LinkManager aimd(tierlink::AimdLinkConfig config = kAimd)
{
  return LinkManager(kSplit, std::make_unique<tierlink::AimdLink>(config));
}

LinkManager gcc()
{
  return LinkManager(kSplit, std::make_unique<tierlink::GccLink>(kGcc));
}

LinkReport report(Tier tier, std::uint64_t sent, std::uint64_t acked, milliseconds delay)
{
  return LinkReport{tier, sent, acked, delay, Nanos{0}};
}

class Constant : public tierlink::RateController
{
public:
  explicit Constant(std::uint64_t bps) : bps_(bps) {}
  tierlink::Decision update(Nanos, const tierlink::Feedback &) override
  {
    return tierlink::Decision{bps_, std::nullopt};
  }

private:
  std::uint64_t bps_;
};

}  // namespace

TEST(LinkReport, RoundTripsAndRefusesOtherSizes)
{
  const LinkReport r{
    Tier::Monitor, 1, 2, milliseconds(3), milliseconds(4), 5, tierlink::Usage::Over};
  const auto bytes = tierlink::encode_report(r);
  const auto back = tierlink::decode_report(bytes);
  ASSERT_TRUE(back);
  EXPECT_EQ(back->tier, Tier::Monitor);
  EXPECT_EQ(back->acked_bps, 2u);
  EXPECT_EQ(back->stall, milliseconds(4));
  EXPECT_EQ(back->lost_bps, 5u);
  EXPECT_EQ(back->usage, tierlink::Usage::Over);
  EXPECT_FALSE(tierlink::decode_report({bytes.begin(), bytes.end() - 1}));
}

TEST(LinkManager, CongestionCutsToWhatWasDeliveredOncePerHold)
{
  LinkManager m = aimd();
  m.on_report(report(Tier::Teleop, 900'000, 800'000, milliseconds(150)), milliseconds(0));
  EXPECT_EQ(m.update(milliseconds(0)).link_bps, 680'000u);
  EXPECT_EQ(m.update(milliseconds(100)).link_bps, 680'000u);
  m.on_report(report(Tier::Teleop, 600'000, 600'000, milliseconds(150)), milliseconds(300));
  EXPECT_EQ(m.update(milliseconds(300)).link_bps, 510'000u);
}

TEST(LinkManager, GrowsOnlyWhileTheRateIsUsed)
{
  LinkManager m = aimd();
  m.on_report(report(Tier::Monitor, 100'000, 100'000, milliseconds(5)), milliseconds(0));
  m.update(milliseconds(0));
  m.on_report(report(Tier::Monitor, 100'000, 100'000, milliseconds(5)), milliseconds(1000));
  EXPECT_EQ(m.update(milliseconds(1000)).link_bps, 1'000'000u);
  m.on_report(report(Tier::Monitor, 900'000, 900'000, milliseconds(5)), milliseconds(1100));
  EXPECT_EQ(m.update(milliseconds(1100)).link_bps, 1'100'000u);
}

TEST(LinkManager, TeleopTakesWhatItNeedsAndMonitoringKeepsItsFloor)
{
  LinkManager m = aimd();
  m.on_report(report(Tier::Teleop, 800'000, 800'000, milliseconds(40)), milliseconds(0));
  m.on_report(report(Tier::Monitor, 50'000, 50'000, milliseconds(40)), milliseconds(0));
  auto b = m.update(milliseconds(0));
  EXPECT_EQ(b.tier_bps[0], 970'000u);
  EXPECT_EQ(b.tier_bps[1], 30'000u);
  m.on_report(report(Tier::Teleop, 800'000, 800'000, milliseconds(60)), milliseconds(100));
  b = m.update(milliseconds(100));
  EXPECT_EQ(b.tier_bps[1], 30'000u);
}

TEST(LinkManager, MonitoringGetsTheWholeLinkWithoutTeleop)
{
  LinkManager m = aimd();
  m.on_report(report(Tier::Monitor, 50'000, 50'000, milliseconds(5)), milliseconds(0));
  const auto b = m.update(milliseconds(0));
  EXPECT_EQ(b.tier_bps[1], 1'000'000u);
}

TEST(LinkManager, AStallNobodyAcknowledgedIsIgnoredButAnOutageCuts)
{
  LinkManager m = aimd();
  // Video with no operator: sent, never acknowledged, its stall grows.
  m.on_report(LinkReport{Tier::Teleop, 900'000, 0, Nanos{0}, milliseconds(2000)}, milliseconds(0));
  EXPECT_EQ(m.update(milliseconds(0)).link_bps, 1'000'000u);
  // The operator watched, then the link went silent.
  m.on_report(report(Tier::Teleop, 900'000, 900'000, milliseconds(5)), milliseconds(100));
  m.on_report(LinkReport{Tier::Teleop, 900'000, 0, Nanos{0}, milliseconds(400)}, milliseconds(400));
  EXPECT_EQ(m.update(milliseconds(400)).link_bps, 850'000u);
}

TEST(Budgeted, FollowsAFreshBudgetAndFallsBackOnceItExpires)
{
  auto slot = std::make_shared<BudgetSlot>();
  Budgeted b(slot, std::make_unique<Constant>(300'000), milliseconds(500));
  EXPECT_EQ(b.update(milliseconds(0), {}).target_bps, 300'000u);
  slot->set(700'000, milliseconds(0));
  EXPECT_EQ(b.update(milliseconds(400), {}).target_bps, 700'000u);
  EXPECT_EQ(b.update(milliseconds(600), {}).target_bps, 300'000u);
}

TEST(LinkManager, GrowsByAFractionOfTheRateOnceThatExceedsTheStep)
{
  auto config = kAimd;
  config.start_bps = 2'000'000;
  LinkManager m = aimd(config);
  m.on_report(report(Tier::Monitor, 1'900'000, 1'900'000, milliseconds(5)), milliseconds(0));
  m.update(milliseconds(0));
  m.on_report(report(Tier::Monitor, 1'900'000, 1'900'000, milliseconds(5)), milliseconds(1000));
  EXPECT_EQ(m.update(milliseconds(1000)).link_bps, 2'200'000u);
}

TEST(Budgeted, KeepsTheWindowOfTheOwnController)
{
  class Windowed : public tierlink::RateController
  {
  public:
    tierlink::Decision update(Nanos, const tierlink::Feedback &) override
    {
      return tierlink::Decision{300'000, 8000};
    }
  };
  auto slot = std::make_shared<BudgetSlot>();
  Budgeted b(slot, std::make_unique<Windowed>(), milliseconds(500));
  slot->set(700'000, milliseconds(0));
  const auto d = b.update(milliseconds(100), {});
  EXPECT_EQ(d.target_bps, 700'000u);
  EXPECT_EQ(d.max_in_flight_bytes, 8000u);
}

namespace
{

LinkReport usage(Tier tier, std::uint64_t acked, tierlink::Usage u, std::uint64_t lost = 0)
{
  return LinkReport{tier, acked, acked, milliseconds(5), Nanos{0}, lost, u};
}

}  // namespace

TEST(GccLink, OveruseCutsToAFractionOfWhatWasDelivered)
{
  LinkManager m = gcc();
  m.on_report(usage(Tier::Teleop, 800'000, tierlink::Usage::Over), milliseconds(0));
  EXPECT_EQ(m.update(milliseconds(0)).link_bps, 680'000u);
  // An overuse within the hold does not cut again.
  m.on_report(usage(Tier::Teleop, 600'000, tierlink::Usage::Over), milliseconds(100));
  EXPECT_EQ(m.update(milliseconds(100)).link_bps, 680'000u);
}

TEST(GccLink, OveruseBetweenTwoUpdatesIsNotMissed)
{
  LinkManager m = gcc();
  m.on_report(usage(Tier::Teleop, 800'000, tierlink::Usage::Over), milliseconds(0));
  m.on_report(usage(Tier::Teleop, 800'000, tierlink::Usage::Normal), milliseconds(50));
  EXPECT_EQ(m.update(milliseconds(100)).link_bps, 680'000u);
}

TEST(GccLink, GrowsMultiplicativelyThenAdditivelyNearTheLastOveruse)
{
  LinkManager m = gcc();
  m.on_report(usage(Tier::Teleop, 1'000'000, tierlink::Usage::Normal), milliseconds(0));
  m.update(milliseconds(0));
  m.on_report(usage(Tier::Teleop, 1'000'000, tierlink::Usage::Normal), milliseconds(1000));
  EXPECT_EQ(m.update(milliseconds(1000)).link_bps, 1'080'000u);
  m.on_report(usage(Tier::Teleop, 1'000'000, tierlink::Usage::Over), milliseconds(1100));
  EXPECT_EQ(m.update(milliseconds(1100)).link_bps, 850'000u);
  m.on_report(usage(Tier::Teleop, 850'000, tierlink::Usage::Normal), milliseconds(2100));
  EXPECT_EQ(m.update(milliseconds(2100)).link_bps, 910'000u);
}

TEST(GccLink, NeverGrowsFarAboveWhatWasDelivered)
{
  LinkManager m = gcc();
  m.on_report(usage(Tier::Teleop, 100'000, tierlink::Usage::Normal), milliseconds(0));
  m.update(milliseconds(0));
  m.on_report(usage(Tier::Teleop, 100'000, tierlink::Usage::Normal), milliseconds(5000));
  EXPECT_EQ(m.update(milliseconds(5000)).link_bps, 1'000'000u);
}

TEST(GccLink, UnderuseAndModerateLossHold)
{
  LinkManager m = gcc();
  m.on_report(usage(Tier::Teleop, 1'000'000, tierlink::Usage::Under), milliseconds(0));
  m.update(milliseconds(0));
  m.on_report(usage(Tier::Teleop, 1'000'000, tierlink::Usage::Under), milliseconds(1000));
  EXPECT_EQ(m.update(milliseconds(1000)).link_bps, 1'000'000u);
  m.on_report(usage(Tier::Teleop, 950'000, tierlink::Usage::Normal, 50'000), milliseconds(2000));
  EXPECT_EQ(m.update(milliseconds(2000)).link_bps, 1'000'000u);
}

TEST(GccLink, HeavyLossCutsByHalfTheLossFraction)
{
  LinkManager m = gcc();
  m.on_report(usage(Tier::Teleop, 800'000, tierlink::Usage::Normal, 200'000), milliseconds(0));
  EXPECT_EQ(m.update(milliseconds(0)).link_bps, 900'000u);
}

TEST(GccLink, AStandingQueueCountsAsOveruse)
{
  LinkManager m = gcc();
  m.on_report(report(Tier::Teleop, 800'000, 800'000, milliseconds(150)), milliseconds(0));
  EXPECT_EQ(m.update(milliseconds(0)).link_bps, 680'000u);
}

TEST(GccLink, MonitoringGradientCountsOnlyWithoutTeleop)
{
  LinkManager m = gcc();
  m.on_report(usage(Tier::Teleop, 800'000, tierlink::Usage::Normal), milliseconds(0));
  m.on_report(usage(Tier::Monitor, 100'000, tierlink::Usage::Over), milliseconds(0));
  EXPECT_EQ(m.update(milliseconds(0)).link_bps, 1'000'000u);
  LinkManager alone = gcc();
  alone.on_report(usage(Tier::Monitor, 800'000, tierlink::Usage::Over), milliseconds(0));
  EXPECT_EQ(alone.update(milliseconds(0)).link_bps, 680'000u);
}
