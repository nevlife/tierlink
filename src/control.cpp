#include "tierlink/control.hpp"

#include "tierlink/zenoh_link.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <utility>

namespace tierlink
{

namespace
{

std::string control_key(const std::string & prefix, Tier tier, const std::string & name)
{
  return prefix + "/" + std::string(tier_name(tier)) + "/ctl/" + name;
}

std::string random_id()
{
  static constexpr char kHex[] = "0123456789abcdef";
  std::random_device rd;
  std::string id(32, '0');
  for (char & c : id) {
    c = kHex[rd() % 16];
  }
  return id;
}

std::uint64_t unix_ms()
{
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
}

}  // namespace

ControlPublisher::ControlPublisher(
  const zenoh::Session & session, const std::string & prefix, Tier tier, const std::string & name)
: publisher_([&] {
    auto options = zenoh::Session::PublisherOptions::create_default();
    options.priority = tier == Tier::Teleop ? Z_PRIORITY_REAL_TIME : Z_PRIORITY_DATA_HIGH;
    options.is_express = true;
    options.congestion_control = Z_CONGESTION_CONTROL_DROP;
    return session.declare_publisher(
      zenoh::KeyExpr(control_key(prefix, tier, name)), std::move(options));
  }())
{
}

void ControlPublisher::send(std::vector<std::uint8_t> message) const
{
  publisher_.put(zenoh::Bytes(std::move(message)));
}

ControlSubscriber::ControlSubscriber(
  const zenoh::Session & session, const std::string & prefix, Tier tier, const std::string & name,
  std::function<void(const std::vector<std::uint8_t> &)> on_message)
{
  subscriber_.emplace(session.declare_subscriber(
    zenoh::KeyExpr(control_key(prefix, tier, name)),
    [on_message = std::move(on_message)](zenoh::Sample & sample) {
      on_message(sample.get_payload().as_vector());
    },
    zenoh::closures::none));
}

std::uint64_t EpochCounter::next(std::uint64_t now_unix_ms)
{
  last_ = std::max(now_unix_ms, last_ + 1);
  return last_;
}

EpochIssuer::EpochIssuer(const zenoh::Session & session, const std::string & prefix)
{
  const std::string key = prefix + "/teleop/session";
  queryable_.emplace(session.declare_queryable(
    zenoh::KeyExpr(key),
    [this, key](const zenoh::Query & query) {
      std::uint64_t epoch = 0;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        epoch = counter_.next(unix_ms());
      }
      const std::string reply = random_id() + ":" + std::to_string(epoch);
      query.reply(zenoh::KeyExpr(key), zenoh::Bytes(reply));
    },
    zenoh::closures::none));
}

std::optional<SessionGrant> request_epoch(
  const zenoh::Session & session, const std::string & prefix, std::uint64_t timeout_ms)
{
  auto options = zenoh::Session::GetOptions::create_default();
  options.timeout_ms = timeout_ms;
  auto replies = session.get(
    zenoh::KeyExpr(prefix + "/teleop/session"), "", zenoh::channels::FifoChannel(4),
    std::move(options));
  for (auto r = replies.recv(); std::holds_alternative<zenoh::Reply>(r); r = replies.recv()) {
    const auto & reply = std::get<zenoh::Reply>(r);
    if (!reply.is_ok()) {
      continue;
    }
    const std::string text = reply.get_ok().get_payload().as_string();
    const auto colon = text.find(':');
    if (colon != std::string::npos) {
      return SessionGrant{text.substr(0, colon), std::stoull(text.substr(colon + 1))};
    }
  }
  return std::nullopt;
}

}  // namespace tierlink
