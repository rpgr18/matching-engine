// lob_replay: record a synthetic request stream to a file, or replay a recorded one.
//
//   lob_replay gen <workload> <requests> <out.bin> [--seed N]
//   lob_replay run <file.bin> [--depth N] [--no-prefetch]
//
// `run` prints what the stream did (trades, cancels, rejects, final book) and how long it took.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>

#include "lob/command_file.hpp"
#include "lob/order_book.hpp"
#include "lob/replay.hpp"
#include "lob/workload.hpp"

namespace {

using namespace lob;

int usage() {
  std::fprintf(stderr,
               "usage: lob_replay gen <workload> <requests> <out.bin> [--seed N]\n"
               "       lob_replay run <file.bin> [--depth N] [--no-prefetch]\n\nworkloads:");
  for (const auto& w : standard_workloads(0)) std::fprintf(stderr, " %s", w.name.c_str());
  std::fprintf(stderr, "\n");
  return 2;
}

struct Tally {
  std::uint64_t accepts = 0, rejects = 0, trades = 0, traded_qty = 0, cancels = 0, modifies = 0;
  std::uint64_t level_updates = 0;
  std::uint64_t reject_by[16] = {};
  std::uint64_t cancel_by[8] = {};

  void on_accept(OrderId) { ++accepts; }
  void on_reject(OrderId, RejectReason r) {
    ++rejects;
    ++reject_by[static_cast<int>(r)];
  }
  void on_trade(const Trade& t) {
    ++trades;
    traded_qty += t.qty;
  }
  void on_cancel(OrderId, Qty, CancelReason r) {
    ++cancels;
    ++cancel_by[static_cast<int>(r)];
  }
  void on_modify(OrderId, Price, Qty) { ++modifies; }
  void on_level(Side, Price, std::uint64_t, std::uint32_t) { ++level_updates; }
};

int gen(int argc, char** argv) {
  if (argc < 5) return usage();
  WorkloadSpec spec;
  bool found = false;
  for (const auto& w : standard_workloads(0)) {
    if (w.name == argv[2]) {
      spec = w;
      found = true;
    }
  }
  if (!found) return usage();
  spec.requests = std::strtoull(argv[3], nullptr, 10);
  for (int i = 5; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--seed") spec.seed = std::strtoull(argv[i + 1], nullptr, 10);

  const Workload w = generate(spec);
  CommandFile file;
  file.config = book_config_for(spec);
  file.prefill = w.prefill;
  file.commands = w.commands;
  write_command_file(argv[4], file);
  std::printf("wrote %zu commands (%zu prefill) to %s\n", w.commands.size(), w.prefill, argv[4]);
  return 0;
}

int run(int argc, char** argv) {
  if (argc < 3) return usage();
  std::size_t depth = 5;
  bool prefetch = true;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--depth" && i + 1 < argc) {
      depth = std::strtoull(argv[++i], nullptr, 10);
    } else if (a == "--no-prefetch") {
      prefetch = false;
    } else {
      return usage();
    }
  }

  const CommandFile file = read_command_file(argv[2]);
  Tally tally;
  auto book = std::make_unique<OrderBook<Tally>>(file.config, tally);
  const Command* first = file.commands.data();
  const Command* begin = first + file.prefill;
  const Command* end = first + file.commands.size();
  for (const Command* c = first; c != begin; ++c) apply(*book, *c);
  tally = Tally{};

  ReplayOptions opt;
  if (!prefetch) opt.min_orders = ~std::size_t{0};
  const auto t0 = std::chrono::steady_clock::now();
  replay(*book, begin, end, opt);
  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  const auto n = static_cast<double>(end - begin);

  std::printf("%s: %.0f requests in %.3f s = %.2f M requests/s (%.1f ns each)\n", argv[2], n,
              secs, n / secs / 1e6, secs * 1e9 / n);
  std::printf("  accepted %llu, rejected %llu, trades %llu (%llu lots), cancels %llu, "
              "modifies %llu, level updates %llu\n",
              static_cast<unsigned long long>(tally.accepts),
              static_cast<unsigned long long>(tally.rejects),
              static_cast<unsigned long long>(tally.trades),
              static_cast<unsigned long long>(tally.traded_qty),
              static_cast<unsigned long long>(tally.cancels),
              static_cast<unsigned long long>(tally.modifies),
              static_cast<unsigned long long>(tally.level_updates));
  for (int r = 0; r < 16; ++r)
    if (tally.reject_by[r])
      std::printf("    reject %-20s %llu\n",
                  std::string(to_string(static_cast<RejectReason>(r))).c_str(),
                  static_cast<unsigned long long>(tally.reject_by[r]));
  for (int c = 0; c < 8; ++c)
    if (tally.cancel_by[c])
      std::printf("    cancel %-20s %llu\n",
                  std::string(to_string(static_cast<CancelReason>(c))).c_str(),
                  static_cast<unsigned long long>(tally.cancel_by[c]));

  std::printf("  final book: %zu resting orders, %zu bid levels, %zu ask levels\n",
              book->order_count(), book->level_count(Side::Buy), book->level_count(Side::Sell));
  const auto bids = book->depth(Side::Buy, depth);
  const auto asks = book->depth(Side::Sell, depth);
  std::printf("  %24s | %-24s\n", "bids", "asks");
  for (std::size_t i = 0; i < std::max(bids.size(), asks.size()); ++i) {
    char l[64] = "";
    char r[64] = "";
    if (i < bids.size())
      std::snprintf(l, sizeof(l), "%llu (%u) @ %lld",
                    static_cast<unsigned long long>(bids[i].total), bids[i].count,
                    static_cast<long long>(bids[i].price));
    if (i < asks.size())
      std::snprintf(r, sizeof(r), "%lld @ %llu (%u)", static_cast<long long>(asks[i].price),
                    static_cast<unsigned long long>(asks[i].total), asks[i].count);
    std::printf("  %24s | %-24s\n", l, r);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    if (cmd == "gen") return gen(argc, argv);
    if (cmd == "run") return run(argc, argv);
    return usage();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lob_replay: %s\n", e.what());
    return 1;
  }
}
