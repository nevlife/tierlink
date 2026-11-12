#ifndef TIERLINK__CONTROL_HPP_
#define TIERLINK__CONTROL_HPP_

#include "tierlink/stream.hpp"
#include "zenoh.hxx"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Small control messages in either direction: motion commands, arm requests, vehicle state. They
// are not paced by the link controller; their sender owns their rate and they are tiny. Keys are
// <prefix>/<tier>/ctl/<name>.
namespace tierlink
{

class ControlPublisher
{
public:
  ControlPublisher(
    const zenoh::Session & session, const std::string & prefix, Tier tier,
    const std::string & name);
  void send(std::vector<std::uint8_t> message) const;

private:
  zenoh::Publisher publisher_;
};

class ControlSubscriber
{
public:
  // on_message runs on a Zenoh thread.
  ControlSubscriber(
    const zenoh::Session & session, const std::string & prefix, Tier tier, const std::string & name,
    std::function<void(const std::vector<std::uint8_t> &)> on_message);

private:
  std::optional<zenoh::Subscriber<void>> subscriber_;
};

struct SessionGrant
{
  std::string session_id;
  std::uint64_t epoch;
};

// Epochs only grow: wall-clock milliseconds keep that true across restarts of the issuer, and +1
// keeps it true within one.
class EpochCounter
{
public:
  std::uint64_t next(std::uint64_t now_unix_ms);

private:
  std::uint64_t last_ = 0;
};

// The single source of session epochs for <prefix>/teleop/session: the vehicle, whose guard
// checks them.
class EpochIssuer
{
public:
  EpochIssuer(const zenoh::Session & session, const std::string & prefix);

private:
  std::mutex mutex_;
  EpochCounter counter_;
  std::optional<zenoh::Queryable<void>> queryable_;
};

// Asks the issuer for a new session; nothing when no issuer answers within the timeout.
std::optional<SessionGrant> request_epoch(
  const zenoh::Session & session, const std::string & prefix, std::uint64_t timeout_ms);

// The server answers <prefix>/server/ping, so each peer can time its own hop to the server; the
// one-way delay of a path through the server is then split by hop.
class PingResponder
{
public:
  PingResponder(const zenoh::Session & session, const std::string & prefix);

private:
  std::optional<zenoh::Queryable<void>> queryable_;
};

// Round trip to the server, or nullopt when no reply came within timeout_ms. Blocks.
std::optional<std::chrono::nanoseconds> ping_server(
  const zenoh::Session & session, const std::string & prefix, std::uint64_t timeout_ms);

}  // namespace tierlink

#endif  // TIERLINK__CONTROL_HPP_
