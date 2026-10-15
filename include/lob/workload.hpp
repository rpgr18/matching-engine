#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "lob/command.hpp"
#include "lob/order_book.hpp"
#include "lob/rng.hpp"

namespace lob {

/// Parameters of a synthetic order flow.
///
/// The model: a reference price takes a random walk; passive orders rest at a geometrically
/// distributed distance behind it (and never through the opposite touch), so liquidity is
/// thickest near the touch and thins out with depth; takers cross the spread with marketable
/// limits, IOCs or market orders, large enough to sweep several levels when the book is thin;
/// cancels and modifies target live orders chosen uniformly, and a re-priced order stays passive.
/// Every trade therefore comes from a taker. The cancel rate scales with live depth, which holds
/// the book near `target_depth` orders.
struct WorkloadSpec {
  std::string name = "balanced";
  std::uint64_t requests = 1'000'000;
  std::uint64_t seed = 1;
  /// Passive orders added before the measured stream, so it starts from a populated book.
  std::uint64_t prefill = 0;

  // Request mix, as relative weights. The cancel weight is multiplied by live / target_depth.
  double w_add = 0.48;
  double w_take = 0.04;
  double w_cancel = 0.43;
  double w_modify = 0.05;

  std::uint32_t target_depth = 5'000;  ///< Resting orders the book settles around.
  double depth_mean = 8;               ///< Mean distance of passive orders behind the touch, ticks.
  double move_prob = 0.01;             ///< Per-request probability the reference price moves a tick.
  double market_share = 0.10;          ///< Fraction of takers that are market orders.
  double gtc_take_share = 0.20;        ///< Fraction of takers that rest any unfilled remainder.
  double sweep_mean = 1.5;             ///< Mean ticks a taker's limit reaches through the touch.
  double qty_mean = 10;                ///< Mean passive order size, lots.
  double take_qty_mean = 30;           ///< Mean taker size, lots.
  double reduce_share = 0.6;           ///< Fraction of modifies that only reduce quantity.
  std::uint32_t owners = 0;            ///< Draw owner ids from [1, owners]; 0 keeps every order anonymous.
  Price min_price = 1;
  Price max_price = 1 << 16;
};

/// What the generated stream contains and what it did to the book while being generated.
struct WorkloadStats {
  std::uint64_t adds = 0;
  std::uint64_t takes = 0;
  std::uint64_t cancels = 0;
  std::uint64_t modifies = 0;
  std::uint64_t trades = 0;
  std::uint64_t rejects = 0;
  std::uint64_t takes_filled = 0;  ///< Takers that traded at least once.
  std::uint64_t take_levels = 0;   ///< Price levels those takers traded at, summed.
  double mean_depth = 0;   ///< Average resting orders, sampled across the run.
  double mean_levels = 0;  ///< Average populated price levels, both sides.
};

struct Workload {
  WorkloadSpec spec;
  /// The first `prefill` commands build the starting book; the rest are the measured stream.
  std::vector<Command> commands;
  std::size_t prefill = 0;
  WorkloadStats stats;
};

/// The book configuration a workload must be replayed against for its stream to mean the same
/// thing it meant when generated.
[[nodiscard]] inline BookConfig book_config_for(const WorkloadSpec& spec) {
  BookConfig c;
  c.min_price = spec.min_price;
  c.max_price = spec.max_price;
  c.max_orders = std::max<std::uint32_t>(1u << 12, spec.target_depth * 2);
  return c;
}

namespace detail {

/// Tracks which order ids are resting, from the book's own events, so the generator can aim
/// cancels and modifies at live orders. O(1) insert, erase and uniform sampling.
class LiveSet {
 public:
  void insert(OrderId id) {
    if (pos_.emplace(id, ids_.size()).second) ids_.push_back(id);
  }
  void erase(OrderId id) {
    const auto it = pos_.find(id);
    if (it == pos_.end()) return;
    const std::size_t i = it->second;
    pos_.erase(it);
    if (i + 1 != ids_.size()) {
      ids_[i] = ids_.back();
      pos_[ids_[i]] = i;
    }
    ids_.pop_back();
  }
  [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }
  [[nodiscard]] OrderId at(std::size_t i) const noexcept { return ids_[i]; }

 private:
  std::vector<OrderId> ids_;
  std::unordered_map<OrderId, std::size_t> pos_;
};

}  // namespace detail

/// The order-flow model as a stream: each call draws one request for the book as it stands now.
///
/// The caller applies each request and keeps the model informed through on_trade() and
/// on_cancel() (from the book's listener) and applied() (after the request), which is how the
/// model knows which of its orders are resting. Orders the model did not create are never
/// targeted, so a person can trade against the flow without the model cancelling their orders.
class FlowModel {
 public:
  enum class Kind : std::uint8_t { Add, Take, Cancel, Modify };

  explicit FlowModel(const WorkloadSpec& spec)
      : spec_(spec),
        rng_(spec.seed),
        lo_(spec.min_price + 64),
        hi_(spec.max_price - 64),
        ref_(spec.min_price + (spec.max_price - spec.min_price) / 2) {}

