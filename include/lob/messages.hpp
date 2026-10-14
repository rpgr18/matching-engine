#pragma once

#include <concepts>
#include <cstdint>

#include "lob/types.hpp"

namespace lob {

/// An order entry request.
struct NewOrder {
  OrderId id = 0;
  Side side = Side::Buy;
  Price price = 0;  ///< Limit price in ticks. Ignored for market orders.
  Qty qty = 0;
  OwnerId owner = 0;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::GTC;
  bool post_only = false;  ///< Reject instead of taking liquidity.
};

/// One fill between an incoming (aggressor) order and a resting order. The trade price is always
/// the resting order's price, so the aggressor receives any price improvement.
struct Trade {
  OrderId aggressor = 0;
  OrderId resting = 0;
  Price price = 0;
  Qty qty = 0;
  Qty aggressor_leaves = 0;  ///< Aggressor's open quantity after this fill.
  Qty resting_leaves = 0;    ///< Resting order's open quantity after this fill.
  Side aggressor_side = Side::Buy;
};

/// The event interface a book reports through. The book takes the listener as a template
/// parameter, so every callback is a direct, inlinable call; a listener that ignores an event
/// costs nothing.
///
/// Within one request, execution reports (accept, trade, cancel, modify) come before the
/// market-by-price level update they cause, and each touched level is reported once, with its
/// final state, rather than once per fill.
template <class L>
concept BookListener = requires(L& l, OrderId id, const Trade& trade, Side side, Price price,
                                Qty qty, std::uint64_t total, std::uint32_t count,
                                RejectReason reject, CancelReason cancel) {
  l.on_accept(id);                       // passed validation; precedes any fills
  l.on_reject(id, reject);               // refused; the book is unchanged
  l.on_trade(trade);                     // one fill
  l.on_cancel(id, qty, cancel);          // `qty` open quantity removed without trading
  l.on_modify(id, price, qty);           // cancel/replace accepted with the new terms
  l.on_level(side, price, total, count); // level now holds `total` across `count` orders
};

/// Discards every event.
struct NullListener {
  void on_accept(OrderId) noexcept {}
  void on_reject(OrderId, RejectReason) noexcept {}
  void on_trade(const Trade&) noexcept {}
  void on_cancel(OrderId, Qty, CancelReason) noexcept {}
  void on_modify(OrderId, Price, Qty) noexcept {}
  void on_level(Side, Price, std::uint64_t, std::uint32_t) noexcept {}
};

static_assert(BookListener<NullListener>);

}  // namespace lob
