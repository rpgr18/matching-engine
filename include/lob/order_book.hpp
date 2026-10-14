#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

#include "lob/command.hpp"
#include "lob/dense_ladder.hpp"
#include "lob/flat_order_map.hpp"
#include "lob/messages.hpp"
#include "lob/order.hpp"
#include "lob/order_pool.hpp"
#include "lob/types.hpp"

namespace lob {

struct BookConfig {
  /// Price band, in ticks. Limit prices outside it are rejected.
  Price min_price = 1;
  Price max_price = 100'000;
  /// Capacity for resting orders.
  std::uint32_t max_orders = 1u << 20;
  /// Self-trade prevention, applied to orders with a non-zero owner.
  StpMode stp = StpMode::None;
};

/// Storage policy: which ladder holds each side's price levels and which map finds an order by
/// id. The default is a dense tick-indexed ladder and the flat open-addressing index.
struct DenseBookTraits {
  template <Side S>
  using Ladder = DenseLadder<S>;
  using Index = FlatOrderMap;
};

/// A single-instrument limit order book that matches with price-time priority.
///
/// Incoming orders trade against the opposite side best price first and, within a price, oldest
/// order first; each fill prints at the resting order's price. Whatever does not trade rests at
/// the back of its price level's queue.
///
/// Nothing on the request path allocates: resting orders live in a preallocated pool, orders at a
/// price form an intrusive FIFO, and the id index is a fixed-size open-addressing table. Every
/// outcome is reported synchronously through the Listener before the call returns.
template <BookListener Listener, class Traits = DenseBookTraits>
class OrderBook {
  using BidLadder = typename Traits::template Ladder<Side::Buy>;
  using AskLadder = typename Traits::template Ladder<Side::Sell>;
  using Index = typename Traits::Index;

 public:
  OrderBook(const BookConfig& config, Listener& listener)
      : config_(validated(config)),
        listener_(listener),
        pool_(config.max_orders),
        index_(config.max_orders),
        bids_(config.min_price, config.max_price),
        asks_(config.min_price, config.max_price) {}

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  /// Submits a new order.
  void add(const NewOrder& order) {
    if (order.side == Side::Buy) {
      add_impl<Side::Buy>(order);
    } else {
      add_impl<Side::Sell>(order);
    }
  }

  /// Cancels a resting order.
  void cancel(OrderId id) {
    const OrderIdx idx = lookup(id);
    if (idx == kNil) return listener_.on_reject(id, RejectReason::UnknownOrder);
    if (pool_[idx].side == Side::Buy) {
      cancel_impl<Side::Buy>(idx);
    } else {
      cancel_impl<Side::Sell>(idx);
    }
  }

  /// Changes a resting order's price and open quantity (cancel/replace).
  ///
  /// Reducing quantity at the same price keeps the order's place in the queue. Any other change
  /// costs it: more quantity sends it to the back of its level, and a new price sends it to the
  /// back of the new level, trading on the way in if the new price crosses. A replace that would
  /// let a post-only order take liquidity is rejected and the order is left as it was.
  void modify(OrderId id, Price price, Qty qty) {
    const OrderIdx idx = lookup(id);
    if (idx == kNil) return listener_.on_reject(id, RejectReason::UnknownOrder);
    if (qty == 0) return listener_.on_reject(id, RejectReason::InvalidQuantity);
    if (!in_band(price)) return listener_.on_reject(id, RejectReason::PriceOutOfBand);
    if (pool_[idx].side == Side::Buy) {
      modify_impl<Side::Buy>(idx, price, qty);
    } else {
      modify_impl<Side::Sell>(idx, price, qty);
    }
  }

  // ---- Mirroring another venue's book (see itch.hpp). ----
  //
  // A feed handler rebuilds an exchange's book from its market-by-order feed. It must reproduce
  // the book exactly as reported, not re-match it: real books lock and cross during auctions and
  // halts, and the exchange reports executions separately. These two operations rest and shrink
  // orders without ever matching.

  /// Rests an order at its price, at the back of its level, without matching, even if it locks
  /// or crosses the opposite side. Only limit orders can rest; time in force is ignored.
  void insert(const NewOrder& order) {
    if (order.side == Side::Buy) {
      insert_impl<Side::Buy>(order);
    } else {
      insert_impl<Side::Sell>(order);
    }
  }

