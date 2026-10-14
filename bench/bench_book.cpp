// lob_bench: single-threaded throughput and per-request latency of the order book.
//
// Each workload is generated once, up front, as a flat array of 32-byte commands, so the timed
// loop does nothing but read the next command and hand it to the book. Throughput is the median of
// several full replays on a fresh book (after one untimed warm-up replay); latency comes from a
// separate replay that timestamps every request with the CPU counter, because the timestamps
// themselves cost a few nanoseconds and would distort the throughput figure.
//
// Every implementation must produce bit-identical output for a workload, checked through a
// running hash of every trade. A storage choice that is faster but wrong fails the run.
//
//   lob_bench [--requests N] [--reps N] [--workload NAME[,NAME...]] [--impl NAME[,NAME...]]
//             [--json PATH] [--no-latency] [--cpu N]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "lob/clock.hpp"
#include "lob/command.hpp"
#include "lob/dense_ladder.hpp"
#include "lob/latency_histogram.hpp"
#include "lob/map_ladder.hpp"
#include "lob/order_book.hpp"
#include "lob/replay.hpp"
#include "lob/std_order_map.hpp"
#include "lob/vector_ladder.hpp"
#include "lob/workload.hpp"
#include "platform.hpp"

namespace {

using namespace lob;

struct DenseStdIndexTraits {
  template <Side S>
  using Ladder = DenseLadder<S>;
  using Index = StdOrderMap;
};

struct MapStdIndexTraits {
  template <Side S>
  using Ladder = MapLadder<S>;
  using Index = StdOrderMap;
};

/// Counts events and folds every trade into a running hash. The hash keeps the compiler from
/// discarding work and lets implementations be checked against each other.
struct CountingListener {
  std::uint64_t trades = 0;
  std::uint64_t traded_qty = 0;
  std::uint64_t cancels = 0;
  std::uint64_t rejects = 0;
  std::uint64_t level_updates = 0;
  std::uint64_t hash = 14695981039346656037ull;

  void on_accept(OrderId) noexcept {}
  void on_reject(OrderId, RejectReason) noexcept { ++rejects; }
  void on_trade(const Trade& t) noexcept {
    ++trades;
    traded_qty += t.qty;
    hash = (hash ^ (t.aggressor * 0x9E3779B97F4A7C15ull + t.resting * 31 +
                    static_cast<std::uint64_t>(t.price) * 131 + t.qty)) *
           1099511628211ull;
  }
  void on_cancel(OrderId, Qty, CancelReason) noexcept { ++cancels; }
  void on_modify(OrderId, Price, Qty) noexcept {}
  void on_level(Side, Price, std::uint64_t, std::uint32_t) noexcept { ++level_updates; }

