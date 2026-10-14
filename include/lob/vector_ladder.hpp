#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <vector>

#include "lob/flat_order_map.hpp"
#include "lob/order.hpp"

namespace lob {

/// One side of the book as a contiguous array of levels sorted worst to best, so the best level
/// is at the back.
///
/// Order flow concentrates near the touch, and near the back of the array inserting or erasing a
/// level shifts only a handful of elements while lookups scan a few contiguous entries. Far from
/// the touch both degrade: lookup falls back to binary search and insertion becomes a memmove of
/// everything better than the new level.
template <Side S>
class VectorLadder {
  struct Entry {
    Price price;
    Level level;
  };
  using Iter = typename std::vector<Entry>::iterator;
  using ConstIter = typename std::vector<Entry>::const_iterator;

  /// Levels within this many of the best are found by a linear scan from the back.
  static constexpr int kLinearScan = 8;

 public:
  VectorLadder(Price /*min_price*/, Price /*max_price*/) { levels_.reserve(4096); }

  [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }

  [[nodiscard]] Price best_price() const noexcept {
    assert(!empty());
    return levels_.back().price;
  }
  [[nodiscard]] Level& best_level() noexcept {
    assert(!empty());
    return levels_.back().level;
  }

  [[nodiscard]] Level& at(Price p) noexcept {
    const Iter it = locate(levels_.begin(), levels_.end(), p);
    assert(it != levels_.begin() && (it - 1)->price == p);
    return (it - 1)->level;
  }

  [[nodiscard]] const Level* find(Price p) const noexcept {
    const ConstIter it = locate(levels_.begin(), levels_.end(), p);
    return it != levels_.begin() && (it - 1)->price == p ? &(it - 1)->level : nullptr;
  }

  /// The returned reference is invalidated by the next insert on this side.
  Level& insert(Price p) {
    const Iter it = locate(levels_.begin(), levels_.end(), p);
    if (it != levels_.begin() && (it - 1)->price == p) return (it - 1)->level;
    return levels_.insert(it, Entry{p, Level{}})->level;
  }

  void erase(Price p) noexcept {
    const Iter it = locate(levels_.begin(), levels_.end(), p);
    assert(it != levels_.begin() && (it - 1)->price == p);
    levels_.erase(it - 1);
  }

  void erase_best() noexcept { levels_.pop_back(); }

  /// Finding a level means searching, which is the access a prefetch would try to hide.
  void prefetch(Price) const noexcept {}

  template <class F>
  void for_each(F&& f) const {
    for (auto it = levels_.rbegin(); it != levels_.rend(); ++it)
      if (!f(it->price, it->level)) return;
  }

 private:
  static constexpr bool better(Price a, Price b) noexcept {
    if constexpr (S == Side::Buy) {
      return a > b;
    } else {
      return a < b;
    }
  }

  /// The first position whose level is strictly better than `p`. If `p` is present it is the
  /// element just before that position.
  template <class It>
  static It locate(It first, It last, Price p) noexcept {
    It it = last;
    for (int i = 0; i < kLinearScan && it != first; ++i, --it)
      if (!better((it - 1)->price, p)) return it;
    return std::partition_point(first, it, [p](const Entry& e) { return !better(e.price, p); });
  }

  std::vector<Entry> levels_;
};

struct VectorBookTraits {
  template <Side S>
  using Ladder = VectorLadder<S>;
  using Index = FlatOrderMap;
};

}  // namespace lob
