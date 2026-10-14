#pragma once

#include <cassert>
#include <cstddef>
#include <functional>
#include <map>
#include <type_traits>

#include "lob/flat_order_map.hpp"
#include "lob/order.hpp"

namespace lob {

/// One side of the book as a red-black tree keyed by price, best price first.
///
/// The textbook design: memory proportional to live levels and no price band, at the cost of a
/// heap allocation per new level and pointer chasing on every lookup. Kept as the baseline the
/// dense ladder is measured against.
template <Side S>
class MapLadder {
  using Compare = std::conditional_t<S == Side::Buy, std::greater<Price>, std::less<Price>>;

 public:
  MapLadder(Price /*min_price*/, Price /*max_price*/) {}

  [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }

  [[nodiscard]] Price best_price() const noexcept {
    assert(!empty());
    return levels_.begin()->first;
  }
  [[nodiscard]] Level& best_level() noexcept {
    assert(!empty());
    return levels_.begin()->second;
  }

  [[nodiscard]] Level& at(Price p) noexcept {
    const auto it = levels_.find(p);
    assert(it != levels_.end());
    return it->second;
  }

  [[nodiscard]] const Level* find(Price p) const noexcept {
    const auto it = levels_.find(p);
    return it == levels_.end() ? nullptr : &it->second;
  }

  Level& insert(Price p) { return levels_.try_emplace(p).first->second; }

  void erase(Price p) noexcept { levels_.erase(p); }
  void erase_best() noexcept { levels_.erase(levels_.begin()); }

  /// Tree nodes cannot be located without walking the tree, so there is nothing to prefetch.
  void prefetch(Price) const noexcept {}

  template <class F>
  void for_each(F&& f) const {
    for (const auto& [price, level] : levels_)
      if (!f(price, level)) return;
  }

 private:
  std::map<Price, Level, Compare> levels_;
};

struct MapBookTraits {
  template <Side S>
  using Ladder = MapLadder<S>;
  using Index = FlatOrderMap;
};

}  // namespace lob