  /// Removes `qty` from a resting order's open quantity without changing its queue position, as
  /// an execution or partial cancel does, and removes the order if nothing is left.
  void reduce(OrderId id, Qty qty) {
    const OrderIdx idx = lookup(id);
    if (idx == kNil) return listener_.on_reject(id, RejectReason::UnknownOrder);
    if (qty == 0) return listener_.on_reject(id, RejectReason::InvalidQuantity);
    if (pool_[idx].side == Side::Buy) {
      reduce_impl<Side::Buy>(idx, qty);
    } else {
      reduce_impl<Side::Sell>(idx, qty);
    }
  }

  // ---- Software-pipelining hooks for batch processing (see replay.hpp). ----
  //
  // Each starts memory accesses a later request will need and changes no state. They are meant
  // to be issued for the same request in order, a few requests apart, so that every stage's
  // input has arrived in cache by the time the stage reads it. The book can change between
  // stages, so every input is re-validated; a stale hint only wastes a fetch.

  /// Stage 0: fetch the index slot the request's id hashes to (a new order probes it for
  /// duplicates and may insert there).
  void prefetch_slot(const Command& c) const noexcept {
    if (c.id != 0) index_.prefetch(c.id);
  }

  /// Stage 1: look the id up (cheap once stage 0 has landed), fetch the resting order, and
  /// return its slot for stage 2. New orders skip this stage.
  [[nodiscard]] OrderIdx prefetch_order(const Command& c) const noexcept {
    if (c.type == CommandType::Add) return kNil;
    const OrderIdx idx = lookup(c.id);
    if (idx != kNil) __builtin_prefetch(&pool_[idx]);
    return idx;
  }

  /// Stage 2: read the order (cheap once stage 1 has landed) and fetch its queue neighbours and
  /// price level; for a new order, fetch the level at its limit price.
  void prefetch_neighbours(const Command& c, OrderIdx idx) const noexcept {
    if (c.type == CommandType::Add) {
      prefetch_level(c.side, c.price);
      return;
    }
    if (idx >= pool_.capacity()) return;
    const Order& o = pool_[idx];
    if (o.id != c.id) return;  // gone, or the slot was reused since stage 1
    if (o.prev < pool_.capacity()) __builtin_prefetch(&pool_[o.prev]);
    if (o.next < pool_.capacity()) __builtin_prefetch(&pool_[o.next]);
    prefetch_level(o.side, o.price);
    if (c.type == CommandType::Modify && c.price != o.price) prefetch_level(o.side, c.price);
  }

  // ---- Queries. None of these are on the matching path. ----

  [[nodiscard]] std::optional<Price> best_bid() const noexcept { return best<Side::Buy>(); }
  [[nodiscard]] std::optional<Price> best_ask() const noexcept { return best<Side::Sell>(); }

  [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }
  [[nodiscard]] std::size_t level_count(Side side) const noexcept {
    return side == Side::Buy ? bids_.size() : asks_.size();
  }

  /// The resting order with this id, or nullptr.
  [[nodiscard]] const Order* find(OrderId id) const noexcept {
    const OrderIdx idx = lookup(id);
    return idx == kNil ? nullptr : &pool_[idx];
  }

  /// Aggregate state of one level; zero total and count if the level is empty.
  [[nodiscard]] LevelInfo level(Side side, Price price) const noexcept {
    const Level* lvl = side == Side::Buy ? bids_.find(price) : asks_.find(price);
    return lvl ? LevelInfo{price, lvl->total, lvl->count} : LevelInfo{price, 0, 0};
  }

  /// Visits levels from best to worst while `f(const LevelInfo&)` returns true.
  template <class F>
  void for_each_level(Side side, F&& f) const {
    auto visit = [&](Price price, const Level& lvl) {
      return f(LevelInfo{price, lvl.total, lvl.count});
    };
    if (side == Side::Buy) {
      bids_.for_each(visit);
    } else {
      asks_.for_each(visit);
    }
  }

  /// The best `max_levels` levels of one side.
  [[nodiscard]] std::vector<LevelInfo> depth(Side side, std::size_t max_levels) const {
    std::vector<LevelInfo> out;
    if (max_levels == 0) return out;
    for_each_level(side, [&](const LevelInfo& info) {
      out.push_back(info);
      return out.size() < max_levels;
    });
    return out;
  }

