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

struct GeneratorListener {
  LiveSet* live;
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
    if (t.resting_leaves == 0) live->erase(t.resting);
    if (t.aggressor_leaves == 0) live->erase(t.aggressor);
  }
  void on_cancel(OrderId id, Qty, CancelReason) { live->erase(id); }
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

  detail::LiveSet live;
  detail::GeneratorListener listener{&live};
  OrderBook<detail::GeneratorListener> book(book_config_for(spec), listener);
  Rng rng(spec.seed);

  const Price lo = spec.min_price + 64;
  const Price hi = spec.max_price - 64;
  Price ref = spec.min_price + (spec.max_price - spec.min_price) / 2;
  OrderId next_id = 1;
  double depth_sum = 0;
  double level_sum = 0;
  std::uint64_t samples = 0;

  auto clamp_price = [&](Price p) { return std::clamp(p, spec.min_price, spec.max_price); };
  auto owner = [&]() -> OwnerId {
    return spec.owners ? static_cast<OwnerId>(1 + rng.below(spec.owners)) : 0;
  };
  auto qty = [&](double mean) { return static_cast<Qty>(1 + rng.geometric(mean - 1)); };
  // Keeps a price on the passive side of the opposite touch.
  auto passive_price = [&](Side side, Price p) {
    if (side == Side::Buy) {
      if (const auto ask = book.best_ask()) p = std::min(p, *ask - 1);
    } else {
      if (const auto bid = book.best_bid()) p = std::max(p, *bid + 1);
    }
    return clamp_price(p);
  };
  auto passive = [&]() {
    NewOrder o;
    o.id = next_id++;
    o.side = rng.chance(0.5) ? Side::Buy : Side::Sell;
    const auto d = static_cast<Price>(rng.geometric(spec.depth_mean));
    o.price = passive_price(o.side, o.side == Side::Buy ? ref - 1 - d : ref + 1 + d);
    o.qty = qty(spec.qty_mean);
    o.owner = owner();
    return Command::add(o);
  };

  for (std::uint64_t n = 0; n < spec.prefill; ++n) {
    const Command cmd = passive();
    apply(book, cmd);
    if (book.find(cmd.id)) live.insert(cmd.id);
    w.commands.push_back(cmd);
  }
  w.prefill = w.commands.size();
  listener.trades = 0;
  listener.rejects = 0;

  for (std::uint64_t n = 0; n < spec.requests; ++n) {
    if (rng.chance(spec.move_prob)) ref = std::clamp(ref + (rng.chance(0.5) ? 1 : -1), lo, hi);

    const double scale = static_cast<double>(live.size()) / spec.target_depth;
    const double wc = live.size() ? spec.w_cancel * scale : 0;
    const double wm = live.size() ? spec.w_modify : 0;
    const double roll = rng.uniform() * (spec.w_add + spec.w_take + wc + wm);

    Command cmd;
    if (roll < spec.w_add) {
      cmd = passive();
      ++w.stats.adds;
    } else if (roll < spec.w_add + spec.w_take) {
      NewOrder o;
      o.id = next_id++;
      o.side = rng.chance(0.5) ? Side::Buy : Side::Sell;
      const auto touch = o.side == Side::Buy ? book.best_ask() : book.best_bid();
      const Price base = touch.value_or(ref);
      const auto through = static_cast<Price>(rng.geometric(spec.sweep_mean));
      o.price = clamp_price(o.side == Side::Buy ? base + through : base - through);
      o.qty = qty(spec.take_qty_mean);
      o.owner = owner();
      if (rng.chance(spec.market_share)) {
        o.type = OrderType::Market;
        o.tif = TimeInForce::IOC;
      } else {
        o.tif = rng.chance(spec.gtc_take_share) ? TimeInForce::GTC : TimeInForce::IOC;
      }
      cmd = Command::add(o);
      ++w.stats.takes;
    } else if (roll < spec.w_add + spec.w_take + wc) {
      cmd = Command::cancel(live.at(rng.below(live.size())));
      ++w.stats.cancels;
    } else {
      const OrderId id = live.at(rng.below(live.size()));
      const Order& o = *book.find(id);
      if (rng.chance(spec.reduce_share)) {
        cmd = Command::modify(id, o.price, static_cast<Qty>(1 + rng.below(o.qty)));
      } else {
        // Re-quote one to three ticks away from the touch or towards it, staying passive.
        const auto step = static_cast<Price>(1 + rng.below(3));
        const Price moved = rng.chance(0.5) ? o.price + step : o.price - step;
        cmd = Command::modify(id, passive_price(o.side, moved), o.qty);
      }
      ++w.stats.modifies;
    }

    listener.request_levels = 0;
    apply(book, cmd);
    if (cmd.type != CommandType::Cancel && book.find(cmd.id)) live.insert(cmd.id);
    if (listener.request_levels > 0 && roll >= spec.w_add && roll < spec.w_add + spec.w_take) {
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
