// lob_shell: type orders at a live book and watch what happens.
//
//   buy|sell <id> <qty> [@ <price>] [ioc|fok] [post] [owner <n>]   no price = market order
//   cancel <id>
//   modify <id> <qty> @ <price>
//   book [levels]        depth ladder
//   orders               every resting order in priority order
//   help | quit
//
// Reads standard input, so it also replays scripts: ./lob_shell < examples/session.txt
// Options: --stp none|cancel-resting|cancel-aggressor|cancel-both

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "lob/events.hpp"
#include "lob/order_book.hpp"

namespace {

using namespace lob;

struct Printer {
  void on_accept(OrderId id) { std::printf("  accepted #%llu\n", ull(id)); }
  void on_reject(OrderId id, RejectReason r) {
    std::printf("  rejected #%llu: %s\n", ull(id), std::string(to_string(r)).c_str());
  }
  void on_trade(const Trade& t) {
    std::printf("  TRADE %llu @ %lld  #%llu (%s, %llu left) x #%llu (resting, %llu left)\n",
                ull(t.qty), static_cast<long long>(t.price), ull(t.aggressor),
                t.aggressor_side == Side::Buy ? "buys" : "sells", ull(t.aggressor_leaves),
                ull(t.resting), ull(t.resting_leaves));
  }
  void on_cancel(OrderId id, Qty qty, CancelReason r) {
    std::printf("  cancelled #%llu, %llu lots (%s)\n", ull(id), ull(qty),
                std::string(to_string(r)).c_str());
  }
  void on_modify(OrderId id, Price price, Qty qty) {
    std::printf("  modified #%llu to %llu @ %lld\n", ull(id), ull(qty),
                static_cast<long long>(price));
  }
  void on_level(Side side, Price price, std::uint64_t total, std::uint32_t count) {
    std::printf("    level %s %lld: %llu lots in %u orders\n", side == Side::Buy ? "bid" : "ask",
                static_cast<long long>(price), ull(total), count);
  }

  static unsigned long long ull(std::uint64_t v) { return static_cast<unsigned long long>(v); }
};

void print_book(const OrderBook<Printer>& book, std::size_t levels) {
  const auto bids = book.depth(Side::Buy, levels);
  const auto asks = book.depth(Side::Sell, levels);
  std::printf("  %10s %7s | %-7s | %7s %-10s\n", "bid lots", "orders", "price", "orders",
              "ask lots");
  for (auto it = asks.rbegin(); it != asks.rend(); ++it)
    std::printf("  %10s %7s | %-7lld | %7u %-10llu\n", "", "", static_cast<long long>(it->price),
                it->count, static_cast<unsigned long long>(it->total));
  if (!bids.empty() && !asks.empty()) {
    std::printf("  %18s   spread %lld\n", "", static_cast<long long>(asks[0].price - bids[0].price));
  } else {
    std::printf("  %18s   ---\n", "");
  }
  for (const auto& b : bids)
    std::printf("  %10llu %7u | %-7lld |\n", static_cast<unsigned long long>(b.total), b.count,
                static_cast<long long>(b.price));
}

void help() {
  std::printf(
      "  buy|sell <id> <qty> [@ <price>] [ioc|fok] [post] [owner <n>]   (no price = market)\n"
      "  cancel <id>\n"
      "  modify <id> <qty> @ <price>\n"
      "  book [levels] | orders | help | quit\n");
}

bool parse_stp(const std::string& s, StpMode& out) {
  for (StpMode m : {StpMode::None, StpMode::CancelResting, StpMode::CancelAggressor,
                    StpMode::CancelBoth}) {
    if (s == to_string(m)) {
      out = m;
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  BookConfig config;
  config.min_price = 1;
  config.max_price = 1'000'000;
  config.max_orders = 1u << 20;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--stp" && i + 1 < argc && parse_stp(argv[i + 1], config.stp)) {
      ++i;
    } else {
      std::fprintf(stderr, "usage: lob_shell [--stp none|cancel-resting|cancel-aggressor|cancel-both]\n");
      return 2;
    }
  }

  Printer printer;
  OrderBook<Printer> book(config, printer);
  const bool interactive = isatty(STDIN_FILENO);
  if (interactive) {
    std::printf("lob_shell: prices are integer ticks in [%lld, %lld]; stp %s. Type help.\n",
                static_cast<long long>(config.min_price), static_cast<long long>(config.max_price),
                std::string(to_string(config.stp)).c_str());
  }

  std::string line;
  while (true) {
    if (interactive) std::printf("lob> ");
    std::fflush(stdout);
    if (!std::getline(std::cin, line)) break;
    const auto hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    std::istringstream in(line);
    std::string cmd;
    if (!(in >> cmd)) continue;
    if (!interactive) std::printf("> %s\n", line.c_str());

    if (cmd == "quit" || cmd == "exit") break;
    if (cmd == "help") {
      help();
    } else if (cmd == "buy" || cmd == "sell") {
      NewOrder o;
      o.side = cmd == "buy" ? Side::Buy : Side::Sell;
      unsigned long long id = 0;
      unsigned long long qty = 0;
      if (!(in >> id >> qty)) {
        help();
        continue;
      }
      o.id = id;
      o.qty = static_cast<Qty>(qty);
      bool bad = false;
      bool priced = false;
      bool tif_given = false;
      for (std::string tok; in >> tok;) {
        if (tok == "@") {
          long long px = 0;
          if (!(in >> px)) bad = true;
          o.price = px;
          priced = true;
        } else if (tok == "ioc" || tok == "fok") {
          o.tif = tok == "ioc" ? TimeInForce::IOC : TimeInForce::FOK;
          tif_given = true;
        } else if (tok == "post") {
          o.post_only = true;
        } else if (tok == "owner") {
          unsigned owner;
          if (!(in >> owner)) bad = true;
          o.owner = owner;
        } else {
          bad = true;
        }
      }
      if (bad) {
        help();
        continue;
      }
      // A price makes it a limit order, good till cancelled unless told otherwise; no price
      // makes it a market order, which never rests.
      o.type = priced ? OrderType::Limit : OrderType::Market;
      if (!tif_given) o.tif = priced ? TimeInForce::GTC : TimeInForce::IOC;
      book.add(o);
    } else if (cmd == "cancel") {
      unsigned long long id;
      if (in >> id) {
        book.cancel(id);
      } else {
        help();
      }
    } else if (cmd == "modify") {
      unsigned long long id, qty;
      std::string at;
      long long px;
      if (in >> id >> qty >> at >> px && at == "@") {
        book.modify(id, px, static_cast<Qty>(qty));
      } else {
        help();
      }
    } else if (cmd == "book") {
      std::size_t levels = 10;
      in >> levels;
      print_book(book, levels);
    } else if (cmd == "orders") {
      for (Side side : {Side::Sell, Side::Buy}) {
        book.for_each_order(side, [&](const Order& o) {
          std::printf("  %-4s #%-6llu %6u @ %lld%s\n", side == Side::Buy ? "bid" : "ask",
                      static_cast<unsigned long long>(o.id), o.qty, static_cast<long long>(o.price),
                      (o.flags & order_flags::kPostOnly) ? "  post-only" : "");
        });
      }
    } else {
      std::printf("  unknown command '%s'\n", cmd.c_str());
      help();
    }
  }
  return 0;
}
