#include "tierlink/zenoh_link.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tierlink
{

namespace
{

std::string json_list(const std::vector<std::string> & items)
{
  std::string out = "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    out += (i == 0 ? "\"" : ",\"") + items[i] + "\"";
  }
  return out + "]";
}

zenoh::Publisher declare_control(const zenoh::Session & session, const std::string & key)
{
  // A lost control message costs little: acks are cumulative and keyframe requests repeat.
  auto options = zenoh::Session::PublisherOptions::create_default();
  options.priority = Z_PRIORITY_INTERACTIVE_HIGH;
  options.is_express = true;
  options.congestion_control = Z_CONGESTION_CONTROL_DROP;
  return session.declare_publisher(zenoh::KeyExpr(key), std::move(options));
}

zenoh::Publisher declare_stream(
  const zenoh::Session & session, const std::string & base, const StreamSpec & spec)
{
  const bool teleop = spec.tier == Tier::Teleop;
  auto options = zenoh::Session::PublisherOptions::create_default();
  options.priority = teleop ? Z_PRIORITY_INTERACTIVE_HIGH : Z_PRIORITY_DATA;
  options.is_express = teleop;
  // Block keeps reliable data instead of dropping it in Zenoh's queue; the scheduler already
  // paces it below the link budget.
  options.congestion_control =
    spec.delivery == Delivery::Reliable ? Z_CONGESTION_CONTROL_BLOCK : Z_CONGESTION_CONTROL_DROP;
  if (spec.delivery == Delivery::Frame) {
    options.reliability = Z_RELIABILITY_BEST_EFFORT;
  }
  return session.declare_publisher(zenoh::KeyExpr(base + "/data/" + spec.name), std::move(options));
}

std::string last_segment(const zenoh::KeyExpr & key)
{
  const std::string_view k = key.as_string_view();
  const auto slash = k.rfind('/');
  return std::string(slash == std::string_view::npos ? k : k.substr(slash + 1));
}

}  // namespace

std::string alive_key(const std::string & prefix, const std::string & name)
{
  return prefix + "/alive/" + name;
}

zenoh::Config make_config(
  const std::string & mode, const std::vector<std::string> & connect,
  const std::vector<std::string> & listen, std::optional<Tuning> tuning)
{
  auto c = zenoh::Config::create_default();
  c.insert_json5("mode", "\"" + mode + "\"");
  c.insert_json5("connect/endpoints", json_list(connect));
  c.insert_json5("listen/endpoints", json_list(listen));
  c.insert_json5("scouting/multicast/enabled", "false");
  c.insert_json5("scouting/gossip/enabled", "false");
  // A link that went silent (an LTE drop, not a closed socket) is declared lost after one lease,
  // so liveliness tokens show who is gone within a second instead of the default ten.
  c.insert_json5("transport/link/tx/lease", std::to_string(kLeaseMs));
  c.insert_json5("transport/link/tx/keep_alive", "4");
  if (tuning) {
    c.insert_json5("transport/link/tx/batch_size", std::to_string(tuning->batch_size));
    for (const char * p :
         {"control", "real_time", "interactive_high", "interactive_low", "data_high", "data",
          "data_low", "background"}) {
      c.insert_json5(
        std::string("transport/link/tx/queue/size/") + p, std::to_string(tuning->queue_batches));
    }
  }
  return c;
}

std::string_view tier_name(Tier tier)
{
  return tier == Tier::Teleop ? "teleop" : "monitor";
}

Nanos steady_now()
{
  return std::chrono::duration_cast<Nanos>(std::chrono::steady_clock::now().time_since_epoch());
}

std::string report_key(const std::string & prefix, Tier tier)
{
  return prefix + "/link/report/" + std::string(tier_name(tier));
}

std::string budget_key(const std::string & prefix, Tier tier)
{
  return prefix + "/link/budget/" + std::string(tier_name(tier));
}

