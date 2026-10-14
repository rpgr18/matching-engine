#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "lob/hierarchical_bitset.hpp"
#include "lob/order.hpp"

namespace lob {

/// One side of the book as a flat array indexed by tick: slot i is price min_price + i.
///
/// Finding, creating and erasing a level are O(1) array accesses. The only non-constant
/// operation is finding the next level after the best one empties, which the occupancy bitset
/// answers in O(log64 n) without touching the empty slots in between.
///
/// The cost is memory proportional to the price band rather than to the number of live levels,
/// so the band has to be bounded. Real venues bound it anyway with price collars; prices outside
/// the band are rejected by the book before they reach the ladder.
template <Side S>
class DenseLadder {
 public:
  /// Widest band accepted: 2^25 ticks is 768 MiB of levels per side.
  static constexpr std::uint64_t kMaxLevels = std::uint64_t{1} << 25;

  DenseLadder(Price min_price, Price max_price)
      : min_(min_price), levels_(span(min_price, max_price)), occupied_(levels_.size()) {}

  [[nodiscard]] bool empty() const noexcept { return best_ == npos; }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }

  [[nodiscard]] Price best_price() const noexcept {
    assert(!empty());
    return price_of(best_);
  }
  [[nodiscard]] Level& best_level() noexcept {
    assert(!empty());
    return levels_[best_];
  }

  /// The populated level at `p`.
  [[nodiscard]] Level& at(Price p) noexcept {
    assert(occupied_.test(index_of(p)));
    return levels_[index_of(p)];
  }

  /// The level at `p`, or nullptr if it is empty or outside the band.
  [[nodiscard]] const Level* find(Price p) const noexcept {
    if (p < min_) return nullptr;
    const std::uint64_t i = static_cast<std::uint64_t>(p) - static_cast<std::uint64_t>(min_);
    if (i >= levels_.size()) return nullptr;
    return occupied_.test(i) ? &levels_[i] : nullptr;
  }

  /// The level at `p`, populating it if necessary.
  Level& insert(Price p) noexcept {
    const std::size_t i = index_of(p);
    if (!occupied_.test(i)) {
      occupied_.set(i);
      ++count_;
      if (best_ == npos || better(i, best_)) best_ = i;
    }
    return levels_[i];
  }

  /// Unpopulates the level at `p`, which must hold no orders.
  void erase(Price p) noexcept {
    const std::size_t i = index_of(p);
    assert(levels_[i].count == 0);
    levels_[i] = Level{};
    occupied_.reset(i);
    --count_;
    if (i == best_) best_ = next_worse(i);
  }

  void erase_best() noexcept { erase(best_price()); }

  /// Starts fetching the level for `p`. A hint only; any price is safe.
  void prefetch(Price p) const noexcept {
    if (p < min_) return;
    const std::uint64_t i = static_cast<std::uint64_t>(p) - static_cast<std::uint64_t>(min_);
    if (i < levels_.size()) __builtin_prefetch(&levels_[i]);
  }

  /// Visits populated levels from best to worst while `f(price, level)` returns true.
  template <class F>
  void for_each(F&& f) const {
    for (std::size_t i = best_; i != npos; i = next_worse(i))
      if (!f(price_of(i), std::as_const(levels_[i]))) return;
  }

 private:
  static constexpr std::size_t npos = HierarchicalBitset::npos;

  static std::size_t span(Price lo, Price hi) {
    if (hi < lo) throw std::invalid_argument("DenseLadder: max_price < min_price");
    const std::uint64_t n = static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo) + 1;
    if (n == 0 || n > kMaxLevels) throw std::invalid_argument("DenseLadder: price band too wide");
    return static_cast<std::size_t>(n);
  }

  static constexpr bool better(std::size_t a, std::size_t b) noexcept {
    if constexpr (S == Side::Buy) {
      return a > b;
    } else {
      return a < b;
    }
  }

  [[nodiscard]] std::size_t next_worse(std::size_t i) const noexcept {
    if constexpr (S == Side::Buy) {
      return i == 0 ? npos : occupied_.find_prev(i - 1);
    } else {
      return occupied_.find_next(i + 1);
    }
  }

  [[nodiscard]] std::size_t index_of(Price p) const noexcept {
    assert(p >= min_);
    return static_cast<std::size_t>(p - min_);
  }
  [[nodiscard]] Price price_of(std::size_t i) const noexcept {
    return min_ + static_cast<Price>(i);
  }

  Price min_;
  std::vector<Level> levels_;
  HierarchicalBitset occupied_;
  std::size_t best_ = npos;
  std::size_t count_ = 0;
};

}  // namespace lob
