#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include "lob/command.hpp"
#include "lob/events.hpp"
#include "lob/messages.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob {

/// The listener interface of a multi-instrument engine: the BookListener callbacks with the
/// instrument they concern as the first argument.
template <class L>
concept EngineListener = requires(L& l, SymbolId symbol, OrderId id, const Trade& trade,
                                  Side side, Price price, Qty qty, std::uint64_t total,
                                  std::uint32_t count, RejectReason reject, CancelReason cancel) {
  l.on_accept(symbol, id);
  l.on_reject(symbol, id, reject);
  l.on_trade(symbol, trade);
  l.on_cancel(symbol, id, qty, cancel);
  l.on_modify(symbol, id, price, qty);
  l.on_level(symbol, side, price, total, count);
};

/// Presents one book's events to an EngineListener, tagged with that book's symbol.
template <class L>
struct SymbolTagger {
  L* listener;
  SymbolId symbol;

  void on_accept(OrderId id) { listener->on_accept(symbol, id); }
  void on_reject(OrderId id, RejectReason r) { listener->on_reject(symbol, id, r); }
  void on_trade(const Trade& t) { listener->on_trade(symbol, t); }
  void on_cancel(OrderId id, Qty qty, CancelReason r) { listener->on_cancel(symbol, id, qty, r); }
  void on_modify(OrderId id, Price price, Qty qty) { listener->on_modify(symbol, id, price, qty); }
  void on_level(Side side, Price price, std::uint64_t total, std::uint32_t count) {
    listener->on_level(symbol, side, price, total, count);
  }
};

/// Routes requests to one OrderBook per instrument. Symbols are dense small integers assigned by
/// add_symbol(), so routing is a bounds check and an array index. Order ids are scoped to their
/// symbol, as on most venues' order entry protocols.
template <EngineListener Listener, class Traits = DenseBookTraits>
class Engine {
 public:
  using Book = OrderBook<SymbolTagger<Listener>, Traits>;

  explicit Engine(Listener& listener) : listener_(listener) {}

  /// Lists a new instrument and returns its symbol id.
  SymbolId add_symbol(const BookConfig& config) {
    if (books_.size() > std::numeric_limits<SymbolId>::max())
      throw std::length_error("Engine: symbol ids exhausted");
    const auto symbol = static_cast<SymbolId>(books_.size());
    books_.push_back(std::make_unique<Slot>(listener_, symbol, config));
    return symbol;
  }

  void apply(const Command& c) {
    if (c.symbol >= books_.size()) [[unlikely]] {
      listener_.on_reject(c.symbol, c.id, RejectReason::UnknownSymbol);
      return;
    }
    lob::apply(books_[c.symbol]->book, c);
  }

  [[nodiscard]] Book& book(SymbolId symbol) { return books_.at(symbol)->book; }
  [[nodiscard]] const Book& book(SymbolId symbol) const { return books_.at(symbol)->book; }
  [[nodiscard]] std::size_t symbols() const noexcept { return books_.size(); }

 private:
  /// A book and the tagger it reports through, allocated together so the book's reference to
  /// its listener stays valid as the table grows.
  struct Slot {
    Slot(Listener& l, SymbolId symbol, const BookConfig& config)
        : tagger{&l, symbol}, book(config, tagger) {}
    SymbolTagger<Listener> tagger;
    Book book;
  };

  Listener& listener_;
  std::vector<std::unique_ptr<Slot>> books_;
};

/// Records engine events, each tagged with its symbol.
struct EngineEventRecorder {
  std::vector<Event> events;

  void on_accept(SymbolId s, OrderId id) { events.push_back(Event::accept(id).on(s)); }
  void on_reject(SymbolId s, OrderId id, RejectReason r) {
    events.push_back(Event::reject(id, r).on(s));
  }
  void on_trade(SymbolId s, const Trade& t) { events.push_back(Event::trade(t).on(s)); }
  void on_cancel(SymbolId s, OrderId id, Qty qty, CancelReason r) {
    events.push_back(Event::cancel(id, qty, r).on(s));
  }
  void on_modify(SymbolId s, OrderId id, Price price, Qty qty) {
    events.push_back(Event::modify(id, price, qty).on(s));
  }
  void on_level(SymbolId s, Side side, Price price, std::uint64_t total, std::uint32_t count) {
    events.push_back(Event::level(side, price, total, count).on(s));
  }

  std::vector<Event> take() {
    std::vector<Event> out;
    out.swap(events);
    return out;
  }
};

}  // namespace lob
