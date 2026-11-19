#ifndef TIERLINK__TELEOP_MESSAGES_HPP_
#define TIERLINK__TELEOP_MESSAGES_HPP_

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Teleop control messages on the control keys (arm, motion, stop, estop, estop_reset from the
// operator; state and hop from the vehicle). Little-endian, strings as u16 length and bytes, led
// by a version byte so a mismatched peer is refused instead of misread. The operator's Rust copy
// is operator/src/wire.rs.
namespace tierlink::teleop
{

inline constexpr std::uint8_t kMessageVersion = 1;

// Wall-clock time as the operator stamps commands. A distinct type, so it is never compared with
// a monotonic time.
struct UnixNs
{
  std::int64_t ns;
};

struct ArmRequest
{
  std::string robot_id;
  std::string session_id;
  std::uint64_t epoch;
};

struct MotionCommand
{
  std::string robot_id;
  std::string session_id;
  std::uint64_t session_epoch;
  std::uint64_t sequence;
  UnixNs sent;
  std::chrono::nanoseconds valid_for;
  double linear_mps;
  double angular_rps;
  bool deadman;
};

// A stop is honored from any session for this robot.
struct StopCommand
{
  std::string robot_id;
  std::string session_id;
  std::uint64_t epoch;
};

struct ArmState
{
  std::string robot_id;
  std::string session_id;
  std::uint64_t epoch;
  bool armed;
  std::string reason;
};

std::vector<std::uint8_t> encode(const ArmRequest & m);
std::vector<std::uint8_t> encode(const MotionCommand & m);
std::vector<std::uint8_t> encode(const StopCommand & m);
std::vector<std::uint8_t> encode(const ArmState & m);

std::optional<ArmRequest> decode_arm(const std::vector<std::uint8_t> & b);
std::optional<MotionCommand> decode_motion(const std::vector<std::uint8_t> & b);
std::optional<StopCommand> decode_stop(const std::vector<std::uint8_t> & b);
std::optional<ArmState> decode_state(const std::vector<std::uint8_t> & b);

// The vehicle's round trip to the server, so the operator can split the path delay by hop.
std::vector<std::uint8_t> encode_hop(std::chrono::nanoseconds rtt);
std::optional<std::chrono::nanoseconds> decode_hop(const std::vector<std::uint8_t> & b);

}  // namespace tierlink::teleop

#endif  // TIERLINK__TELEOP_MESSAGES_HPP_