  /// A passive order resting behind the reference price: the request used to build a book.
  template <class Book>
  [[nodiscard]] Command next_passive(const Book& book) {
    NewOrder o;
    o.id = next_id_++;
    o.side = rng_.chance(0.5) ? Side::Buy : Side::Sell;
    const auto d = static_cast<Price>(rng_.geometric(spec_.depth_mean));
    o.price = passive_price(book, o.side, o.side == Side::Buy ? ref_ - 1 - d : ref_ + 1 + d);
    o.qty = qty(spec_.qty_mean);
    o.owner = owner();
    return Command::add(o);
  }

  /// The next request of the flow. `kind`, if given, receives what sort of request it is.
  template <class Book>
  [[nodiscard]] Command next(const Book& book, Kind* kind = nullptr) {
    if (rng_.chance(spec_.move_prob)) ref_ = std::clamp(ref_ + (rng_.chance(0.5) ? 1 : -1), lo_, hi_);

    const double scale = static_cast<double>(live_.size()) / spec_.target_depth;
    const double wc = live_.size() ? spec_.w_cancel * scale : 0;
    const double wm = live_.size() ? spec_.w_modify : 0;
    const double roll = rng_.uniform() * (spec_.w_add + spec_.w_take + wc + wm);

    Kind k;
    Command cmd;
    if (roll < spec_.w_add) {
      k = Kind::Add;
      cmd = next_passive(book);
    } else if (roll < spec_.w_add + spec_.w_take) {
      k = Kind::Take;
      NewOrder o;
      o.id = next_id_++;
      o.side = rng_.chance(0.5) ? Side::Buy : Side::Sell;
      const auto touch = o.side == Side::Buy ? book.best_ask() : book.best_bid();
      const Price base = touch.value_or(ref_);
      const auto through = static_cast<Price>(rng_.geometric(spec_.sweep_mean));
      o.price = clamp_price(o.side == Side::Buy ? base + through : base - through);
      o.qty = qty(spec_.take_qty_mean);
      o.owner = owner();
      if (rng_.chance(spec_.market_share)) {
        o.type = OrderType::Market;
        o.tif = TimeInForce::IOC;
      } else {
        o.tif = rng_.chance(spec_.gtc_take_share) ? TimeInForce::GTC : TimeInForce::IOC;
      }
      cmd = Command::add(o);
    } else if (roll < spec_.w_add + spec_.w_take + wc) {
      k = Kind::Cancel;
      cmd = Command::cancel(live_.at(rng_.below(live_.size())));
    } else {
      k = Kind::Modify;
      const OrderId id = live_.at(rng_.below(live_.size()));
      const Order& o = *book.find(id);
      if (rng_.chance(spec_.reduce_share)) {
        cmd = Command::modify(id, o.price, static_cast<Qty>(1 + rng_.below(o.qty)));
      } else {
        // Re-quote one to three ticks away from the touch or towards it, staying passive.
        const auto step = static_cast<Price>(1 + rng_.below(3));
        const Price moved = rng_.chance(0.5) ? o.price + step : o.price - step;
        cmd = Command::modify(id, passive_price(book, o.side, moved), o.qty);
      }
    }
    if (kind) *kind = k;
    return cmd;
  }

  /// Call after applying a request the model produced.
  template <class Book>
  void applied(const Book& book, const Command& cmd) {
    if (cmd.type != CommandType::Cancel && book.find(cmd.id)) live_.insert(cmd.id);
  }

  /// Forward the book's trade and cancel events here.
  void on_trade(const Trade& t) {
    if (t.resting_leaves == 0) live_.erase(t.resting);
    if (t.aggressor_leaves == 0) live_.erase(t.aggressor);
  }
  void on_cancel(OrderId id) { live_.erase(id); }

  [[nodiscard]] std::size_t live() const noexcept { return live_.size(); }
  [[nodiscard]] Price reference_price() const noexcept { return ref_; }
  [[nodiscard]] const WorkloadSpec& spec() const noexcept { return spec_; }

 private:
  Price clamp_price(Price p) const { return std::clamp(p, spec_.min_price, spec_.max_price); }
  OwnerId owner() {
    return spec_.owners ? static_cast<OwnerId>(1 + rng_.below(spec_.owners)) : 0;
  }
  Qty qty(double mean) { return static_cast<Qty>(1 + rng_.geometric(mean - 1)); }

  /// Keeps a price on the passive side of the opposite touch.
  template <class Book>
  Price passive_price(const Book& book, Side side, Price p) const {
    if (side == Side::Buy) {
      if (const auto ask = book.best_ask()) p = std::min(p, *ask - 1);
    } else {
      if (const auto bid = book.best_bid()) p = std::max(p, *bid + 1);
    }
    return clamp_price(p);
  }