  [[nodiscard]] bool same_output(const CountingListener& o) const noexcept {
    return trades == o.trades && traded_qty == o.traded_qty && cancels == o.cancels &&
           rejects == o.rejects && level_updates == o.level_updates && hash == o.hash;
  }
};

struct Options {
  std::uint64_t requests = 5'000'000;
  int reps = 5;
  std::vector<std::string> workloads;
  std::vector<std::string> impls;
  std::string json;
  bool latency = true;
  int cpu = -1;
};

/// Request categories for the latency breakdown.
enum Category { kAddRests, kAddTrades, kCancel, kModify, kCategories };
constexpr const char* kCategoryName[] = {"add, no fill", "add, fills", "cancel", "modify"};

struct ImplResult {
  std::string impl;
  double median_mreq_s = 0;
  double best_mreq_s = 0;
  double median_ns_per_req = 0;
  LatencyHistogram latency;
  LatencyHistogram by_category[kCategories];
  CountingListener output;
};

[[noreturn]] void die(const std::string& msg) {
  std::fprintf(stderr, "lob_bench: %s\n", msg.c_str());
  std::exit(1);
}

ReplayOptions replay_options() {
  ReplayOptions o;
  // For tuning: LOB_PREFETCH="slot,order,neighbour[,min_orders]".
  if (const char* v = std::getenv("LOB_PREFETCH")) {
    std::sscanf(v, "%td,%td,%td,%zu", &o.slot_distance, &o.order_distance,
                &o.neighbour_distance, &o.min_orders);
  }
  return o;
}

/// Plain request-at-a-time replay, or the software-pipelined batch replay.
template <bool Pipelined, class Book>
void replay_all(Book& book, const Command* begin, const Command* end, const ReplayOptions& o) {
  if constexpr (Pipelined) {
    replay(book, begin, end, o);
  } else {
    for (const Command* c = begin; c != end; ++c) apply(book, *c);
  }
}

template <class Traits, bool Pipelined = false>
ImplResult run_impl(const std::string& name, const Workload& w, const Options& opt) {
  using Book = OrderBook<CountingListener, Traits>;
  ImplResult r;
  r.impl = name;
  const BookConfig cfg = book_config_for(w.spec);
  const Command* first = w.commands.data();
  const Command* begin = first + w.prefill;
  const Command* end = first + w.commands.size();
  const auto n = static_cast<double>(end - begin);
  const ReplayOptions ropt = replay_options();

  std::vector<double> seconds;
  for (int rep = 0; rep <= opt.reps; ++rep) {
    CountingListener listener;
    auto book = std::make_unique<Book>(cfg, listener);
    for (const Command* c = first; c != begin; ++c) apply(*book, *c);
    listener = CountingListener{};

    const auto t0 = std::chrono::steady_clock::now();
    replay_all<Pipelined>(*book, begin, end, ropt);
    const auto t1 = std::chrono::steady_clock::now();
    bench::keep(listener.hash);

    if (rep == 0) {
      r.output = listener;  // warm-up replay; also the reference output for later replays
      continue;
    }
    if (!listener.same_output(r.output)) die(name + ": replays of one workload disagree");
    seconds.push_back(std::chrono::duration<double>(t1 - t0).count());
  }
  std::sort(seconds.begin(), seconds.end());
  const double median = seconds[seconds.size() / 2];
  r.median_mreq_s = n / median / 1e6;
  r.best_mreq_s = n / seconds.front() / 1e6;
  r.median_ns_per_req = median * 1e9 / n;

  if (opt.latency) {
    CountingListener listener;
    auto book = std::make_unique<Book>(cfg, listener);
    for (const Command* c = first; c != begin; ++c) apply(*book, *c);
    for (const Command* c = begin; c != end; ++c) {
      const std::uint64_t trades_before = listener.trades;
      const std::uint64_t t0 = clock::now();
      if constexpr (Pipelined) {
        // The lookahead replay() issues, timed together with the request it precedes.
        if (book->order_count() >= ropt.min_orders) {
          if (end - c > ropt.slot_distance) book->prefetch_slot(c[ropt.slot_distance]);
          if (end - c > ropt.order_distance)
            (void)book->prefetch_order(c[ropt.order_distance]);
          if (end - c > ropt.neighbour_distance)
            book->prefetch_neighbours(c[ropt.neighbour_distance],
                                      book->prefetch_order(c[ropt.neighbour_distance]));
        }
      }
      apply(*book, *c);
      const std::uint64_t t1 = clock::now();
      const std::uint64_t dt = t1 - t0;
      r.latency.record(dt);
      Category cat = kModify;
      if (c->type == CommandType::Add) {
        cat = listener.trades != trades_before ? kAddTrades : kAddRests;
      } else if (c->type == CommandType::Cancel) {
        cat = kCancel;
      }
      r.by_category[cat].record(dt);
    }
    bench::keep(listener.hash);
  }
  return r;
}

struct Impl {
  const char* name;
  const char* description;
  std::function<ImplResult(const std::string&, const Workload&, const Options&)> run;
};

std::vector<Impl> all_impls() {
  return {
      {"dense+flat", "dense tick ladder, flat open-addressing index (default)",
       run_impl<DenseBookTraits>},
      {"dense+flat+pf", "dense+flat, batch replay with software-pipelined prefetch",
       run_impl<DenseBookTraits, true>},
      {"vector+flat", "sorted vector ladder, flat index", run_impl<VectorBookTraits>},
      {"map+flat", "std::map ladder, flat index", run_impl<MapBookTraits>},
      {"dense+std", "dense tick ladder, std::unordered_map index", run_impl<DenseStdIndexTraits>},
      {"map+std", "std::map ladder, std::unordered_map index (textbook)",
       run_impl<MapStdIndexTraits>},
  };
}

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  for (std::string item; std::getline(ss, item, ',');)
    if (!item.empty()) out.push_back(item);
  return out;
}