  /// Visits every resting order of one side in priority order.
  template <class F>
  void for_each_order(Side side, F&& f) const {
    auto visit = [&](Price, const Level& lvl) {
      for (OrderIdx i = lvl.head; i != kNil; i = pool_[i].next) f(pool_[i]);
      return true;
    };
    if (side == Side::Buy) {
      bids_.for_each(visit);
    } else {
      asks_.for_each(visit);
    }
  }

  [[nodiscard]] const BookConfig& config() const noexcept { return config_; }

 private:
  struct MatchResult {
    Qty remaining;
    bool self_trade;  ///< Matching stopped on a self-trade; the remainder must be cancelled.
  };

  static const BookConfig& validated(const BookConfig& config) {
    if (config.max_price < config.min_price)
      throw std::invalid_argument("BookConfig: max_price < min_price");
    if (config.max_orders == kNil) throw std::invalid_argument("BookConfig: max_orders too large");
    return config;
  }

  template <Side S>
  auto& ladder() noexcept {
    if constexpr (S == Side::Buy) {
      return bids_;
    } else {
      return asks_;
    }
  }
  template <Side S>
  const auto& ladder() const noexcept {
    if constexpr (S == Side::Buy) {
      return bids_;
    } else {
      return asks_;
    }
  }

  template <Side S>
  [[nodiscard]] std::optional<Price> best() const noexcept {
    const auto& lad = ladder<S>();
    return lad.empty() ? std::nullopt : std::optional<Price>(lad.best_price());
  }

  /// True if an order on side S with limit `limit` can trade with a resting order at `resting`.
  template <Side S>
  static constexpr bool crosses(Price limit, Price resting) noexcept {
    if constexpr (S == Side::Buy) {
      return limit >= resting;
    } else {
      return limit <= resting;
    }
  }

  [[nodiscard]] bool in_band(Price p) const noexcept {
    return p >= config_.min_price && p <= config_.max_price;
  }

  [[nodiscard]] OrderIdx lookup(OrderId id) const noexcept {
    return id == 0 ? kNil : index_.find(id);
  }

  void prefetch_level(Side side, Price price) const noexcept {
    if (side == Side::Buy) {
      bids_.prefetch(price);
    } else {
      asks_.prefetch(price);
    }
  }

  template <Side S>
  void add_impl(const NewOrder& o) {
    if (o.id == 0) return listener_.on_reject(o.id, RejectReason::InvalidOrderId);
    if (o.qty == 0) return listener_.on_reject(o.id, RejectReason::InvalidQuantity);
    const bool market = o.type == OrderType::Market;
    if (o.post_only && (market || o.tif != TimeInForce::GTC))
      return listener_.on_reject(o.id, RejectReason::InvalidOrderType);
    if (!market && !in_band(o.price)) return listener_.on_reject(o.id, RejectReason::PriceOutOfBand);
    if (index_.contains(o.id)) return listener_.on_reject(o.id, RejectReason::DuplicateOrderId);
    // Market orders never rest, whatever their time in force says.
    const bool may_rest = !market && o.tif == TimeInForce::GTC;
    if (may_rest && pool_.full()) return listener_.on_reject(o.id, RejectReason::BookFull);
    if (o.post_only && would_cross<S>(o.price))
      return listener_.on_reject(o.id, RejectReason::WouldCross);

    listener_.on_accept(o.id);
    const Price limit = market ? kMarketLimit<S> : o.price;
    if (o.tif == TimeInForce::FOK && !fillable<S>(o.owner, limit, o.qty))
      return listener_.on_cancel(o.id, o.qty, CancelReason::FillOrKill);

    const MatchResult m = match<S>(o.id, o.owner, limit, o.qty);
    if (m.remaining == 0) return;
    if (m.self_trade) return listener_.on_cancel(o.id, m.remaining, CancelReason::SelfTrade);
    if (!may_rest) return listener_.on_cancel(o.id, m.remaining, CancelReason::Unfilled);

    const OrderIdx idx = pool_.allocate();
    const std::uint8_t flags = o.post_only ? order_flags::kPostOnly : 0;
    pool_[idx] = Order{o.id, o.price, m.remaining, kNil, kNil, o.owner, S, flags};
    index_.insert(o.id, idx);
    rest<S>(idx);
  }

  /// A market order is matched as a limit order at the most aggressive representable price.
  template <Side S>
  static constexpr Price kMarketLimit =
      S == Side::Buy ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min();

