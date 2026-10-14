#pragma once

#include <cstdint>
#include <limits>

#include "lob/types.hpp"

namespace lob {

/// Index of an order slot in the OrderPool. Links are 32-bit indices rather than pointers, which
/// keeps an Order at 40 bytes and makes the pool relocatable.
using OrderIdx = std::uint32_t;
inline constexpr OrderIdx kNil = std::numeric_limits<OrderIdx>::max();

namespace order_flags {
inline constexpr std::uint8_t kPostOnly = 1u << 0;
}  // namespace order_flags

/// A resting order. Orders at one price form an intrusive doubly linked FIFO: matching pops from
/// the head, new orders append at the tail, and a cancel unlinks in O(1) given the slot index.
struct Order {
  OrderId id;
  Price price;
  Qty qty;  ///< Open quantity.
  OrderIdx prev;
  OrderIdx next;  ///< Doubles as the free-list link while the slot is unused.
  OwnerId owner;
  Side side;
  std::uint8_t flags;
};

static_assert(sizeof(Order) == 40, "Order layout is part of the cache budget; keep it compact");

/// One price level: the FIFO of orders at that price plus running aggregates, so market data
/// (total size, order count) never requires walking the queue.
struct Level {
  OrderIdx head = kNil;
  OrderIdx tail = kNil;
  std::uint32_t count = 0;
  std::uint64_t total = 0;  ///< Sum of open quantity.
};

/// Read-only view of a level for queries and snapshots.
struct LevelInfo {
  Price price = 0;
  std::uint64_t total = 0;
  std::uint32_t count = 0;

  friend bool operator==(const LevelInfo&, const LevelInfo&) = default;
};

}  // namespace lob
