#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "book_test_util.hpp"
#include "lob/events.hpp"
#include "lob/messages.hpp"
#include "lob/order_book.hpp"

namespace lob::test {

/// An executable specification of the matching rules, written for obviousness rather than speed:
/// a std::map of std::list per side, aggregates recomputed from scratch whenever they are
/// reported, and no shared code with OrderBook beyond the message types.
///
/// Two rules are formulated differently on purpose, so the two implementations cannot share a
/// mistake in them:
/// - Market orders are matched with an explicit "no limit" flag rather than an extreme price.
/// - Fill-or-kill is decided by matching against a copy of the book and checking whether the copy
///   filled the order in full, rather than by summing available liquidity.
class ReferenceBook {
 public:
  ReferenceBook(const BookConfig& config, std::vector<Event>& out) : cfg_(config), out_(&out) {}

  void add(const NewOrder& o) {
    if (o.id == 0) return reject(o.id, RejectReason::InvalidOrderId);
    if (o.qty == 0) return reject(o.id, RejectReason::InvalidQuantity);
    const bool market = o.type == OrderType::Market;
    if (o.post_only && (market || o.tif != TimeInForce::GTC))
      return reject(o.id, RejectReason::InvalidOrderType);
    if (!market && !in_band(o.price)) return reject(o.id, RejectReason::PriceOutOfBand);
    if (where_.contains(o.id)) return reject(o.id, RejectReason::DuplicateOrderId);
    const bool rests = !market && o.tif == TimeInForce::GTC;
    if (rests && where_.size() >= cfg_.max_orders) return reject(o.id, RejectReason::BookFull);
    if (o.post_only && marketable(o.side, o.price))
      return reject(o.id, RejectReason::WouldCross);

    emit(Event::accept(o.id));

    if (o.tif == TimeInForce::FOK) {
      ReferenceBook trial(*this);
      std::vector<Event> discard;
      trial.out_ = &discard;
      if (trial.match(o.id, o.side, market, o.price, o.owner, o.qty).first != 0)
        return emit(Event::cancel(o.id, o.qty, CancelReason::FillOrKill));
    }

    const auto [left, self_trade] = match(o.id, o.side, market, o.price, o.owner, o.qty);
    if (left == 0) return;
    if (self_trade) return emit(Event::cancel(o.id, left, CancelReason::SelfTrade));
    if (!rests) return emit(Event::cancel(o.id, left, CancelReason::Unfilled));
    rest(o.side, o.price, Ord{o.id, left, o.owner, o.post_only});
  }

  void cancel(OrderId id) {
    const auto it = where_.find(id);
    if (it == where_.end()) return reject(id, RejectReason::UnknownOrder);
    const auto [side, price] = it->second;
    const Ord o = take(side, price, id);
    emit(Event::cancel(id, o.qty, CancelReason::Requested));
    report(side, price);
    prune(side, price);
  }

  void modify(OrderId id, Price price, Qty qty) {
    const auto it = where_.find(id);
    if (it == where_.end()) return reject(id, RejectReason::UnknownOrder);
    if (qty == 0) return reject(id, RejectReason::InvalidQuantity);
    if (!in_band(price)) return reject(id, RejectReason::PriceOutOfBand);
    const auto [side, old_price] = it->second;
    Queue& q = side_of(side)[old_price];
    auto pos = find_in(q, id);

    if (price == old_price) {
      if (qty <= pos->qty) {
        pos->qty = qty;
      } else {
        Ord moved = *pos;
        moved.qty = qty;
        q.erase(pos);
        q.push_back(moved);
      }
      emit(Event::modify(id, price, qty));
      report(side, price);
      return;
    }

    if (pos->post_only && marketable(side, price)) return reject(id, RejectReason::WouldCross);

    emit(Event::modify(id, price, qty));
    Ord o = take(side, old_price, id);
    report(side, old_price);
    prune(side, old_price);

    const auto [left, self_trade] = match(id, side, false, price, o.owner, qty);
    if (left == 0) return;
    if (self_trade) return emit(Event::cancel(id, left, CancelReason::SelfTrade));
    o.qty = left;
    rest(side, price, o);
  }

  void insert(const NewOrder& o) {
    if (o.id == 0) return reject(o.id, RejectReason::InvalidOrderId);
    if (o.qty == 0) return reject(o.id, RejectReason::InvalidQuantity);
    if (o.type != OrderType::Limit) return reject(o.id, RejectReason::InvalidOrderType);
    if (!in_band(o.price)) return reject(o.id, RejectReason::PriceOutOfBand);
    if (where_.contains(o.id)) return reject(o.id, RejectReason::DuplicateOrderId);
    if (where_.size() >= cfg_.max_orders) return reject(o.id, RejectReason::BookFull);
    emit(Event::accept(o.id));
    rest(o.side, o.price, Ord{o.id, o.qty, o.owner, o.post_only});
  }

