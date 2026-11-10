#include "tierlink/stream.hpp"

#include <utility>

namespace tierlink
{

namespace
{

Pushed replace(std::optional<Item> & slot, Item item)
{
  const bool replaced = slot.has_value();
  slot = std::move(item);
  return replaced ? Pushed::Replaced : Pushed::Queued;
}

}  // namespace

Outbox::Outbox(const StreamSpec & spec)
: delivery_(spec.delivery), inter_frame_(spec.inter_frame), capacity_(spec.capacity)
{
}

Pushed Outbox::push(Item item)
{
  if (delivery_ == Delivery::Reliable) {
    const bool full = queue_.size() >= capacity_;
    if (full) {
      queue_.pop_front();
    }
    queue_.push_back(std::move(item));
    return full ? Pushed::Replaced : Pushed::Queued;
  }
  if (delivery_ == Delivery::Frame && inter_frame_) {
    return push_inter_frame(std::move(item));
  }
  return replace(slot_, std::move(item));
}

Pushed Outbox::push_inter_frame(Item item)
{
  if (item.keyframe) {
    awaiting_keyframe_ = false;
    return replace(slot_, std::move(item));
  }
  if (awaiting_keyframe_) {
    return Pushed::Discarded;
  }
  if (slot_) {
    // The dropped frame and everything after it are undecodable without it.
    slot_.reset();
    awaiting_keyframe_ = true;
    return Pushed::KeyframeNeeded;
  }
  slot_ = std::move(item);
  return Pushed::Queued;
}

std::optional<Item> Outbox::pop()
{
  if (delivery_ == Delivery::Reliable) {
    if (queue_.empty()) {
      return std::nullopt;
    }
    Item item = std::move(queue_.front());
    queue_.pop_front();
    return item;
  }
  std::optional<Item> item = std::move(slot_);
  slot_.reset();
  return item;
}

bool Outbox::empty() const
{
  return delivery_ == Delivery::Reliable ? queue_.empty() : !slot_.has_value();
}

}  // namespace tierlink
