#pragma once

// Rebuilding every instrument's order book from a NASDAQ TotalView-ITCH 5.0 feed.
//
// The feed handler is a thin translation onto the same books the matching engine uses, in their
// mirroring mode: adds become insert(), executions and partial cancels become reduce(), deletes
// become cancel(), and a replace becomes a cancel plus an insert at the back of the new level.
// Nothing is re-matched, so the books reproduce NASDAQ's exactly, crossed auctions included.
//
// Books are sized from a first pass over the feed (scan_capacity), which finds each instrument's
// peak number of live orders, so the second pass never allocates and never runs out.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lob/engine.hpp"
#include "lob/itch.hpp"
#include "lob/order_book.hpp"
#include "lob/vector_ladder.hpp"
#include "lob/workload.hpp"

namespace lob::itch {

/// What a feed needs from the books that mirror it.
struct Capacity {
  std::vector<std::uint32_t> peak_orders;  ///< Indexed by stock locate.
  std::vector<std::uint64_t> messages;     ///< Order messages per stock locate.
  std::uint64_t live_at_end = 0;
  std::uint64_t unknown_refs = 0;  ///< Order messages for references never added.
};

namespace detail {

class CapacityScanner : public NullHandler {
 public:
  Capacity result;

  CapacityScanner() { live_.reserve(1u << 20); }

  void on_add(const AddOrder& m) {
    count(m.h.locate);
    open(m.h.locate, m.ref, m.shares);
  }
  void on_executed(const OrderExecuted& m) {
    count(m.h.locate);
    shrink(m.ref, m.shares);
  }
  void on_cancel(const OrderCancel& m) {
    count(m.h.locate);
    shrink(m.ref, m.shares);
  }
  void on_delete(const OrderDelete& m) {
    count(m.h.locate);
    shrink(m.ref, ~std::uint32_t{0});
  }
  void on_replace(const OrderReplace& m) {
    count(m.h.locate);
    const auto it = live_.find(m.old_ref);
    if (it == live_.end()) {
      ++result.unknown_refs;
      return;
    }
    const std::uint16_t locate = it->second.first;
    close(it);
    open(locate, m.new_ref, m.shares);
  }

  void finish() { result.live_at_end = live_.size(); }

 private:
  using Live = std::unordered_map<std::uint64_t, std::pair<std::uint16_t, std::uint32_t>>;

  void grow(std::uint16_t locate) {
    if (result.peak_orders.size() <= locate) {
      result.peak_orders.resize(locate + std::size_t{1}, 0);
      result.messages.resize(locate + std::size_t{1}, 0);
      count_.resize(locate + std::size_t{1}, 0);
    }
  }

  void count(std::uint16_t locate) {
    grow(locate);
    ++result.messages[locate];
  }

  void open(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares) {
    grow(locate);
    if (!live_.emplace(ref, std::make_pair(locate, shares)).second) return;
    if (++count_[locate] > result.peak_orders[locate]) result.peak_orders[locate] = count_[locate];
  }

  void shrink(std::uint64_t ref, std::uint32_t shares) {
    const auto it = live_.find(ref);
    if (it == live_.end()) {
      ++result.unknown_refs;
      return;
    }
    if (shares >= it->second.second) {
      close(it);
    } else {
      it->second.second -= shares;
    }
  }

  void close(Live::iterator it) {
    --count_[it->second.first];
    live_.erase(it);
  }

  Live live_;
  std::vector<std::uint32_t> count_;
};

/// Counts rejects on the way to the real listener: in a mirror, every reject is a reference the
/// feed and the book disagree about.
template <class L>
struct RejectCounter {
  L* inner;
  std::uint64_t rejects = 0;

