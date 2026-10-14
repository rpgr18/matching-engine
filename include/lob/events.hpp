#pragma once

#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "lob/messages.hpp"
#include "lob/types.hpp"

namespace lob {

enum class EventKind : std::uint8_t { Accept, Reject, Trade, Cancel, Modify, Level };

/// A flat, comparable record of one listener callback. Used to capture and diff event streams in
/// tests and to carry events across thread boundaries.
struct Event {
  EventKind kind = EventKind::Accept;
  Side side = Side::Buy;      ///< Trade: aggressor side. Level: book side.
  std::uint8_t reason = 0;    ///< Reject: RejectReason. Cancel: CancelReason.
  OrderId id = 0;             ///< Trade: aggressor.
  OrderId other = 0;          ///< Trade: resting order.
  Price price = 0;
  Qty qty = 0;                ///< Trade: fill. Cancel: removed quantity. Modify: new quantity.
  Qty leaves = 0;             ///< Trade: aggressor leaves.
  Qty other_leaves = 0;       ///< Trade: resting leaves.
  std::uint32_t count = 0;    ///< Level: order count.
  std::uint64_t total = 0;    ///< Level: total open quantity.

  friend bool operator==(const Event&, const Event&) = default;

  static Event accept(OrderId id) {
    Event e;
    e.kind = EventKind::Accept;
    e.id = id;
    return e;
  }
  static Event reject(OrderId id, RejectReason r) {
    Event e;
    e.kind = EventKind::Reject;
    e.id = id;
    e.reason = static_cast<std::uint8_t>(r);
    return e;
  }
  static Event trade(const Trade& t) {
    Event e;
    e.kind = EventKind::Trade;
    e.side = t.aggressor_side;
    e.id = t.aggressor;
    e.other = t.resting;
    e.price = t.price;
    e.qty = t.qty;
    e.leaves = t.aggressor_leaves;
    e.other_leaves = t.resting_leaves;
    return e;
  }
  static Event cancel(OrderId id, Qty qty, CancelReason r) {
    Event e;
    e.kind = EventKind::Cancel;
    e.id = id;
    e.qty = qty;
    e.reason = static_cast<std::uint8_t>(r);
    return e;
  }
  static Event modify(OrderId id, Price price, Qty qty) {
    Event e;
    e.kind = EventKind::Modify;
    e.id = id;
    e.price = price;
    e.qty = qty;
    return e;
  }
  static Event level(Side side, Price price, std::uint64_t total, std::uint32_t count) {
    Event e;
    e.kind = EventKind::Level;
    e.side = side;
    e.price = price;
    e.total = total;
    e.count = count;
    return e;
  }
};

inline std::ostream& operator<<(std::ostream& os, const Event& e) {
  switch (e.kind) {
    case EventKind::Accept:
      return os << "ACCEPT id=" << e.id;
    case EventKind::Reject:
      return os << "REJECT id=" << e.id << ' '
                << to_string(static_cast<RejectReason>(e.reason));
    case EventKind::Trade:
      return os << "TRADE " << to_string(e.side) << " aggressor=" << e.id << " resting=" << e.other
                << ' ' << e.qty << '@' << e.price << " leaves=" << e.leaves << '/'
                << e.other_leaves;
    case EventKind::Cancel:
      return os << "CANCEL id=" << e.id << " qty=" << e.qty << ' '
                << to_string(static_cast<CancelReason>(e.reason));
    case EventKind::Modify:
      return os << "MODIFY id=" << e.id << ' ' << e.qty << '@' << e.price;
    case EventKind::Level:
      return os << "LEVEL " << to_string(e.side) << ' ' << e.price << " total=" << e.total
                << " count=" << e.count;
  }
  return os << "?";
}

inline std::string to_string(const Event& e) {
  std::ostringstream os;
  os << e;
  return os.str();
}

/// Listener that records every callback as an Event.
struct EventRecorder {
  std::vector<Event> events;

  void on_accept(OrderId id) { events.push_back(Event::accept(id)); }
  void on_reject(OrderId id, RejectReason r) { events.push_back(Event::reject(id, r)); }
  void on_trade(const Trade& t) { events.push_back(Event::trade(t)); }
  void on_cancel(OrderId id, Qty qty, CancelReason r) {
    events.push_back(Event::cancel(id, qty, r));
  }
  void on_modify(OrderId id, Price price, Qty qty) {
    events.push_back(Event::modify(id, price, qty));
  }
  void on_level(Side side, Price price, std::uint64_t total, std::uint32_t count) {
    events.push_back(Event::level(side, price, total, count));
  }

  /// Returns the events recorded so far and starts a fresh recording.
  std::vector<Event> take() {
    std::vector<Event> out;
    out.swap(events);
    return out;
  }
};

static_assert(BookListener<EventRecorder>);

}  // namespace lob
