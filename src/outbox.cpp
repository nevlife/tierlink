#include "tierlink/stream.hpp"

#include <utility>

namespace tierlink
{

Outbox::Outbox(const StreamSpec & spec)
: delivery_(spec.delivery),
  inter_frame_(spec.inter_frame),
  capacity_(spec.capacity),
  max_delay_(spec.max_delay)
{
}

Pushed Outbox::push(Item item, std::chrono::nanoseconds now)
{
  if (delivery_ == Delivery::Frame) {
    return push_frame(std::move(item), now);
  }
  if (delivery_ == Delivery::Reliable) {
    const bool full = queue_.size() >= capacity_;
    if (full) {
      queue_.pop_front();
    }
    queue_.push_back(std::move(item));
    return full ? Pushed::Replaced : Pushed::Queued;
  }
  const bool replaced = slot_.has_value();
  slot_ = std::move(item);
  return replaced ? Pushed::Replaced : Pushed::Queued;
}

bool Outbox::front_expired(std::chrono::nanoseconds now) const
{
  return !queued_at_.empty() && now - queued_at_.front() >= max_delay_;
}

Pushed Outbox::push_frame(Item item, std::chrono::nanoseconds now)
{
  Pushed result = Pushed::Queued;
  if (inter_frame_ && item.keyframe) {
    // Frames before a keyframe are no longer needed to decode what follows.
    result = queue_.empty() ? Pushed::Queued : Pushed::Replaced;
    queue_.clear();
    queued_at_.clear();
    awaiting_keyframe_ = false;
  } else if (inter_frame_ && awaiting_keyframe_) {
    return Pushed::Discarded;
  } else if (inter_frame_ && front_expired(now)) {
    // The dropped frames and everything after them are undecodable without them.
    queue_.clear();
    queued_at_.clear();
    awaiting_keyframe_ = true;
    return Pushed::KeyframeNeeded;
  } else {
    while (front_expired(now)) {
      queue_.pop_front();
      queued_at_.pop_front();
      result = Pushed::Replaced;
    }
  }
  queue_.push_back(std::move(item));
  queued_at_.push_back(now);
  return result;
}

std::optional<Item> Outbox::pop()
{
  if (delivery_ == Delivery::Latest) {
    std::optional<Item> item = std::move(slot_);
    slot_.reset();
    return item;
  }
  if (queue_.empty()) {
    return std::nullopt;
  }
  Item item = std::move(queue_.front());
  queue_.pop_front();
  if (!queued_at_.empty()) {
    queued_at_.pop_front();
  }
  return item;
}

bool Outbox::empty() const
{
  return delivery_ == Delivery::Latest ? !slot_.has_value() : queue_.empty();
}

}  // namespace tierlink