  WorkloadSpec spec_;
  Rng rng_;
  detail::LiveSet live_;
  Price lo_;
  Price hi_;
  Price ref_;
  OrderId next_id_ = 1;
};

namespace detail {

struct GeneratorListener {
  FlowModel* model;
  std::uint64_t trades = 0;
  std::uint64_t rejects = 0;
  std::uint64_t request_levels = 0;  ///< Distinct trade prices in the current request.
  Price last_price = 0;

  void on_accept(OrderId) {}
  void on_reject(OrderId, RejectReason) { ++rejects; }
  void on_trade(const Trade& t) {
    if (request_levels == 0 || t.price != last_price) ++request_levels;
    last_price = t.price;
    ++trades;
    model->on_trade(t);
  }
  void on_cancel(OrderId id, Qty, CancelReason) { model->on_cancel(id); }
  void on_modify(OrderId, Price, Qty) {}
  void on_level(Side, Price, std::uint64_t, std::uint32_t) {}
};

}  // namespace detail

/// Generates a request stream by running the flow model against a real book, so every cancel and
/// modify is aimed at an order that is resting at that moment. Deterministic for a given spec.
[[nodiscard]] inline Workload generate(const WorkloadSpec& spec) {
  Workload w;
  w.spec = spec;
  w.commands.reserve(spec.prefill + spec.requests);

  FlowModel model(spec);
  detail::GeneratorListener listener{&model};
  OrderBook<detail::GeneratorListener> book(book_config_for(spec), listener);
  double depth_sum = 0;
  double level_sum = 0;
  std::uint64_t samples = 0;

  for (std::uint64_t n = 0; n < spec.prefill; ++n) {
    const Command cmd = model.next_passive(book);
    apply(book, cmd);
    model.applied(book, cmd);
    w.commands.push_back(cmd);
  }
  w.prefill = w.commands.size();
  listener.trades = 0;
  listener.rejects = 0;

  for (std::uint64_t n = 0; n < spec.requests; ++n) {
    FlowModel::Kind kind;
    const Command cmd = model.next(book, &kind);
    switch (kind) {
      case FlowModel::Kind::Add: ++w.stats.adds; break;
      case FlowModel::Kind::Take: ++w.stats.takes; break;
      case FlowModel::Kind::Cancel: ++w.stats.cancels; break;
      case FlowModel::Kind::Modify: ++w.stats.modifies; break;
    }

    listener.request_levels = 0;
    apply(book, cmd);
    model.applied(book, cmd);
    if (listener.request_levels > 0 && kind == FlowModel::Kind::Take) {
      ++w.stats.takes_filled;
      w.stats.take_levels += listener.request_levels;
    }
    w.commands.push_back(cmd);

    if ((n & 1023) == 0) {
      depth_sum += static_cast<double>(book.order_count());
      level_sum += static_cast<double>(book.level_count(Side::Buy) + book.level_count(Side::Sell));
      ++samples;
    }
  }

  w.stats.trades = listener.trades;
  w.stats.rejects = listener.rejects;
  w.stats.mean_depth = samples ? depth_sum / static_cast<double>(samples) : 0;
  w.stats.mean_levels = samples ? level_sum / static_cast<double>(samples) : 0;
  return w;
}

/// The workloads the benchmark reports on. Each is a different stress, not a different size.
[[nodiscard]] inline std::vector<WorkloadSpec> standard_workloads(std::uint64_t requests) {
  std::vector<WorkloadSpec> out;

  WorkloadSpec balanced;
  balanced.name = "balanced";
  balanced.requests = requests;
  out.push_back(balanced);

  // Market-maker heavy flow, closer to what lit equity venues see: liquidity sits within a few
  // ticks of the touch and is cancelled roughly ten times as often as it trades.
  WorkloadSpec cancels = balanced;
  cancels.name = "cancel-heavy";
  cancels.w_add = 0.50;
  cancels.w_take = 0.012;
  cancels.w_cancel = 0.465;
  cancels.w_modify = 0.023;
  cancels.target_depth = 2'000;
  cancels.depth_mean = 3;
  cancels.seed = 2;
  out.push_back(cancels);

  // A thin book and large takers: each taker that trades sweeps between two and three price
  // levels on average, so level creation, removal and best-price recovery dominate.
  WorkloadSpec sweeps = balanced;
  sweeps.name = "sweep-heavy";
  sweeps.w_add = 0.60;
  sweeps.w_take = 0.04;
  sweeps.w_cancel = 0.30;
  sweeps.w_modify = 0.06;
  sweeps.target_depth = 600;
  sweeps.depth_mean = 10;
  sweeps.take_qty_mean = 150;
  sweeps.sweep_mean = 4;
  sweeps.seed = 3;
  out.push_back(sweeps);

  // A million resting orders spread over thousands of levels: the working set no longer fits in
  // cache, so this measures memory behaviour rather than instruction counts.
  WorkloadSpec deep = balanced;
  deep.name = "deep-book";
  deep.target_depth = 1'000'000;
  deep.prefill = 1'000'000;
  deep.depth_mean = 400;
  deep.move_prob = 0.002;
  deep.seed = 4;
  out.push_back(deep);

  return out;
}

}  // namespace lob
