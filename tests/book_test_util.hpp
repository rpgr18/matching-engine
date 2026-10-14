#pragma once

#include <vector>

#include "lob/events.hpp"
#include "lob/messages.hpp"
#include "lob/order.hpp"

namespace lob::test {

inline NewOrder limit(OrderId id, Side side, Price price, Qty qty, OwnerId owner = 0) {
  NewOrder o;
  o.id = id;
  o.side = side;
  o.price = price;
  o.qty = qty;
  o.owner = owner;
  return o;
}

inline NewOrder buy(OrderId id, Price price, Qty qty, OwnerId owner = 0) {
  return limit(id, Side::Buy, price, qty, owner);
}

inline NewOrder sell(OrderId id, Price price, Qty qty, OwnerId owner = 0) {
  return limit(id, Side::Sell, price, qty, owner);
}

using Events = std::vector<Event>;

/// Every resting order of one side in priority order, as (id, price, qty) triples.
struct Resting {
  OrderId id;
  Price price;
  Qty qty;
  friend bool operator==(const Resting&, const Resting&) = default;
};

template <class Book>
std::vector<Resting> resting(const Book& book, Side side) {
  std::vector<Resting> out;
  book.for_each_order(side, [&](const Order& o) { out.push_back({o.id, o.price, o.qty}); });
  return out;
}

inline std::ostream& operator<<(std::ostream& os, const Resting& r) {
  return os << '#' << r.id << ' ' << r.qty << '@' << r.price;
}

}  // namespace lob::test
