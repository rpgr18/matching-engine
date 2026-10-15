// The engine compiled to WebAssembly for the browser demo (web/index.html).
//
// One live book, driven by the same order-flow model the benchmarks use, that a visitor can also
// trade on. The interface is a handful of C functions over plain numbers so the JavaScript side
// needs no bindings library; anything bulky (depth, events, the trade tape) is written into
// static buffers that JavaScript reads straight out of the module's memory.
//
// Build: scripts/build_web.sh

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "lob/command.hpp"
#include "lob/events.hpp"
#include "lob/map_ladder.hpp"
#include "lob/order_book.hpp"
#include "lob/std_order_map.hpp"
#include "lob/workload.hpp"

namespace {

using namespace lob;

/// Visitor orders get ids far above the model's, so either side can tell them apart.
constexpr OrderId kUserBase = OrderId{1} << 40;
bool is_user(OrderId id) { return id >= kUserBase; }

struct Tape {
  double seq;
  double price;
  double qty;
  double aggressor_side;  // 0 buy, 1 sell
  double user_aggressor;
  double user_resting;
};

struct Demo;

struct Listener {
  Demo* demo;
  void on_accept(OrderId id);
  void on_reject(OrderId id, RejectReason r);
  void on_trade(const Trade& t);
  void on_cancel(OrderId id, Qty qty, CancelReason r);
  void on_modify(OrderId id, Price price, Qty qty);
  void on_level(Side, Price, std::uint64_t, std::uint32_t) {}
};

struct Demo {
  explicit Demo(const WorkloadSpec& s) : spec(s), model(s), book(book_config_for(s), listener) {}

  WorkloadSpec spec;
  FlowModel model;
  Listener listener{this};
  OrderBook<Listener> book;
  std::vector<Event> user_events;  // every event that concerns a visitor order
  std::vector<Tape> tape;          // trades since the last drain, newest last
  double requests = 0;
  double trades = 0;
  double volume = 0;
  double engine_ms = 0;
  double last_trade_price = 0;
  OrderId next_user = kUserBase;
};

std::unique_ptr<Demo> g;

void Listener::on_accept(OrderId id) {
  if (is_user(id)) demo->user_events.push_back(Event::accept(id));
}
void Listener::on_reject(OrderId id, RejectReason r) {
  if (is_user(id)) demo->user_events.push_back(Event::reject(id, r));
}
void Listener::on_trade(const Trade& t) {
  demo->model.on_trade(t);
  demo->trades += 1;
  demo->volume += t.qty;
  demo->last_trade_price = static_cast<double>(t.price);
  const bool ua = is_user(t.aggressor);
  const bool ur = is_user(t.resting);
  if (ua || ur) demo->user_events.push_back(Event::trade(t));
  if (demo->tape.size() < 4096)
    demo->tape.push_back(Tape{demo->trades, static_cast<double>(t.price), static_cast<double>(t.qty),
                              t.aggressor_side == Side::Buy ? 0.0 : 1.0, ua ? 1.0 : 0.0,
                              ur ? 1.0 : 0.0});
}
void Listener::on_cancel(OrderId id, Qty qty, CancelReason r) {
  demo->model.on_cancel(id);
  if (is_user(id)) demo->user_events.push_back(Event::cancel(id, qty, r));
}
void Listener::on_modify(OrderId id, Price price, Qty qty) {
  if (is_user(id)) demo->user_events.push_back(Event::modify(id, price, qty));
}

// Scratch buffers JavaScript reads from.
double depth_buf[2 * 3 * 64];
double event_buf[9 * 256];
double tape_buf[6 * 256];
double info_buf[8];
double stats_buf[12];

double now_ms() { return emscripten_get_now(); }

// ---- In-browser benchmark: the same comparison lob_bench makes, on this machine. ----

struct MapStdTraits {
  template <Side S>
  using Ladder = MapLadder<S>;
  using Index = StdOrderMap;
};

struct Counter {
  std::uint64_t trades = 0;
  void on_accept(OrderId) {}
  void on_reject(OrderId, RejectReason) {}
  void on_trade(const Trade&) { ++trades; }
  void on_cancel(OrderId, Qty, CancelReason) {}
  void on_modify(OrderId, Price, Qty) {}
  void on_level(Side, Price, std::uint64_t, std::uint32_t) {}
};

std::unique_ptr<Workload> bench_workload;

template <class Traits>
double best_ms(const Workload& w, int reps, std::uint64_t* trades) {
  double best = 1e300;
  for (int r = 0; r < reps; ++r) {
    Counter c;
    auto book = std::make_unique<OrderBook<Counter, Traits>>(book_config_for(w.spec), c);
    const double t0 = now_ms();
    for (const Command& cmd : w.commands) apply(*book, cmd);
    best = std::min(best, now_ms() - t0);
    *trades = c.trades;
  }
  return best;
}

}  // namespace

