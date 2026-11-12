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

constexpr tierlink::LinkManagerConfig kConfig{
  1'000'000,
  500'000,
  4'000'000,
  0.85,
  0.1,
  100'000,
  milliseconds(300),
  milliseconds(1000),
  milliseconds(100),
  milliseconds(30),
  milliseconds(50),
  150'000,
  2'000'000,
  30'000,
  milliseconds(500),
  milliseconds(3000)};

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
  const LinkReport r{Tier::Monitor, 1, 2, milliseconds(3), milliseconds(4)};
  const auto bytes = tierlink::encode_report(r);
  const auto back = tierlink::decode_report(bytes);
  ASSERT_TRUE(back);
  EXPECT_EQ(back->tier, Tier::Monitor);
  EXPECT_EQ(back->acked_bps, 2u);
  EXPECT_EQ(back->stall, milliseconds(4));
  EXPECT_FALSE(tierlink::decode_report({bytes.begin(), bytes.end() - 1}));
}

TEST(LinkManager, CongestionCutsToWhatWasDeliveredOncePerHold)
{
  LinkManager m(kConfig);
  m.on_report(report(Tier::Teleop, 900'000, 800'000, milliseconds(150)), milliseconds(0));
  EXPECT_EQ(m.update(milliseconds(0)).link_bps, 680'000u);
  EXPECT_EQ(m.update(milliseconds(100)).link_bps, 680'000u);
  m.on_report(report(Tier::Teleop, 600'000, 600'000, milliseconds(150)), milliseconds(300));
  EXPECT_EQ(m.update(milliseconds(300)).link_bps, 510'000u);
}

TEST(LinkManager, GrowsOnlyWhileTheRateIsUsed)
{
  LinkManager m(kConfig);
  m.on_report(report(Tier::Monitor, 100'000, 100'000, milliseconds(5)), milliseconds(0));
  m.update(milliseconds(0));
  m.on_report(report(Tier::Monitor, 100'000, 100'000, milliseconds(5)), milliseconds(1000));
  EXPECT_EQ(m.update(milliseconds(1000)).link_bps, 1'000'000u);
  m.on_report(report(Tier::Monitor, 900'000, 900'000, milliseconds(5)), milliseconds(1100));
  EXPECT_EQ(m.update(milliseconds(1100)).link_bps, 1'100'000u);
}

TEST(LinkManager, TeleopTakesWhatItNeedsAndMonitoringKeepsItsFloor)
{
  LinkManager m(kConfig);
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
  LinkManager m(kConfig);
  m.on_report(report(Tier::Monitor, 50'000, 50'000, milliseconds(5)), milliseconds(0));
  const auto b = m.update(milliseconds(0));
  EXPECT_EQ(b.tier_bps[1], 1'000'000u);
}

TEST(LinkManager, AStallNobodyAcknowledgedIsIgnoredButAnOutageCuts)
{
  LinkManager m(kConfig);
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
  auto config = kConfig;
  config.start_bps = 2'000'000;
  LinkManager m(config);
  m.on_report(report(Tier::Monitor, 1'900'000, 1'900'000, milliseconds(5)), milliseconds(0));
  m.update(milliseconds(0));
  m.on_report(report(Tier::Monitor, 1'900'000, 1'900'000, milliseconds(5)), milliseconds(1000));
  EXPECT_EQ(m.update(milliseconds(1000)).link_bps, 2'200'000u);
}