  /// True if a limit order on side S at `limit` would trade on arrival.
  template <Side S>
  [[nodiscard]] bool would_cross(Price limit) const noexcept {
    const auto& opp = ladder<opposite(S)>();
    return !opp.empty() && crosses<S>(limit, opp.best_price());
  }

  /// True if matching would fill `qty` in full at prices `limit` or better: the fill-or-kill
  /// precondition. Without self-trade prevention it walks level aggregates only, one step per
  /// level crossed. With it, the owner's own resting orders are not liquidity: they are skipped
  /// under cancel-resting and end the walk otherwise, exactly as they would end matching. A FOK
  /// that fails this check has no side effects at all, self-trade cancels included.
  template <Side S>
  [[nodiscard]] bool fillable(OwnerId owner, Price limit, Qty qty) const noexcept {
    const bool stp = config_.stp != StpMode::None && owner != 0;
    std::uint64_t needed = qty;
    bool ok = false;
    ladder<opposite(S)>().for_each([&](Price px, const Level& lvl) {
      if (!crosses<S>(limit, px)) return false;
      if (!stp) {
        if (lvl.total >= needed) {
          ok = true;
          return false;
        }
        needed -= lvl.total;
        return true;
      }
      for (OrderIdx i = lvl.head; i != kNil; i = pool_[i].next) {
        const Order& r = pool_[i];
        if (r.owner == owner) {
          if (config_.stp == StpMode::CancelResting) continue;
          return false;
        }
        if (r.qty >= needed) {
          ok = true;
          return false;
        }
        needed -= r.qty;
      }
      return true;
    });
    return ok;
  }

  /// Trades an incoming order on side S against the opposite side, best price first and oldest
  /// order first, until it is filled or the next level no longer crosses `limit`.
  template <Side S>
  MatchResult match(OrderId id, OwnerId owner, Price limit, Qty qty) {
    constexpr Side O = opposite(S);
    auto& opp = ladder<O>();
    const bool stp = config_.stp != StpMode::None && owner != 0;

    while (qty > 0 && !opp.empty()) {
      const Price px = opp.best_price();
      if (!crosses<S>(limit, px)) break;
      Level& lvl = opp.best_level();
      bool touched = false;
      bool stop = false;

      while (qty > 0 && lvl.head != kNil) {
        const OrderIdx ridx = lvl.head;
        Order& r = pool_[ridx];

        if (stp && r.owner == owner) [[unlikely]] {
          if (config_.stp == StpMode::CancelAggressor) {
            stop = true;
            break;
          }
          const OrderId rid = r.id;
          const Qty rqty = r.qty;
          unlink(r, lvl);
          index_.erase(rid);
          pool_.release(ridx);
          touched = true;
          listener_.on_cancel(rid, rqty, CancelReason::SelfTrade);
          if (config_.stp == StpMode::CancelBoth) {
            stop = true;
            break;
          }
          continue;
        }

        const Qty fill = std::min(qty, r.qty);
        qty -= fill;
        r.qty -= fill;
        lvl.total -= fill;
        touched = true;
        listener_.on_trade(Trade{id, r.id, px, fill, qty, r.qty, S});
        if (r.qty == 0) {
          lvl.head = r.next;
          if (lvl.head != kNil) {
            pool_[lvl.head].prev = kNil;
          } else {
            lvl.tail = kNil;
          }
          --lvl.count;
          index_.erase(r.id);
          pool_.release(ridx);
        }
      }

      if (touched) listener_.on_level(O, px, lvl.total, lvl.count);
      if (lvl.count == 0) opp.erase_best();
      if (stop) return MatchResult{qty, true};
    }
    return MatchResult{qty, false};
  }

  /// Places a pooled order at the back of its price level and reports the level.
  template <Side S>
  void rest(OrderIdx idx) {
    Order& o = pool_[idx];
    Level& lvl = ladder<S>().insert(o.price);
    append(idx, lvl);
    listener_.on_level(S, o.price, lvl.total, lvl.count);
  }

  /// Links a pooled order at the tail of `lvl`'s queue.
  void append(OrderIdx idx, Level& lvl) noexcept {
    Order& o = pool_[idx];
    o.prev = lvl.tail;
    o.next = kNil;
    if (lvl.tail != kNil) {
      pool_[lvl.tail].next = idx;
    } else {
      lvl.head = idx;
    }
    lvl.tail = idx;
    ++lvl.count;
    lvl.total += o.qty;
  }

