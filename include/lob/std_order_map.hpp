#pragma once

#include <cstddef>
#include <unordered_map>

#include "lob/order.hpp"

namespace lob {

/// The order index as a std::unordered_map, with the same interface as FlatOrderMap. Exists so
/// benchmarks can measure what the flat table buys: unordered_map allocates a node per insert
/// and follows at least one pointer per lookup.
class StdOrderMap {
 public:
  explicit StdOrderMap(std::size_t max_entries) { map_.reserve(max_entries); }

  [[nodiscard]] OrderIdx find(OrderId key) const {
    const auto it = map_.find(key);
    return it == map_.end() ? kNil : it->second;
  }
  [[nodiscard]] bool contains(OrderId key) const { return map_.contains(key); }
  bool insert(OrderId key, OrderIdx value) { return map_.emplace(key, value).second; }
  bool erase(OrderId key) { return map_.erase(key) > 0; }
  void prefetch(OrderId) const noexcept {}
  [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

 private:
  std::unordered_map<OrderId, OrderIdx> map_;
};

}  // namespace lob