  void on_accept(SymbolId s, OrderId id) { inner->on_accept(s, id); }
  void on_reject(SymbolId s, OrderId id, RejectReason r) {
    ++rejects;
    inner->on_reject(s, id, r);
  }
  void on_trade(SymbolId s, const Trade& t) { inner->on_trade(s, t); }
  void on_cancel(SymbolId s, OrderId id, Qty q, CancelReason r) { inner->on_cancel(s, id, q, r); }
  void on_modify(SymbolId s, OrderId id, Price p, Qty q) { inner->on_modify(s, id, p, q); }
  void on_level(SymbolId s, Side side, Price p, std::uint64_t total, std::uint32_t count) {
    inner->on_level(s, side, p, total, count);
  }
};

}  // namespace detail

/// First pass over a feed: replays only reference bookkeeping to find each instrument's peak
/// number of simultaneously live orders.
[[nodiscard]] inline Capacity scan_capacity(const std::uint8_t* data, std::size_t size) {
  detail::CapacityScanner scanner;
  parse(data, size, scanner);
  scanner.finish();
  return scanner.result;
}

/// Engine listener that ignores everything.
struct NullEngineListener {
  void on_accept(SymbolId, OrderId) noexcept {}
  void on_reject(SymbolId, OrderId, RejectReason) noexcept {}
  void on_trade(SymbolId, const Trade&) noexcept {}
  void on_cancel(SymbolId, OrderId, Qty, CancelReason) noexcept {}
  void on_modify(SymbolId, OrderId, Price, Qty) noexcept {}
  void on_level(SymbolId, Side, Price, std::uint64_t, std::uint32_t) noexcept {}
};

/// Mirrors every instrument in a feed. The default ladder is the sorted vector: ITCH prices span
/// four decimal places and some resting orders sit absurdly far from the market, so a dense
/// tick-indexed ladder per instrument would need gigabytes. The vector costs memory only for
/// levels that exist and stays fast near the touch, where nearly all of the activity is.
template <EngineListener Listener = NullEngineListener, class Traits = VectorBookTraits>
class BookBuilder : public NullHandler {
 public:
  using Engine = lob::Engine<detail::RejectCounter<Listener>, Traits>;

  BookBuilder(Listener& listener, const Capacity& capacity, double headroom = 1.1)
      : counter_{&listener}, engine_(counter_) {
    static_assert(EngineListener<detail::RejectCounter<Listener>>);
    names_.resize(capacity.peak_orders.size());
    for (std::uint32_t peak : capacity.peak_orders) {
      BookConfig c;
      c.min_price = 0;
      c.max_price = 0xFFFF'FFFF;  // every representable ITCH price
      c.max_orders = 16 + static_cast<std::uint32_t>(peak * headroom);
      engine_.add_symbol(c);
    }
  }

  void on_stock_directory(const StockDirectory& m) {
    if (m.h.locate < names_.size()) names_[m.h.locate] = std::string(m.symbol());
  }

  void on_add(const AddOrder& m) {
    if (!known(m.h.locate)) return;
    NewOrder o;
    o.id = m.ref;
    o.side = m.side;
    o.price = m.price;
    o.qty = m.shares;
    engine_.book(m.h.locate).insert(o);
  }

  void on_executed(const OrderExecuted& m) {
    if (known(m.h.locate)) engine_.book(m.h.locate).reduce(m.ref, m.shares);
  }

  void on_cancel(const OrderCancel& m) {
    if (known(m.h.locate)) engine_.book(m.h.locate).reduce(m.ref, m.shares);
  }

  void on_delete(const OrderDelete& m) {
    if (known(m.h.locate)) engine_.book(m.h.locate).cancel(m.ref);
  }

  void on_replace(const OrderReplace& m) {
    if (!known(m.h.locate)) return;
    auto& book = engine_.book(m.h.locate);
    const Order* old = book.find(m.old_ref);
    if (old == nullptr) {
      ++unknown_;
      return;
    }
    NewOrder o;
    o.id = m.new_ref;
    o.side = old->side;
    o.price = m.price;
    o.qty = m.shares;
    book.cancel(m.old_ref);
    book.insert(o);
  }

  // The engine holds a reference to counter_, so a moved or copied builder would report to the
  // wrong object.
  BookBuilder(const BookBuilder&) = delete;
  BookBuilder& operator=(const BookBuilder&) = delete;

  [[nodiscard]] const Engine& engine() const noexcept { return engine_; }
  [[nodiscard]] const std::string& name(std::uint16_t locate) const { return names_.at(locate); }
  [[nodiscard]] std::size_t instruments() const noexcept { return engine_.symbols(); }

  /// Messages the books could not apply: unknown references, out-of-range locates, rejects. Zero
  /// for a complete, consistent feed.
  [[nodiscard]] std::uint64_t inconsistencies() const noexcept {
    return counter_.rejects + unknown_;
  }

 private:
  bool known(std::uint16_t locate) {
    if (locate < engine_.symbols()) return true;
    ++unknown_;
    return false;
  }

