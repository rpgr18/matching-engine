#pragma once

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "lob/order.hpp"

namespace lob {

/// Fixed-capacity slab of Order slots with an intrusive LIFO free list.
///
/// All memory is allocated and touched up front, so allocate() and release() are a couple of
/// loads and stores with no syscalls, no locks and no page faults. LIFO reuse hands back the most
/// recently freed slot, which is the one most likely to still be in cache.
class OrderPool {
 public:
  explicit OrderPool(std::uint32_t capacity) : orders_(capacity) {
    if (capacity == kNil) throw std::invalid_argument("OrderPool capacity collides with kNil");
    for (std::uint32_t i = 0; i < capacity; ++i) orders_[i].next = i + 1 < capacity ? i + 1 : kNil;
    free_head_ = capacity > 0 ? 0 : kNil;
  }

  /// Returns a free slot, or kNil when the pool is exhausted. The slot's contents are unspecified.
  [[nodiscard]] OrderIdx allocate() noexcept {
    const OrderIdx idx = free_head_;
    if (idx != kNil) {
      free_head_ = orders_[idx].next;
      ++size_;
    }
    return idx;
  }

  void release(OrderIdx idx) noexcept {
    assert(idx < orders_.size());
    assert(size_ > 0);
    orders_[idx].next = free_head_;
    free_head_ = idx;
    --size_;
  }

  [[nodiscard]] Order& operator[](OrderIdx idx) noexcept {
    assert(idx < orders_.size());
    return orders_[idx];
  }
  [[nodiscard]] const Order& operator[](OrderIdx idx) const noexcept {
    assert(idx < orders_.size());
    return orders_[idx];
  }

  [[nodiscard]] bool full() const noexcept { return free_head_ == kNil; }
  [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
  [[nodiscard]] std::uint32_t capacity() const noexcept {
    return static_cast<std::uint32_t>(orders_.size());
  }

 private:
  std::vector<Order> orders_;
  OrderIdx free_head_ = kNil;
  std::uint32_t size_ = 0;
};

}  // namespace lob