extern "C" {

/// Starts a fresh book with flow profile `profile` (0 balanced, 1 cancel-heavy, 2 sweep-heavy),
/// prefilled with `prefill` passive orders.
EMSCRIPTEN_KEEPALIVE void lob_reset(int profile, int seed, int prefill) {
  auto profiles = standard_workloads(0);
  WorkloadSpec spec = profiles[static_cast<std::size_t>(std::clamp(profile, 0, 2))];
  spec.seed = static_cast<std::uint64_t>(seed);
  g = std::make_unique<Demo>(spec);
  for (int i = 0; i < prefill; ++i) {
    const Command c = g->model.next_passive(g->book);
    apply(g->book, c);
    g->model.applied(g->book, c);
  }
  g->tape.clear();
  g->trades = 0;
  g->volume = 0;
}

/// Applies the next `n` requests of the flow. Returns milliseconds spent inside the engine.
EMSCRIPTEN_KEEPALIVE double lob_step(int n) {
  const double t0 = now_ms();
  for (int i = 0; i < n; ++i) {
    const Command c = g->model.next(g->book);
    apply(g->book, c);
    g->model.applied(g->book, c);
  }
  const double dt = now_ms() - t0;
  g->requests += n;
  g->engine_ms += dt;
  return dt;
}

/// Submits a visitor order. Returns its id.
EMSCRIPTEN_KEEPALIVE double lob_user_add(int side, double price, int qty, int market, int tif,
                                        int post_only) {
  NewOrder o;
  o.id = g->next_user++;
  o.side = side == 0 ? Side::Buy : Side::Sell;
  o.price = static_cast<Price>(price);
  o.qty = static_cast<Qty>(std::max(qty, 0));
  o.type = market ? OrderType::Market : OrderType::Limit;
  o.tif = static_cast<TimeInForce>(std::clamp(tif, 0, 2));
  o.post_only = post_only != 0;
  g->book.add(o);
  g->requests += 1;
  return static_cast<double>(o.id);
}

EMSCRIPTEN_KEEPALIVE void lob_user_cancel(double id) {
  g->book.cancel(static_cast<OrderId>(id));
  g->requests += 1;
}

EMSCRIPTEN_KEEPALIVE void lob_user_modify(double id, double price, int qty) {
  g->book.modify(static_cast<OrderId>(id), static_cast<Price>(price),
                 static_cast<Qty>(std::max(qty, 0)));
  g->requests += 1;
}

/// The best `levels` levels of each side into depth_buf as (price, total, count) triples: bids
/// first, then asks. Returns bid_count * 1000 + ask_count.
EMSCRIPTEN_KEEPALIVE int lob_depth(int levels) {
  const auto n = static_cast<std::size_t>(std::clamp(levels, 0, 64));
  const auto bids = g->book.depth(Side::Buy, n);
  const auto asks = g->book.depth(Side::Sell, n);
  std::size_t k = 0;
  for (const auto* side : {&bids, &asks}) {
    for (const LevelInfo& l : *side) {
      depth_buf[k++] = static_cast<double>(l.price);
      depth_buf[k++] = static_cast<double>(l.total);
      depth_buf[k++] = static_cast<double>(l.count);
    }
  }
  return static_cast<int>(bids.size() * 1000 + asks.size());
}
EMSCRIPTEN_KEEPALIVE double* lob_depth_buf() { return depth_buf; }

/// A visitor order's state into info_buf: side, price, open qty, orders ahead of it in its level's
/// queue, quantity ahead of it. Returns 0 if the order is not resting.
EMSCRIPTEN_KEEPALIVE int lob_order_info(double id) {
  const Order* o = g->book.find(static_cast<OrderId>(id));
  if (o == nullptr) return 0;
  double ahead = 0;
  double ahead_qty = 0;
  bool found = false;
  g->book.for_each_order(o->side, [&](const Order& r) {
    if (found || r.price != o->price) return;
    if (r.id == o->id) {
      found = true;
      return;
    }
    ahead += 1;
    ahead_qty += r.qty;
  });
  info_buf[0] = o->side == Side::Buy ? 0 : 1;
  info_buf[1] = static_cast<double>(o->price);
  info_buf[2] = o->qty;
  info_buf[3] = ahead;
  info_buf[4] = ahead_qty;
  return 1;
}
EMSCRIPTEN_KEEPALIVE double* lob_info_buf() { return info_buf; }

/// Moves up to 256 visitor-order events into event_buf as nine numbers each: kind, side, reason,
/// id, other, price, qty, leaves, other_leaves. Returns how many.
EMSCRIPTEN_KEEPALIVE int lob_drain_events() {
  auto& ev = g->user_events;
  const std::size_t n = std::min<std::size_t>(ev.size(), 256);
  for (std::size_t i = 0; i < n; ++i) {
    const Event& e = ev[i];
    double* p = event_buf + 9 * i;
    p[0] = static_cast<double>(e.kind);
    p[1] = e.side == Side::Buy ? 0 : 1;
    p[2] = e.reason;
    p[3] = static_cast<double>(e.id);
    p[4] = static_cast<double>(e.other);
    p[5] = static_cast<double>(e.price);
    p[6] = e.qty;
    p[7] = e.leaves;
    p[8] = e.other_leaves;
  }
  ev.erase(ev.begin(), ev.begin() + static_cast<std::ptrdiff_t>(n));
  return static_cast<int>(n);
}
EMSCRIPTEN_KEEPALIVE double* lob_event_buf() { return event_buf; }

/// Moves the newest (up to 256) trades since the last call into tape_buf, oldest first, as six
/// numbers each: sequence, price, qty, aggressor side, visitor was aggressor, visitor was resting.
EMSCRIPTEN_KEEPALIVE int lob_drain_tape() {
  auto& t = g->tape;
  const std::size_t n = std::min<std::size_t>(t.size(), 256);
  const std::size_t first = t.size() - n;
  for (std::size_t i = 0; i < n; ++i) {
    const Tape& r = t[first + i];
    double* p = tape_buf + 6 * i;
    p[0] = r.seq;
    p[1] = r.price;
    p[2] = r.qty;
    p[3] = r.aggressor_side;
    p[4] = r.user_aggressor;
    p[5] = r.user_resting;
  }
  t.clear();
  return static_cast<int>(n);
}
EMSCRIPTEN_KEEPALIVE double* lob_tape_buf() { return tape_buf; }

/// Counters into stats_buf: requests, trades, volume, resting orders, bid levels, ask levels,
/// best bid (or -1), best ask (or -1), engine milliseconds, last trade price.
EMSCRIPTEN_KEEPALIVE double* lob_stats() {
  const auto& b = g->book;
  stats_buf[0] = g->requests;
  stats_buf[1] = g->trades;
  stats_buf[2] = g->volume;
  stats_buf[3] = static_cast<double>(b.order_count());
  stats_buf[4] = static_cast<double>(b.level_count(Side::Buy));
  stats_buf[5] = static_cast<double>(b.level_count(Side::Sell));
  stats_buf[6] = b.best_bid() ? static_cast<double>(*b.best_bid()) : -1;
  stats_buf[7] = b.best_ask() ? static_cast<double>(*b.best_ask()) : -1;
  stats_buf[8] = g->engine_ms;
  stats_buf[9] = g->last_trade_price;
  return stats_buf;
}

/// Generates a `requests`-long workload of profile `profile`, ready for lob_bench_run.
EMSCRIPTEN_KEEPALIVE void lob_bench_prepare(int profile, int requests) {
  auto profiles = standard_workloads(static_cast<std::uint64_t>(requests));
  bench_workload =
      std::make_unique<Workload>(generate(profiles[static_cast<std::size_t>(std::clamp(profile, 0, 2))]));
}

/// Replays the prepared workload with one storage design (0: dense ladder + flat index, the
/// default; 1: std::map + std::unordered_map) and returns the best of `reps` runs in
/// milliseconds. Both designs must agree on the trade count, which is returned in info_buf[0].
EMSCRIPTEN_KEEPALIVE double lob_bench_run(int design, int reps) {
  std::uint64_t trades = 0;
  const double ms = design == 0 ? best_ms<DenseBookTraits>(*bench_workload, reps, &trades)
                                : best_ms<MapStdTraits>(*bench_workload, reps, &trades);
  info_buf[0] = static_cast<double>(trades);
  return ms;
}

EMSCRIPTEN_KEEPALIVE double lob_bench_requests() {
  return bench_workload ? static_cast<double>(bench_workload->commands.size()) : 0;
}

}  // extern "C"
