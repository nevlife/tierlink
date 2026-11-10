#ifndef TIERLINK__STREAM_HPP_
#define TIERLINK__STREAM_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace tierlink
{

// Remote control goes first; monitoring uses what is left.
enum class Tier { Teleop, Monitor };

enum class Delivery {
  // Only the newest value matters.
  Latest,
  // Every value is kept, in order, up to the capacity.
  Reliable,
  // Encoded frames; frames that waited longer than max_delay are dropped.
  Frame,
};

struct StreamSpec
{
  std::string name;
  Tier tier;
  Delivery delivery;
  // Frames reference earlier ones (H.264, H.265), so a dropped frame breaks decoding until a
  // keyframe.
  bool inter_frame = false;
  std::size_t capacity = 256;
  // Frame only. Zero: an unsent frame is replaced by the next one.
  std::chrono::nanoseconds max_delay{0};
};

struct Item
{
  std::vector<std::uint8_t> payload;
  bool keyframe = false;
};

enum class Pushed {
  Queued,
  // An older pending value was dropped to make room.
  Replaced,
  // Not kept: an inter-frame stream is waiting for a keyframe.
  Discarded,
  // An unsent frame was dropped; the encoder must produce a keyframe next.
  KeyframeNeeded,
};

// The pending data of one stream, kept by its delivery rule.
class Outbox
{
public:
  explicit Outbox(const StreamSpec & spec);
  Pushed push(Item item, std::chrono::nanoseconds now);
  std::optional<Item> pop();
  bool empty() const;

private:
  Pushed push_frame(Item item, std::chrono::nanoseconds now);
  bool front_expired(std::chrono::nanoseconds now) const;

  Delivery delivery_;
  bool inter_frame_;
  std::size_t capacity_;
  std::chrono::nanoseconds max_delay_;
  std::optional<Item> slot_;
  std::deque<Item> queue_;
  std::deque<std::chrono::nanoseconds> queued_at_;
  bool awaiting_keyframe_ = false;
};

}  // namespace tierlink

#endif  // TIERLINK__STREAM_HPP_
