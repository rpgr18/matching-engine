#pragma once

#include <cstdint>
#include <type_traits>

#include "lob/messages.hpp"
#include "lob/order.hpp"
#include "lob/types.hpp"

namespace lob {

enum class CommandType : std::uint8_t { Add = 1, Cancel = 2, Modify = 3 };

/// A request in fixed-size binary form: the record format of replay files and the unit carried by
/// the inbound queue. Exactly 32 bytes, so two fit in a 64-byte cache line.
struct Command {
  OrderId id = 0;
  Price price = 0;
  Qty qty = 0;
  OwnerId owner = 0;
  SymbolId symbol = 0;
  CommandType type = CommandType::Add;
  Side side = Side::Buy;
  OrderType order_type = OrderType::Limit;
  TimeInForce tif = TimeInForce::GTC;
  std::uint8_t flags = 0;  ///< order_flags::kPostOnly
  std::uint8_t reserved = 0;

  static Command add(const NewOrder& o, SymbolId symbol = 0) {
    Command c;
    c.type = CommandType::Add;
    c.id = o.id;
    c.side = o.side;
    c.price = o.price;
    c.qty = o.qty;
    c.owner = o.owner;
    c.order_type = o.type;
    c.tif = o.tif;
    c.flags = o.post_only ? order_flags::kPostOnly : 0;
    c.symbol = symbol;
    return c;
  }
  static Command cancel(OrderId id, SymbolId symbol = 0) {
    Command c;
    c.type = CommandType::Cancel;
    c.id = id;
    c.symbol = symbol;
    return c;
  }
  static Command modify(OrderId id, Price price, Qty qty, SymbolId symbol = 0) {
    Command c;
    c.type = CommandType::Modify;
    c.id = id;
    c.price = price;
    c.qty = qty;
    c.symbol = symbol;
    return c;
  }

  [[nodiscard]] NewOrder to_new_order() const noexcept {
    NewOrder o;
    o.id = id;
    o.side = side;
    o.price = price;
    o.qty = qty;
    o.owner = owner;
    o.type = order_type;
    o.tif = tif;
    o.post_only = (flags & order_flags::kPostOnly) != 0;
    return o;
  }

  friend bool operator==(const Command&, const Command&) = default;
};

static_assert(sizeof(Command) == 32);
static_assert(std::is_trivially_copyable_v<Command>);

/// Applies one command to anything with the OrderBook request interface.
template <class Book>
inline void apply(Book& book, const Command& c) {
  switch (c.type) {
    case CommandType::Add:
      book.add(c.to_new_order());
      return;
    case CommandType::Cancel:
      book.cancel(c.id);
      return;
    case CommandType::Modify:
      book.modify(c.id, c.price, c.qty);
      return;
  }
}

}  // namespace lob