bool wanted(const std::vector<std::string>& filter, const std::string& name) {
  return filter.empty() || std::find(filter.begin(), filter.end(), name) != filter.end();
}

Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) die("missing value for " + a);
      return argv[++i];
    };
    if (a == "--requests") {
      o.requests = std::strtoull(value().c_str(), nullptr, 10);
    } else if (a == "--reps") {
      o.reps = std::max(1, std::atoi(value().c_str()));
    } else if (a == "--workload") {
      o.workloads = split(value());
    } else if (a == "--impl") {
      o.impls = split(value());
    } else if (a == "--json") {
      o.json = value();
    } else if (a == "--no-latency") {
      o.latency = false;
    } else if (a == "--cpu") {
      o.cpu = std::atoi(value().c_str());
    } else if (a == "--help" || a == "-h") {
      std::printf(
          "usage: lob_bench [--requests N] [--reps N] [--workload a,b] [--impl a,b]\n"
          "                 [--json PATH] [--no-latency] [--cpu N]\n\nworkloads:");
      for (const auto& w : standard_workloads(0)) std::printf(" %s", w.name.c_str());
      std::printf("\nimpls:\n");
      for (const auto& im : all_impls()) std::printf("  %-12s %s\n", im.name, im.description);
      std::exit(0);
    } else {
      die("unknown argument " + a + " (try --help)");
    }
  }
  return o;
}

std::string with_commas(double v) {
  auto n = static_cast<std::uint64_t>(v + 0.5);
  std::string s = std::to_string(n);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
  return s;
}

double ns(std::uint64_t ticks) { return clock::to_ns(ticks); }

void print_workload(const Workload& w) {
  const auto& s = w.stats;
  const double n = static_cast<double>(w.spec.requests);
  std::printf("\n%s: %s requests (%.1f%% add, %.1f%% take, %.1f%% cancel, %.1f%% modify)\n",
              w.spec.name.c_str(), with_commas(n).c_str(), 100.0 * static_cast<double>(s.adds) / n,
              100.0 * static_cast<double>(s.takes) / n, 100.0 * static_cast<double>(s.cancels) / n,
              100.0 * static_cast<double>(s.modifies) / n);
  const double takers = static_cast<double>(std::max<std::uint64_t>(1, s.takes_filled));
  std::printf("  %.3f trades/request, %.1f price levels per taker that traded, %s resting orders over "
              "%s levels on average\n\n",
              static_cast<double>(s.trades) / n, static_cast<double>(s.take_levels) / takers,
              with_commas(s.mean_depth).c_str(), with_commas(s.mean_levels).c_str());
  std::printf("  %-12s %8s %8s %8s | %7s %7s %7s %7s %8s %9s   latency, ns\n", "impl", "Mreq/s",
              "best", "ns/req", "p50", "p90", "p99", "p99.9", "p99.99", "max");
}

void print_result(const ImplResult& r, bool latency) {
  std::printf("  %-12s %8.2f %8.2f %8.1f |", r.impl.c_str(), r.median_mreq_s, r.best_mreq_s,
              r.median_ns_per_req);
  if (latency) {
    const auto& h = r.latency;
    std::printf(" %7.0f %7.0f %7.0f %7.0f %8.0f %9s", ns(h.quantile(0.5)), ns(h.quantile(0.9)),
                ns(h.quantile(0.99)), ns(h.quantile(0.999)), ns(h.quantile(0.9999)),
                with_commas(ns(h.max())).c_str());
  }
  std::printf("\n");
  std::fflush(stdout);
}

void print_breakdown(const ImplResult& r) {
  std::printf("\n  %s by request type:\n", r.impl.c_str());
  std::printf("    %-14s %11s %8s %7s %7s %7s\n", "", "count", "mean", "p50", "p99", "p99.9");
  for (int c = 0; c < kCategories; ++c) {
    const auto& h = r.by_category[c];
    if (h.count() == 0) continue;
    std::printf("    %-14s %11s %8.1f %7.0f %7.0f %7.0f\n", kCategoryName[c],
                with_commas(static_cast<double>(h.count())).c_str(), clock::to_ns(1) * h.mean(),
                ns(h.quantile(0.5)), ns(h.quantile(0.99)), ns(h.quantile(0.999)));
  }
}

