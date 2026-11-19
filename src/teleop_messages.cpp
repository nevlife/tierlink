#include "tierlink/teleop_messages.hpp"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace tierlink::teleop
{

namespace
{

class Writer
{
public:
  Writer() { out_.push_back(kMessageVersion); }
  Writer & u8(std::uint8_t v)
  {
    out_.push_back(v);
    return *this;
  }
  Writer & u64(std::uint64_t v)
  {
    for (int i = 0; i < 8; ++i) {
      out_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    return *this;
  }
  Writer & f64(double v)
  {
    std::uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return u64(bits);
  }
  Writer & str(const std::string & s)
  {
    const auto n = static_cast<std::uint16_t>(s.size());
    out_.push_back(static_cast<std::uint8_t>(n));
    out_.push_back(static_cast<std::uint8_t>(n >> 8));
    out_.insert(out_.end(), s.begin(), s.begin() + n);
    return *this;
  }
  std::vector<std::uint8_t> done() { return std::move(out_); }

private:
  std::vector<std::uint8_t> out_;
};

// Every read checks the length; ok() is false once one ran past the end.
class Reader
{
public:
  explicit Reader(const std::vector<std::uint8_t> & b)
  : b_(b), ok_(!b.empty() && b[0] == kMessageVersion), at_(1)
  {
  }
  std::uint8_t u8() { return need(1) ? b_[at_++] : 0; }
  std::uint64_t u64()
  {
    if (!need(8)) {
      return 0;
    }
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= static_cast<std::uint64_t>(b_[at_ + i]) << (8 * i);
    }
    at_ += 8;
    return v;
  }
  double f64()
  {
    const std::uint64_t bits = u64();
    double v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
  std::string str()
  {
    if (!need(2)) {
      return {};
    }
    const std::size_t n = b_[at_] | (b_[at_ + 1] << 8);
    at_ += 2;
    if (!need(n)) {
      return {};
    }
    std::string s(b_.begin() + at_, b_.begin() + at_ + n);
    at_ += n;
    return s;
  }
  bool ok() const { return ok_ && at_ == b_.size(); }

private:
  bool need(std::size_t n)
  {
    ok_ = ok_ && at_ + n <= b_.size();
    return ok_;
  }
  const std::vector<std::uint8_t> & b_;
  bool ok_;
  std::size_t at_;
};

template <class T>
std::optional<T> checked(const Reader & r, T value)
{
  if (!r.ok()) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

std::vector<std::uint8_t> encode(const ArmRequest & m)
{
  return Writer().str(m.robot_id).str(m.session_id).u64(m.epoch).done();
}

std::vector<std::uint8_t> encode(const MotionCommand & m)
{
  return Writer()
    .str(m.robot_id)
    .str(m.session_id)
    .u64(m.session_epoch)
    .u64(m.sequence)
    .u64(static_cast<std::uint64_t>(m.sent.ns))
    .u64(static_cast<std::uint64_t>(m.valid_for.count()))
    .f64(m.linear_mps)
    .f64(m.angular_rps)
    .u8(m.deadman ? 1 : 0)
    .done();
}

std::vector<std::uint8_t> encode(const StopCommand & m)
{
  return Writer().str(m.robot_id).str(m.session_id).u64(m.epoch).done();
}

std::vector<std::uint8_t> encode(const ArmState & m)
{
  return Writer()
    .str(m.robot_id)
    .str(m.session_id)
    .u64(m.epoch)
    .u8(m.armed ? 1 : 0)
    .str(m.reason)
    .done();
}

std::optional<ArmRequest> decode_arm(const std::vector<std::uint8_t> & b)
{
  Reader r(b);
  ArmRequest m{r.str(), r.str(), r.u64()};
  return checked(r, std::move(m));
}

std::optional<MotionCommand> decode_motion(const std::vector<std::uint8_t> & b)
{
  Reader r(b);
  MotionCommand m;
  m.robot_id = r.str();
  m.session_id = r.str();
  m.session_epoch = r.u64();
  m.sequence = r.u64();
  m.sent = UnixNs{static_cast<std::int64_t>(r.u64())};
  m.valid_for = std::chrono::nanoseconds(static_cast<std::int64_t>(r.u64()));
  m.linear_mps = r.f64();
  m.angular_rps = r.f64();
  m.deadman = r.u8() != 0;
  return checked(r, std::move(m));
}

std::optional<StopCommand> decode_stop(const std::vector<std::uint8_t> & b)
{
  Reader r(b);
  StopCommand m{r.str(), r.str(), r.u64()};
  return checked(r, std::move(m));
}

std::optional<ArmState> decode_state(const std::vector<std::uint8_t> & b)
{
  Reader r(b);
  ArmState m;
  m.robot_id = r.str();
  m.session_id = r.str();
  m.epoch = r.u64();
  m.armed = r.u8() != 0;
  m.reason = r.str();
  return checked(r, std::move(m));
}

std::vector<std::uint8_t> encode_hop(std::chrono::nanoseconds rtt)
{
  return Writer().u64(static_cast<std::uint64_t>(rtt.count())).done();
}

std::optional<std::chrono::nanoseconds> decode_hop(const std::vector<std::uint8_t> & b)
{
  Reader r(b);
  const std::chrono::nanoseconds rtt(static_cast<std::int64_t>(r.u64()));
  return checked(r, rtt);
}

}  // namespace tierlink::teleop