  /// Removes a pooled order from its level's queue, leaving the level in place even if empty.
  void unlink(Order& o, Level& lvl) noexcept {
    if (o.prev != kNil) {
      pool_[o.prev].next = o.next;
    } else {
      lvl.head = o.next;
    }
    if (o.next != kNil) {
      pool_[o.next].prev = o.prev;
    } else {
      lvl.tail = o.prev;
    }
    --lvl.count;
    lvl.total -= o.qty;
  }

  template <Side S>
  void cancel_impl(OrderIdx idx) {
    Order& o = pool_[idx];
    auto& lad = ladder<S>();
    Level& lvl = lad.at(o.price);
    const OrderId id = o.id;
    const Price px = o.price;
    const Qty qty = o.qty;

    unlink(o, lvl);
    index_.erase(id);
    pool_.release(idx);

    listener_.on_cancel(id, qty, CancelReason::Requested);
    listener_.on_level(S, px, lvl.total, lvl.count);
    if (lvl.count == 0) lad.erase(px);
  }

  template <Side S>
  void insert_impl(const NewOrder& o) {
    if (o.id == 0) return listener_.on_reject(o.id, RejectReason::InvalidOrderId);
    if (o.qty == 0) return listener_.on_reject(o.id, RejectReason::InvalidQuantity);
    if (o.type != OrderType::Limit) return listener_.on_reject(o.id, RejectReason::InvalidOrderType);
    if (!in_band(o.price)) return listener_.on_reject(o.id, RejectReason::PriceOutOfBand);
    if (index_.contains(o.id)) return listener_.on_reject(o.id, RejectReason::DuplicateOrderId);
    if (pool_.full()) return listener_.on_reject(o.id, RejectReason::BookFull);
    listener_.on_accept(o.id);
    const OrderIdx idx = pool_.allocate();
    const std::uint8_t flags = o.post_only ? order_flags::kPostOnly : 0;
    pool_[idx] = Order{o.id, o.price, o.qty, kNil, kNil, o.owner, S, flags};
    index_.insert(o.id, idx);
    rest<S>(idx);
  }

  template <Side S>
  void reduce_impl(OrderIdx idx, Qty qty) {
    Order& o = pool_[idx];
    if (qty >= o.qty) return cancel_impl<S>(idx);
    Level& lvl = ladder<S>().at(o.price);
    o.qty -= qty;
    lvl.total -= qty;
    listener_.on_modify(o.id, o.price, o.qty);
    listener_.on_level(S, o.price, lvl.total, lvl.count);
  }

  template <Side S>
  void modify_impl(OrderIdx idx, Price price, Qty qty) {
    Order& o = pool_[idx];
    auto& own = ladder<S>();

    if (price == o.price) {
      // The book is never crossed, so a resting price cannot trade: no matching needed.
      Level& lvl = own.at(price);
      if (qty > o.qty) {
        unlink(o, lvl);
        o.qty = qty;
        append(idx, lvl);
      } else {
        lvl.total -= o.qty - qty;
        o.qty = qty;
      }
      listener_.on_modify(o.id, price, qty);
      listener_.on_level(S, price, lvl.total, lvl.count);
      return;
    }

    if ((o.flags & order_flags::kPostOnly) && would_cross<S>(price))
      return listener_.on_reject(o.id, RejectReason::WouldCross);

    const OrderId id = o.id;
    const Price old_price = o.price;
    listener_.on_modify(id, price, qty);
    Level& old_lvl = own.at(old_price);
    unlink(o, old_lvl);
    listener_.on_level(S, old_price, old_lvl.total, old_lvl.count);
    if (old_lvl.count == 0) own.erase(old_price);

    // The order stays allocated (and indexed) while it trades as an aggressor; it is linked into
    // no level, so it cannot match against itself.
    const MatchResult m = match<S>(id, o.owner, price, qty);
    if (m.remaining == 0 || m.self_trade) {
      index_.erase(id);
      pool_.release(idx);
      if (m.self_trade) listener_.on_cancel(id, m.remaining, CancelReason::SelfTrade);
      return;
    }
    o.price = price;
    o.qty = m.remaining;
    rest<S>(idx);
  }

  BookConfig config_;
  Listener& listener_;
  OrderPool pool_;
  Index index_;
  BidLadder bids_;
  AskLadder asks_;
};

}  // namespace lob
