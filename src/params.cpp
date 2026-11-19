#include "tierlink/params.hpp"

#include "tierlink/zenoh_link.hpp"

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace tierlink
{

namespace
{

// A FRAME with one PUT of a declared key and the 30-byte attachment: 83 bytes in a capture
// (tools/bench/zcap.py), with room for longer sequence numbers.
constexpr std::size_t kZenohOverhead = 100;

std::uint64_t kbps(const YAML::Node & n, const char * key)
{
  return n[key].as<std::uint64_t>() * 1000;
}

Nanos ms(const YAML::Node & n, const char * key)
{
  return std::chrono::milliseconds(n[key].as<std::int64_t>());
}

template <typename Config>
void check_range(const Config & c, const char * name)
{
  if (c.min_bps > c.start_bps || c.start_bps > c.max_bps) {
    throw std::invalid_argument(std::string(name) + ": needs min_kbps <= start_kbps <= max_kbps");
  }
}

EdgeParams edge_params(const YAML::Node & e)
{
  EdgeParams p;
  p.prefix = e["prefix"].as<std::string>();
  p.listen = e["listen"].as<std::vector<std::string>>();
  p.connect = e["connect"].as<std::vector<std::string>>();
  p.period = ms(e, "period_ms");
  p.estimator = e["estimator"].as<std::string>();
  const YAML::Node rate = e["rate"];
  const YAML::Node a = e["aimd"];
  p.aimd = AimdLinkConfig{
    kbps(rate, "start_kbps"), kbps(rate, "min_kbps"),     kbps(rate, "max_kbps"),
    a["factor"].as<double>(), a["increase"].as<double>(), kbps(a, "step_kbps"),
    ms(a, "hold_ms"),         ms(a, "probe_ms"),          ms(a, "congested_above_ms"),
    ms(a, "clear_below_ms")};
  const YAML::Node g = e["gcc"];
  p.gcc = GccLinkConfig{
    kbps(rate, "start_kbps"),
    kbps(rate, "min_kbps"),
    kbps(rate, "max_kbps"),
    g["decrease"].as<double>(),
    g["increase_per_s"].as<double>(),
    kbps(g, "additive_kbps_per_s"),
    ms(g, "hold_ms"),
    ms(g, "congested_above_ms"),
    g["loss_high"].as<double>(),
    g["loss_low"].as<double>()};
  const YAML::Node s = e["split"];
  p.split = SplitConfig{
    ms(s, "monitor_stop_above_ms"), kbps(s, "teleop_min_kbps"),   kbps(s, "teleop_max_kbps"),
    kbps(s, "monitor_floor_kbps"),  ms(s, "report_timeout_ms"), ms(s, "acked_within_ms"),
    s["teleop_headroom"].as<double>(), ms(s, "teleop_usage_window_ms")};
  check_range(p.aimd, "edge.rate");
  if (p.estimator != "aimd" && p.estimator != "gcc") {
    throw std::invalid_argument("edge.estimator must be aimd or gcc");
  }
  return p;
}

}  // namespace

Params load_params(const std::string & path)
{
  const YAML::Node root = YAML::LoadFile(path);
  const YAML::Node l = root["link"];
  Params p;
  p.link = LinkParams{
    l["batch_size"].as<std::uint16_t>(),
    l["lease_ms"].as<int>(),
    l["keep_alive"].as<int>(),
    l["tcp_sndbuf"].as<std::uint32_t>(),
    l["chunk_bytes"].as<std::size_t>(),
    l["frame_chunk_bytes"].as<std::size_t>(),
    static_cast<std::uint8_t>(l["fec_group"].as<int>())};
  // Zenoh adds its frame, message and attachment headers to each piece.
  if (p.link.frame_chunk_bytes + kZenohOverhead > p.link.batch_size) {
    throw std::invalid_argument(
      "link.frame_chunk_bytes + " + std::to_string(kZenohOverhead) +
      " must fit link.batch_size, or a lost datagram loses a piece that FEC counted on");
  }
  if (p.link.keep_alive < 1 || p.link.lease_ms < p.link.keep_alive) {
    throw std::invalid_argument("link: needs 1 <= keep_alive <= lease_ms");
  }
  p.edge = edge_params(root["edge"]);
  return p;
}

std::unique_ptr<LinkEstimator> make_estimator(const EdgeParams & edge)
{
  if (edge.estimator == "gcc") {
    return std::make_unique<GccLink>(edge.gcc);
  }
  return std::make_unique<AimdLink>(edge.aimd);
}

zenoh::Config zenoh_config(
  const LinkParams & link, const std::string & mode, const std::vector<std::string> & connect,
  const std::vector<std::string> & listen)
{
  auto c = make_config(mode, connect, listen, std::nullopt);
  c.insert_json5("transport/link/tx/batch_size", std::to_string(link.batch_size));
  c.insert_json5("transport/link/tx/lease", std::to_string(link.lease_ms));
  c.insert_json5("transport/link/tx/keep_alive", std::to_string(link.keep_alive));
  if (link.tcp_sndbuf > 0) {
    c.insert_json5("transport/link/tcp/so_sndbuf", std::to_string(link.tcp_sndbuf));
  }
  return c;
}

}  // namespace tierlink
