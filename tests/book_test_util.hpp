#pragma once

#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <vector>

#include "lob/events.hpp"
#include "lob/map_ladder.hpp"
#include "lob/messages.hpp"
#include "lob/order.hpp"
#include "lob/order_book.hpp"
#include "lob/vector_ladder.hpp"

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

inline NewOrder ioc(OrderId id, Side side, Price price, Qty qty, OwnerId owner = 0) {
  NewOrder o = limit(id, side, price, qty, owner);
  o.tif = TimeInForce::IOC;
  return o;
}

inline NewOrder fok(OrderId id, Side side, Price price, Qty qty, OwnerId owner = 0) {
  NewOrder o = limit(id, side, price, qty, owner);
  o.tif = TimeInForce::FOK;
  return o;
}

inline NewOrder market(OrderId id, Side side, Qty qty, OwnerId owner = 0) {
  NewOrder o = limit(id, side, 0, qty, owner);
  o.type = OrderType::Market;
  o.tif = TimeInForce::IOC;
  return o;
}

inline NewOrder post_only(OrderId id, Side side, Price price, Qty qty, OwnerId owner = 0) {
  NewOrder o = limit(id, side, price, qty, owner);
  o.post_only = true;
  return o;
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

/// Every storage policy. Behavioural tests run once per policy; they must be indistinguishable.
using AllBookTraits = ::testing::Types<DenseBookTraits, MapBookTraits, VectorBookTraits>;

struct BookTraitsName {
  template <class T>
  static std::string GetName(int) {
    if constexpr (std::is_same_v<T, DenseBookTraits>) {
      return "Dense";
    } else if constexpr (std::is_same_v<T, MapBookTraits>) {
      return "Map";
    } else {
      return "Vector";
    }
  }
};

}  // namespace lob::test