BudgetSubscriber::BudgetSubscriber(
  const zenoh::Session & session, const std::string & prefix, Tier tier,
  std::shared_ptr<BudgetSlot> slot)
{
  subscriber_.emplace(session.declare_subscriber(
    zenoh::KeyExpr(budget_key(prefix, tier)),
    [slot](zenoh::Sample & sample) {
      const auto bps = decode_bps(sample.get_payload().as_vector());
      if (bps) {
        slot->set(*bps, steady_now());
      }
    },
    zenoh::closures::none));
}

Uplink::Uplink(
  const zenoh::Session & session, UplinkConfig config, std::unique_ptr<RateController> controller)
: scheduler_(config.streams, 0, config.burst, config.chunk_bytes),
  tracker_(config.delay_window),
  controller_(std::move(controller)),
  tier_(config.tier),
  stream_seq_(config.streams.size(), 0),
  item_seq_(config.streams.size(), 0),
  keyframe_requested_(config.streams.size(), false),
  report_pub_(declare_control(session, report_key(config.prefix, config.tier)))
{
  const std::string base = config.prefix + "/" + std::string(tier_name(config.tier));
  for (const auto & s : config.streams) {
    publishers_.push_back(declare_stream(session, base, s));
  }
  acks_.emplace(session.declare_subscriber(
    zenoh::KeyExpr(base + "/ack"),
    [this](zenoh::Sample & sample) {
      const auto ack = decode_ack(sample.get_payload().as_vector());
      if (ack) {
        std::lock_guard<std::mutex> lock(mutex_);
        tracker_.on_ack(ack->first, ack->second, steady_now());
      }
    },
    zenoh::closures::none));
  keyframe_requests_.emplace(session.declare_subscriber(
    zenoh::KeyExpr(base + "/keyframe"),
    [this](zenoh::Sample & sample) {
      const auto bytes = sample.get_payload().as_vector();
      std::lock_guard<std::mutex> lock(mutex_);
      const auto id =
        scheduler_.id(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
      if (id) {
        keyframe_requested_[*id] = true;
      }
    },
    zenoh::closures::none));
  thread_ =
    std::thread([this, tick = config.tick, period = config.report_period] { run(tick, period); });
}

Uplink::~Uplink()
{
  stop_ = true;
  thread_.join();
}

std::optional<std::size_t> Uplink::stream_id(std::string_view name) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return scheduler_.id(name);
}

Pushed Uplink::push(std::size_t id, std::vector<std::uint8_t> payload, bool keyframe)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const Pushed pushed = scheduler_.push(id, Item{std::move(payload), keyframe}, steady_now());
  if (pushed == Pushed::KeyframeNeeded) {
    keyframe_requested_[id] = true;
  }
  return pushed;
}

bool Uplink::take_keyframe_request(std::size_t id)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const bool requested = keyframe_requested_[id];
  keyframe_requested_[id] = false;
  return requested;
}

void Uplink::set_cap(std::optional<std::uint64_t> bps)
{
  std::lock_guard<std::mutex> lock(mutex_);
  cap_bps_ = bps;
}

std::pair<std::optional<Decision>, Feedback> Uplink::status() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return {decision_, tracker_.feedback(steady_now())};
}

std::vector<Uplink::Outgoing> Uplink::take_batch()
{
  std::lock_guard<std::mutex> lock(mutex_);
  const Nanos now = steady_now();
  const Decision decision = controller_->update(now, tracker_.feedback(now));
  scheduler_.set_budget(cap_bps_ ? std::min(*cap_bps_, decision.target_bps) : decision.target_bps);
  decision_ = decision;
  std::vector<Outgoing> batch;
  while (!decision.max_in_flight_bytes ||
         tracker_.feedback(now).in_flight_bytes < *decision.max_in_flight_bytes) {
    auto piece = scheduler_.next(now);
    if (!piece) {
      break;
    }
    const std::size_t id = piece->id;
    if (piece->chunk == 0) {
      item_seq_[id] = stream_seq_[id]++;
    }
    const Header header{next_seq_++,     item_seq_[id], now.count(),
                        piece->keyframe, piece->chunk,  piece->chunks};
    tracker_.on_sent(header.seq, piece->payload.size(), now);
    batch.push_back(Outgoing{id, header, std::move(piece->payload)});
  }
  return batch;
}

