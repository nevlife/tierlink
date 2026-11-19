#ifndef TIERLINK__PARAMS_HPP_
#define TIERLINK__PARAMS_HPP_

#include "tierlink/link_manager.hpp"
#include "zenoh.hxx"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// One file for the edge, the server and the client, so values that depend on each other (batch
// size, chunk size, lease) are written once.
namespace tierlink
{

struct LinkParams
{
  std::uint16_t batch_size;
  int lease_ms;
  int keep_alive;
  // Kernel send buffer of the TCP link in bytes; 0 keeps the OS default (autotuned to MBs).
  std::uint32_t tcp_sndbuf;
  std::size_t chunk_bytes;
  // Frame streams (UDP): pieces that fit one datagram, and one parity per fec_group pieces.
  std::size_t frame_chunk_bytes;
  std::uint8_t fec_group;
};

struct EdgeParams
{
  std::string prefix;
  std::vector<std::string> listen;
  std::vector<std::string> connect;
  Nanos period;
  std::string estimator;
  AimdLinkConfig aimd;
  GccLinkConfig gcc;
  SplitConfig split;
};

struct Params
{
  LinkParams link;
  EdgeParams edge;
};

// Throws YAML::Exception on a missing or malformed value, and std::invalid_argument on values
// that contradict each other.
Params load_params(const std::string & path);

std::unique_ptr<LinkEstimator> make_estimator(const EdgeParams & edge);

// The Zenoh settings of `link` on top of make_config.
zenoh::Config zenoh_config(
  const LinkParams & link, const std::string & mode, const std::vector<std::string> & connect,
  const std::vector<std::string> & listen);

}  // namespace tierlink

#endif  // TIERLINK__PARAMS_HPP_