  detail::RejectCounter<Listener> counter_;
  Engine engine_;
  std::vector<std::string> names_;
  std::uint64_t unknown_ = 0;
};

// ---- Synthetic feeds ----------------------------------------------------------------------------

/// A synthetic ITCH feed and the ground truth it encodes.
struct SyntheticFeed {
  std::vector<std::uint8_t> bytes;
  std::uint64_t messages = 0;
  /// Per instrument (locate - 1): the ITCH reference each live order ends the day with.
  std::vector<std::unordered_map<OrderId, std::uint64_t>> ref_of;
};

/// Turns per-instrument request streams into the ITCH feed an exchange would publish for them.
///
/// Each stream is run through a matching book, and its outcome is written as the market-by-order
/// messages that describe it:
///   - an order that rests becomes 'A' with its resting size, after any 'E' it caused;
///   - a fill becomes 'E' against the resting order;
///   - a cancel of a resting order (requested or self-trade) becomes 'D';
///   - a size reduction in place becomes 'X';
///   - any other change to a resting order becomes 'U' with a fresh reference.
/// IOC remainders and rejects never touched the book and produce nothing. Instrument k gets locate
/// k + 1 and symbol "SYMk". Streams are interleaved request by request.
[[nodiscard]] inline SyntheticFeed synthesize(const std::vector<Workload>& streams) {
  struct Recorder {
    std::vector<Trade> trades;
    std::vector<OrderId> cancelled;
    void on_accept(OrderId) {}
    void on_reject(OrderId, RejectReason) {}
    void on_trade(const Trade& t) { trades.push_back(t); }
    void on_cancel(OrderId id, Qty, CancelReason) { cancelled.push_back(id); }
    void on_modify(OrderId, Price, Qty) {}
    void on_level(Side, Price, std::uint64_t, std::uint32_t) {}
  };
  struct Instrument {
    Recorder rec;
    std::unique_ptr<OrderBook<Recorder>> book;
    std::string symbol;
    std::uint16_t locate = 0;
    std::size_t next = 0;
  };

  SyntheticFeed feed;
  Writer w;
  std::uint64_t ts = 34'200'000'000'000;  // 09:30:00
  std::uint64_t match = 0;
  std::uint64_t next_ref = std::uint64_t{1} << 56;
  feed.ref_of.resize(streams.size());

  std::vector<Instrument> inst(streams.size());
  w.system_event(ts, 'O');
  for (std::size_t k = 0; k < streams.size(); ++k) {
    inst[k].book = std::make_unique<OrderBook<Recorder>>(book_config_for(streams[k].spec),
                                                         inst[k].rec);
    inst[k].symbol = "SYM" + std::to_string(k);
    inst[k].locate = static_cast<std::uint16_t>(k + 1);
    w.stock_directory(inst[k].locate, ++ts, inst[k].symbol);
  }

  for (bool any = true; any;) {
    any = false;
    for (std::size_t k = 0; k < streams.size(); ++k) {
      Instrument& in = inst[k];
      const auto& cmds = streams[k].commands;
      if (in.next == cmds.size()) continue;
      any = true;
      const Command& c = cmds[in.next++];
      auto& refs = feed.ref_of[k];
      const auto ref_for = [&](OrderId id) { return (std::uint64_t{k} << 40) | id; };

      const Order* before = in.book->find(c.id);
      // An add whose id is already resting is rejected and must not appear in the feed.
      const bool add_of_live_id = c.type == CommandType::Add && before != nullptr;
      const Price p0 = before ? before->price : 0;
      const Qty q0 = before ? before->qty : 0;
      in.rec.trades.clear();
      in.rec.cancelled.clear();
      apply(*in.book, c);

      for (const Trade& t : in.rec.trades) {
        w.executed(in.locate, ++ts, refs.at(t.resting), t.qty, ++match);
        if (t.resting_leaves == 0) refs.erase(t.resting);
      }
      for (OrderId id : in.rec.cancelled) {
        const auto it = refs.find(id);
        if (it == refs.end()) continue;  // never rested: an IOC or FOK remainder
        w.del(in.locate, ++ts, it->second);
        refs.erase(it);
      }

      const Order* after = in.book->find(c.id);
      if (c.type == CommandType::Add) {
        if (after && !add_of_live_id) {
          refs[c.id] = ref_for(c.id);
          w.add_order(in.locate, ++ts, refs[c.id], after->side, after->qty, in.symbol,
                      static_cast<std::uint32_t>(after->price));
        }
      } else if (c.type == CommandType::Modify && before) {
        const auto it = refs.find(c.id);
        if (after && it != refs.end()) {
          if (after->price == p0 && after->qty <= q0) {
            if (after->qty < q0) w.cancel(in.locate, ++ts, it->second, q0 - after->qty);
          } else {
            const std::uint64_t fresh = next_ref++;
            w.replace(in.locate, ++ts, it->second, fresh, after->qty,
                      static_cast<std::uint32_t>(after->price));
            it->second = fresh;
          }
        } else if (!after && it != refs.end()) {
          // Filled in full as it traded on the way to its new price.
          w.del(in.locate, ++ts, it->second);
          refs.erase(it);
        }
      }
    }
  }
  w.system_event(++ts, 'C');

  feed.bytes = w.take();
  // Count by walking the framing rather than tracking every write.
  ParseStats stats;
  NullHandler none;
  parse(feed.bytes.data(), feed.bytes.size(), none, &stats);
  feed.messages = stats.messages;
  return feed;
}

}  // namespace lob::itch