LinkReport Uplink::report(Nanos now)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto [sent, acked] = tracker_.totals();
  totals_.emplace_back(now, sent, acked);
  while (totals_.size() > 2 && now - std::get<0>(totals_[1]) >= std::chrono::seconds(1)) {
    totals_.pop_front();
  }
  const auto & [t0, sent0, acked0] = totals_.front();
  const double seconds = std::chrono::duration<double>(now - t0).count();
  const auto rate = [seconds](std::uint64_t bytes) {
    return seconds > 0.0 ? static_cast<std::uint64_t>(static_cast<double>(bytes) * 8 / seconds) : 0;
  };
  const Feedback f = tracker_.feedback(now);
  return LinkReport{tier_, rate(sent - sent0), rate(acked - acked0), f.queue_delay, f.stall};
}

void Uplink::run(Nanos tick, Nanos report_period)
{
  auto wake = std::chrono::steady_clock::now();
  Nanos next_report = steady_now();
  while (!stop_) {
    wake += tick;
    std::this_thread::sleep_until(wake);
    if (steady_now() >= next_report) {
      next_report += report_period;
      report_pub_.put(zenoh::Bytes(encode_report(report(steady_now()))));
    }
    for (auto & out : take_batch()) {
      auto options = zenoh::Publisher::PutOptions::create_default();
      options.attachment = zenoh::Bytes(out.header.encode());
      publishers_[out.id].put(zenoh::Bytes(std::move(out.payload)), std::move(options));
    }
  }
}

Receiver::PerTier::PerTier(const zenoh::Session & session, const std::string & base)
: acks(declare_control(session, base + "/ack")),
  keyframe_requests(declare_control(session, base + "/keyframe"))
{
}

Receiver::Receiver(
  const zenoh::Session & session, const std::string & prefix, Nanos keyframe_repeat,
  std::function<void(Received)> on_received)
{
  for (const Tier tier : {Tier::Teleop, Tier::Monitor}) {
    const std::string base = prefix + "/" + std::string(tier_name(tier));
    auto & t = tiers_[static_cast<std::size_t>(tier)];
    t = std::make_unique<PerTier>(session, base);
    PerTier * state = t.get();
    state->subscriber.emplace(session.declare_subscriber(
      zenoh::KeyExpr(base + "/data/*"),
      [state, tier, keyframe_repeat, on_received](zenoh::Sample & sample) {
        const Nanos now = steady_now();
        const auto attachment = sample.get_attachment();
        const auto header =
          attachment ? Header::decode(attachment->get().as_vector()) : std::nullopt;
        if (!header) {
          return;
        }
        state->acks.put(zenoh::Bytes(encode_ack(header->seq, now.count())));
        std::string stream = last_segment(sample.get_keyexpr());
        Arrival arrival{};
        std::optional<std::vector<std::uint8_t>> item;
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          item = state->pieces[stream].add(*header, sample.get_payload().as_vector());
          if (!item) {
            return;
          }
          arrival = state->gaps.try_emplace(stream, keyframe_repeat)
                      .first->second.on_arrival(header->stream_seq, header->keyframe, now);
        }
        if (arrival.request_keyframe) {
          state->keyframe_requests.put(
            zenoh::Bytes(std::vector<std::uint8_t>(stream.begin(), stream.end())));
        }
        on_received(Received{
          tier, std::move(stream), header->seq, header->keyframe, arrival.lost, std::move(*item)});
      },
      zenoh::closures::none));
  }
}

void Receiver::request_keyframe(Tier tier, const std::string & stream)
{
  tiers_[static_cast<std::size_t>(tier)]->keyframe_requests.put(
    zenoh::Bytes(std::vector<std::uint8_t>(stream.begin(), stream.end())));
}

}  // namespace tierlink