std::string json_latency(const LatencyHistogram& h) {
  std::ostringstream os;
  os.precision(6);
  os << "{\"count\": " << h.count() << ", \"mean\": " << clock::to_ns(1) * h.mean()
     << ", \"p50\": " << ns(h.quantile(0.5)) << ", \"p90\": " << ns(h.quantile(0.9))
     << ", \"p99\": " << ns(h.quantile(0.99)) << ", \"p999\": " << ns(h.quantile(0.999))
     << ", \"p9999\": " << ns(h.quantile(0.9999)) << ", \"max\": " << ns(h.max()) << "}";
  return os.str();
}

void write_json(const std::string& path, const Options& opt, const std::string& placement,
                const std::vector<std::pair<Workload, std::vector<ImplResult>>>& runs) {
  std::ofstream out(path);
  if (!out) die("cannot write " + path);
  const std::time_t now = std::time(nullptr);
  char date[32];
  std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&now));
  out.precision(6);
  out << "{\n  \"machine\": {\"cpu\": \"" << bench::cpu_name() << "\", \"os\": \""
      << bench::os_name() << "\", \"compiler\": \"" << bench::compiler_name()
      << "\", \"timer_resolution_ns\": " << clock::resolution_ns()
      << ", \"timer_overhead_ns\": " << clock::overhead_ns() << ", \"placement\": \"" << placement
      << "\", \"date\": \"" << date << "\"},\n";
  out << "  \"reps\": " << opt.reps << ",\n  \"workloads\": [\n";
  for (std::size_t i = 0; i < runs.size(); ++i) {
    const auto& [w, results] = runs[i];
    const auto& s = w.stats;
    out << "    {\"name\": \"" << w.spec.name << "\", \"requests\": " << w.spec.requests
        << ", \"adds\": " << s.adds << ", \"takes\": " << s.takes << ", \"cancels\": " << s.cancels
        << ", \"modifies\": " << s.modifies << ", \"trades\": " << s.trades
        << ", \"takes_filled\": " << s.takes_filled << ", \"take_levels\": " << s.take_levels
        << ", \"mean_depth\": " << s.mean_depth << ", \"mean_levels\": " << s.mean_levels
        << ",\n     \"results\": [\n";
    for (std::size_t j = 0; j < results.size(); ++j) {
      const auto& r = results[j];
      out << "       {\"impl\": \"" << r.impl << "\", \"mreq_s\": " << r.median_mreq_s
          << ", \"best_mreq_s\": " << r.best_mreq_s << ", \"ns_per_req\": " << r.median_ns_per_req;
      if (opt.latency) {
        out << ",\n        \"latency_ns\": " << json_latency(r.latency) << ",\n        \"by_type\": {";
        for (int c = 0; c < kCategories; ++c)
          out << (c ? ", " : "") << '"' << kCategoryName[c] << "\": " << json_latency(r.by_category[c]);
        out << "}";
      }
      out << "}" << (j + 1 < results.size() ? "," : "") << "\n";
    }
    out << "     ]}" << (i + 1 < runs.size() ? "," : "") << "\n";
  }
  out << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse(argc, argv);
  const std::string placement = bench::place_current_thread(opt.cpu);

  std::printf("lob_bench | %s | %s | %s\n", bench::cpu_name().c_str(), bench::os_name().c_str(),
              bench::compiler_name().c_str());
  std::printf("timer: %.2f ns resolution, %.1f ns per read | %s | %d timed replays per result\n",
              clock::resolution_ns(), clock::overhead_ns(), placement.c_str(), opt.reps);

  std::vector<std::pair<Workload, std::vector<ImplResult>>> runs;
  for (const WorkloadSpec& spec : standard_workloads(opt.requests)) {
    if (!wanted(opt.workloads, spec.name)) continue;
    Workload w = generate(spec);
    print_workload(w);
    std::vector<ImplResult> results;
    for (const Impl& im : all_impls()) {
      if (!wanted(opt.impls, im.name)) continue;
      results.push_back(im.run(im.name, w, opt));
      if (results.size() > 1 && !results.back().output.same_output(results.front().output))
        die(std::string(im.name) + " produced different output from " + results.front().impl);
      print_result(results.back(), opt.latency);
    }
    if (opt.latency && !results.empty()) print_breakdown(results.front());
    w.commands.clear();
    w.commands.shrink_to_fit();
    runs.emplace_back(std::move(w), std::move(results));
  }

  if (!opt.json.empty()) {
    write_json(opt.json, opt, placement, runs);
    std::printf("\nwrote %s\n", opt.json.c_str());
  }
  return 0;
}
