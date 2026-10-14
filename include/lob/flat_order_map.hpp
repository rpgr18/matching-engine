#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/order.hpp"

namespace lob {

/// Fixed-capacity open-addressing hash map from OrderId to OrderIdx.
///
/// - Linear probing over a flat array of 16-byte slots: a lookup is usually one cache line.
/// - Fibonacci hashing (multiply by 2^64 / phi, keep the top bits) spreads sequential exchange
///   ids, the common case, evenly across the table.
/// - Backward-shift deletion instead of tombstones. A cancel-heavy order flow deletes almost as
///   often as it inserts; tombstones would grow probe lengths until a rehash, and a rehash is a
///   latency spike. With backward shift the table never degrades and never rehashes.
/// - The table is sized for twice the maximum number of live orders, so load stays at or below
///   0.5 and there is always an empty slot to terminate a probe.
///
/// Key 0 marks an empty slot, which is why order id 0 is invalid throughout the engine.
class FlatOrderMap {
 public:
  explicit FlatOrderMap(std::size_t max_entries)
      : slots_(std::bit_ceil(std::max<std::size_t>(16, max_entries * 2))),
        mask_(slots_.size() - 1),
        shift_(64 - std::countr_zero(slots_.size())),
        max_entries_(max_entries) {}

  /// Returns the slot index stored for `key`, or kNil.
  [[nodiscard]] OrderIdx find(OrderId key) const noexcept {
    assert(key != kEmpty);
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (s.key == key) return s.value;
      if (s.key == kEmpty) return kNil;
    }
  }

  [[nodiscard]] bool contains(OrderId key) const noexcept { return find(key) != kNil; }

  /// Inserts `key`, which must not be zero. Returns false if it was already present.
  bool insert(OrderId key, OrderIdx value) noexcept {
    assert(key != kEmpty);
    assert(size_ < max_entries_ || max_entries_ == 0);
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      Slot& s = slots_[i];
      if (s.key == key) return false;
      if (s.key == kEmpty) {
        s = Slot{key, value};
        ++size_;
        return true;
      }
    }
  }

  /// Removes `key`. Returns false if it was not present.
  bool erase(OrderId key) noexcept {
    assert(key != kEmpty);
    std::size_t hole = home(key);
    for (;; hole = (hole + 1) & mask_) {
      if (slots_[hole].key == key) break;
      if (slots_[hole].key == kEmpty) return false;
    }
    // Walk the rest of the probe run. An entry at j whose home is h can move into the hole if
    // the hole lies on its probe path h..j; moving it opens a new hole at j.
    for (std::size_t j = (hole + 1) & mask_;; j = (j + 1) & mask_) {
      const OrderId k = slots_[j].key;
      if (k == kEmpty) break;
      const std::size_t h = home(k);
      if (((j - h) & mask_) >= ((j - hole) & mask_)) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole] = Slot{};
    --size_;
    return true;
  }

  /// Hints the cache to fetch the line a lookup for `key` will touch first.
  void prefetch(OrderId key) const noexcept { __builtin_prefetch(&slots_[home(key)]); }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t max_entries() const noexcept { return max_entries_; }
  [[nodiscard]] std::size_t slot_count() const noexcept { return slots_.size(); }

  /// The slot a key probes first. Exposed so tests can construct collisions deliberately.
  [[nodiscard]] std::size_t home(OrderId key) const noexcept {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
  }

 private:
  static constexpr OrderId kEmpty = 0;

  struct Slot {
    OrderId key = kEmpty;
    OrderIdx value = kNil;
  };

  std::vector<Slot> slots_;
  std::size_t mask_;
  int shift_;
  std::size_t max_entries_;
  std::size_t size_ = 0;
};

}  // namespace lob