  void reduce(OrderId id, Qty qty) {
    const auto it = where_.find(id);
    if (it == where_.end()) return reject(id, RejectReason::UnknownOrder);
    if (qty == 0) return reject(id, RejectReason::InvalidQuantity);
    const auto [side, price] = it->second;
    const auto pos = find_in(side_of(side)[price], id);
    if (qty >= pos->qty) return cancel(id);
    pos->qty -= qty;
    emit(Event::modify(id, price, pos->qty));
    report(side, price);
  }

  // ---- Snapshot ----

  [[nodiscard]] std::vector<Resting> resting(Side side) const {
    std::vector<Resting> out;
    auto add_level = [&](Price p, const Queue& q) {
      for (const Ord& o : q) out.push_back({o.id, p, o.qty});
    };
    if (side == Side::Buy) {
      for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) add_level(it->first, it->second);
    } else {
      for (const auto& [p, q] : asks_) add_level(p, q);
    }
    return out;
  }

  [[nodiscard]] std::optional<Price> best(Side side) const {
    if (side == Side::Buy) {
      return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.rbegin()->first);
    }
    return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
  }

  [[nodiscard]] std::size_t order_count() const { return where_.size(); }
  [[nodiscard]] std::size_t level_count(Side side) const { return side_of(side).size(); }

 private:
  struct Ord {
    OrderId id;
    Qty qty;
    OwnerId owner;
    bool post_only;
  };
  using Queue = std::list<Ord>;
  using Ladder = std::map<Price, Queue>;  // ascending for both sides

  void emit(const Event& e) { out_->push_back(e); }
  void reject(OrderId id, RejectReason r) { emit(Event::reject(id, r)); }

  [[nodiscard]] bool in_band(Price p) const { return p >= cfg_.min_price && p <= cfg_.max_price; }

  Ladder& side_of(Side s) { return s == Side::Buy ? bids_ : asks_; }
  [[nodiscard]] const Ladder& side_of(Side s) const { return s == Side::Buy ? bids_ : asks_; }

  static bool crosses(Side aggressor, Price limit, Price resting) {
    return aggressor == Side::Buy ? limit >= resting : limit <= resting;
  }

  /// True if a limit order would trade immediately on arrival.
  [[nodiscard]] bool marketable(Side side, Price limit) const {
    const auto b = best(opposite(side));
    return b && crosses(side, limit, *b);
  }

  static Queue::iterator find_in(Queue& q, OrderId id) {
    for (auto it = q.begin(); it != q.end(); ++it)
      if (it->id == id) return it;
    return q.end();
  }

  Ord take(Side side, Price price, OrderId id) {
    Queue& q = side_of(side)[price];
    const auto it = find_in(q, id);
    const Ord o = *it;
    q.erase(it);
    where_.erase(id);
    return o;
  }

  void rest(Side side, Price price, const Ord& o) {
    side_of(side)[price].push_back(o);
    where_[o.id] = {side, price};
    report(side, price);
  }

  void report(Side side, Price price) {
    const Queue& q = side_of(side)[price];
    std::uint64_t total = 0;
    for (const Ord& o : q) total += o.qty;
    emit(Event::level(side, price, total, static_cast<std::uint32_t>(q.size())));
  }

  void prune(Side side, Price price) {
    Ladder& l = side_of(side);
    const auto it = l.find(price);
    if (it != l.end() && it->second.empty()) l.erase(it);
  }

  /// Returns (unfilled quantity, stopped by self-trade prevention).
  std::pair<Qty, bool> match(OrderId id, Side side, bool market, Price limit, OwnerId owner,
                             Qty qty) {
    const Side other = opposite(side);
    const bool stp = cfg_.stp != StpMode::None && owner != 0;
    while (qty > 0) {
      const auto bp = best(other);
      if (!bp || (!market && !crosses(side, limit, *bp))) break;
      const Price px = *bp;
      Queue& q = side_of(other)[px];
      bool touched = false;
      bool stop = false;
      while (qty > 0 && !q.empty()) {
        Ord& r = q.front();
        if (stp && r.owner == owner) {
          if (cfg_.stp == StpMode::CancelAggressor) {
            stop = true;
            break;
          }
          emit(Event::cancel(r.id, r.qty, CancelReason::SelfTrade));
          where_.erase(r.id);
          q.pop_front();
          touched = true;
          if (cfg_.stp == StpMode::CancelBoth) {
            stop = true;
            break;
          }
          continue;
        }
        const Qty n = r.qty < qty ? r.qty : qty;
        qty -= n;
        r.qty -= n;
        touched = true;
        emit(Event::trade(Trade{id, r.id, px, n, qty, r.qty, side}));
        if (r.qty == 0) {
          where_.erase(r.id);
          q.pop_front();
        }
      }
      if (touched) report(other, px);
      prune(other, px);
      if (stop) return {qty, true};
    }
    return {qty, false};
  }

  BookConfig cfg_;
  std::vector<Event>* out_;
  Ladder bids_;
  Ladder asks_;
  std::map<OrderId, std::pair<Side, Price>> where_;
};

}  // namespace lob::test
