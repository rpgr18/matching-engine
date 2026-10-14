// lob_itch: rebuild every order book from a NASDAQ TotalView-ITCH 5.0 file, or make a synthetic one.
//
//   lob_itch replay <file> [--book vector|map|both] [--top N]
//                   [--verify] [--at HH:MM:SS] [--symbols A,B,...]
//   lob_itch synth <out> [--symbols K] [--requests N] [--workload NAME]
//
// --verify replays the feed once more, untimed, and checks the rebuilt books against two things
// NASDAQ guarantees during continuous trading: displayed orders execute in price-time priority,
// so an execution should hit the order at the front of its side's best level; and its displayed
// book never stays crossed, since a crossing order would have matched. --at prints the listed
// symbols' books as they stood at that time of day.
//
// NASDAQ publishes full trading days at https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ as gzipped
// files of length-prefixed messages; gunzip one and pass it to replay. A day is several billion
// bytes and a few hundred million messages, so the file is memory-mapped rather than read.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "lob/itch.hpp"
#include "lob/itch_book.hpp"
#include "lob/map_ladder.hpp"
#include "lob/vector_ladder.hpp"
#include "lob/workload.hpp"

namespace {

using namespace lob;
using Clock = std::chrono::steady_clock;

int usage() {
  std::fprintf(stderr,
               "usage: lob_itch replay <file> [--book vector|map|both] [--top N]\n"
               "                       [--verify] [--at HH:MM:SS] [--symbols A,B,...]\n"
               "       lob_itch synth <out> [--symbols K] [--requests N] [--workload NAME]\n");
  return 2;
}

/// A read-only memory mapping of a whole file.
class Mapping {
 public:
  explicit Mapping(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) throw std::runtime_error("cannot open " + path);
    struct stat st {};
    if (::fstat(fd_, &st) != 0) throw std::runtime_error("cannot stat " + path);
    size_ = static_cast<std::size_t>(st.st_size);
    if (size_ == 0) return;
    void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (p == MAP_FAILED) throw std::runtime_error("cannot map " + path);
    // WILLNEED, not SEQUENTIAL: the file is read twice (capacity scan, then rebuild), and macOS
    // treats SEQUENTIAL as permission to free pages behind the reader, which turns the second
    // pass into a page fault every 16 KB and parsing from 4 ns a message into 22.
    ::madvise(p, size_, MADV_WILLNEED);
    data_ = static_cast<const std::uint8_t*>(p);
  }
  ~Mapping() {
    if (data_) ::munmap(const_cast<std::uint8_t*>(data_), size_);
    if (fd_ >= 0) ::close(fd_);
  }
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;

  [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

 private:
  int fd_ = -1;
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
};

double seconds(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

std::string price(Price p) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld.%04lld", static_cast<long long>(p / 10000),
                static_cast<long long>(p % 10000));
  return buf;
}

template <class Traits>
void rebuild(const char* label, const Mapping& file, const itch::Capacity& cap, std::size_t top) {
  itch::NullEngineListener none;
  const auto t0 = Clock::now();
  itch::BookBuilder<itch::NullEngineListener, Traits> builder(none, cap);
  const auto t1 = Clock::now();
  itch::ParseStats stats;
  itch::parse(file.data(), file.size(), builder, &stats);
  const auto t2 = Clock::now();

  const double secs = seconds(t1, t2);
  std::size_t live = 0;
  std::size_t with_orders = 0;
  for (std::size_t s = 0; s < builder.instruments(); ++s) {
    const auto n = builder.engine().book(static_cast<SymbolId>(s)).order_count();
    live += n;
    with_orders += n > 0;
  }
  std::printf("\n%s ladder: %.0f messages in %.3f s = %.1f M messages/s (%.1f ns each); "
              "books sized in %.3f s\n",
              label, static_cast<double>(stats.messages), secs,
              static_cast<double>(stats.messages) / secs / 1e6,
              secs * 1e9 / static_cast<double>(stats.messages), seconds(t0, t1));
  std::printf("  %zu orders resting across %zu instruments at the end; %llu inconsistencies\n",
              live, with_orders, static_cast<unsigned long long>(builder.inconsistencies()));
  if (top == 0) return;

  std::vector<std::size_t> order(cap.messages.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::sort(order.begin(), order.end(),
            [&](std::size_t a, std::size_t b) { return cap.messages[a] > cap.messages[b]; });
  std::printf("  %-8s %12s %10s | %18s | %-18s %8s\n", "symbol", "messages", "peak live",
              "best bid", "best ask", "resting");
  for (std::size_t i = 0; i < std::min(top, order.size()); ++i) {
    const auto locate = static_cast<std::uint16_t>(order[i]);
    if (cap.messages[locate] == 0) break;
    const auto& book = builder.engine().book(locate);
    const auto bids = book.depth(Side::Buy, 1);
    const auto asks = book.depth(Side::Sell, 1);
    char bid[48] = "-";
    char ask[48] = "-";
    if (!bids.empty())
      std::snprintf(bid, sizeof(bid), "%llu @ %s", static_cast<unsigned long long>(bids[0].total),
                    price(bids[0].price).c_str());
    if (!asks.empty())
      std::snprintf(ask, sizeof(ask), "%s @ %llu", price(asks[0].price).c_str(),
                    static_cast<unsigned long long>(asks[0].total));
    std::printf("  %-8s %12llu %10u | %18s | %-18s %8zu\n", builder.name(locate).c_str(),
                static_cast<unsigned long long>(cap.messages[locate]), cap.peak_orders[locate],
                bid, ask, book.order_count());
  }
}

constexpr std::uint64_t kSecond = 1'000'000'000;

std::string clock_time(std::uint64_t ns) {
  char buf[32];
  const std::uint64_t s = ns / kSecond;
  std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu.%03llu",
                static_cast<unsigned long long>(s / 3600),
                static_cast<unsigned long long>(s / 60 % 60),
                static_cast<unsigned long long>(s % 60),
                static_cast<unsigned long long>(ns % kSecond / 1'000'000));
  return buf;
}

/// Wraps a BookBuilder, checking the rebuilt books against the exchange's own rules as the feed
/// is applied.
template <class Builder>
class Verifier : public itch::NullHandler {
 public:
  struct Tally {
    std::uint64_t executions = 0;
    std::uint64_t at_best = 0;   ///< executed order was at its side's best price
    std::uint64_t at_front = 0;  ///< ...and first in that level's queue
    std::uint64_t messages = 0;
    std::uint64_t crossed = 0;   ///< bid > ask after the message
    std::uint64_t locked = 0;    ///< bid == ask after the message
  };

  Verifier(Builder& b, std::uint64_t at, std::vector<std::string> symbols)
      : b_(b), at_(at), symbols_(std::move(symbols)) {}

  void on_system_event(const itch::SystemEvent& m) { tick(m.h.timestamp); }
  void on_stock_directory(const itch::StockDirectory& m) {
    tick(m.h.timestamp);
    b_.on_stock_directory(m);
  }
  void on_add(const itch::AddOrder& m) {
    tick(m.h.timestamp);
    b_.on_add(m);
    after(m.h);
  }
  void on_executed(const itch::OrderExecuted& m) {
    tick(m.h.timestamp);
    check(m);
    b_.on_executed(m);
    after(m.h);
  }
  void on_cancel(const itch::OrderCancel& m) {
    tick(m.h.timestamp);
    b_.on_cancel(m);
    after(m.h);
  }
  void on_delete(const itch::OrderDelete& m) {
    tick(m.h.timestamp);
    b_.on_delete(m);
    after(m.h);
  }
  void on_replace(const itch::OrderReplace& m) {
    tick(m.h.timestamp);
    b_.on_replace(m);
    after(m.h);
  }

  /// NASDAQ's own auction prints ('Q') for the symbols being watched: the official opening and
  /// closing prices, to compare the rebuilt books against.
  void on_other(char type, const std::uint8_t* p, std::size_t len) {
    if (type != itch::kCrossTrade || len < 40) return;
    const std::uint16_t locate = itch::detail::be16(p + 1);
    if (locate >= b_.instruments() || !watched(locate)) return;
    crosses.push_back(Cross{locate, itch::detail::be48(p + 5), itch::detail::be64(p + 11),
                            itch::detail::be32(p + 27), static_cast<char>(p[39])});
  }

  struct Cross {
    std::uint16_t locate;
    std::uint64_t ts;
    std::uint64_t shares;
    std::uint32_t price;
    char kind;  ///< 'O' opening, 'C' closing, 'H' halt or IPO, 'I' intraday
  };
  std::vector<Cross> crosses;

  Tally continuous;               ///< 09:30:01 to 15:59:59, clear of the opening and closing crosses
  Tally outside;                  ///< everything else
  std::uint64_t with_price = 0;   ///< 'C' executions, which print away from the order's price
  std::uint64_t not_found = 0;

 private:
  static bool in_session(std::uint64_t ts) {
    return ts >= (9 * 3600 + 30 * 60 + 1) * kSecond && ts <= (15 * 3600 + 59 * 60 + 59) * kSecond;
  }

  void check(const itch::OrderExecuted& m) {
    if (m.with_price) {
      ++with_price;
      return;
    }
    if (m.h.locate >= b_.instruments()) return;
    const auto& book = b_.engine().book(m.h.locate);
    const Order* o = book.find(m.ref);
    if (o == nullptr) {
      ++not_found;
      return;
    }
    Tally& t = in_session(m.h.timestamp) ? continuous : outside;
    ++t.executions;
    const auto best = o->side == Side::Buy ? book.best_bid() : book.best_ask();
    if (best && *best == o->price) {
      ++t.at_best;
      t.at_front += o->prev == kNil;
    }
  }

  void after(const itch::Header& h) {
    if (h.locate >= b_.instruments()) return;
    Tally& t = in_session(h.timestamp) ? continuous : outside;
    ++t.messages;
    const auto& book = b_.engine().book(h.locate);
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    if (bid && ask) {
      t.crossed += *bid > *ask;
      t.locked += *bid == *ask;
    }
  }

  bool watched(std::uint16_t locate) const {
    for (const std::string& s : symbols_)
      if (b_.name(locate) == s) return true;
    return false;
  }

  void tick(std::uint64_t ts) {
    if (snapped_ || at_ == 0 || ts < at_) return;
    snapped_ = true;
    for (const std::string& sym : symbols_) {
      for (std::size_t l = 0; l < b_.instruments(); ++l) {
        const auto locate = static_cast<std::uint16_t>(l);
        if (b_.name(locate) != sym) continue;
        const auto& book = b_.engine().book(locate);
        const auto bids = book.depth(Side::Buy, 5);
        const auto asks = book.depth(Side::Sell, 5);
        std::printf("\n  %s at %s (%zu resting orders)\n", sym.c_str(), clock_time(ts).c_str(),
                    book.order_count());
        std::printf("    %16s %12s | %-12s %-16s\n", "bid shares", "bid", "ask", "ask shares");
        for (std::size_t i = 0; i < std::max(bids.size(), asks.size()); ++i) {
          std::printf("    %16s %12s | %-12s %-16s\n",
                      i < bids.size() ? std::to_string(bids[i].total).c_str() : "",
                      i < bids.size() ? price(bids[i].price).c_str() : "",
                      i < asks.size() ? price(asks[i].price).c_str() : "",
                      i < asks.size() ? std::to_string(asks[i].total).c_str() : "");
        }
      }
    }
  }

  Builder& b_;
  std::uint64_t at_;
  std::vector<std::string> symbols_;
  bool snapped_ = false;
};

double pct(std::uint64_t a, std::uint64_t b) {
  return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0;
}

void verify(const Mapping& file, const itch::Capacity& cap, std::uint64_t at,
            const std::vector<std::string>& symbols) {
  itch::NullEngineListener none;
  itch::BookBuilder<> builder(none, cap);
  Verifier<itch::BookBuilder<>> v(builder, at, symbols);
  itch::parse(file.data(), file.size(), v);

  std::printf("\nverification (untimed replay):\n");
  std::printf("  inconsistencies: %llu (unknown references, rejects)\n",
              static_cast<unsigned long long>(builder.inconsistencies()));
  for (const auto* t : {&v.continuous, &v.outside}) {
    std::printf("  %s:\n", t == &v.continuous ? "continuous trading, 09:30:01-15:59:59"
                                               : "outside continuous trading");
    std::printf("    executions: %llu; at the best price %.3f%%; at the front of the best level "
                "%.3f%%\n",
                static_cast<unsigned long long>(t->executions), pct(t->at_best, t->executions),
                pct(t->at_front, t->executions));
    std::printf("    order messages: %llu; book crossed after %llu (%.4f%%), locked after %llu "
                "(%.4f%%)\n",
                static_cast<unsigned long long>(t->messages),
                static_cast<unsigned long long>(t->crossed), pct(t->crossed, t->messages),
                static_cast<unsigned long long>(t->locked), pct(t->locked, t->messages));
  }
  std::printf("  'C' executions (printed away from the order price, not checked): %llu\n",
              static_cast<unsigned long long>(v.with_price));
  for (const auto& c : v.crosses) {
    const char* kind = c.kind == 'O'   ? "opening cross"
                       : c.kind == 'C' ? "closing cross"
                       : c.kind == 'H' ? "halt/IPO cross"
                                       : "intraday cross";
    std::printf("  %s %s at %s: %llu shares @ %s\n", builder.name(c.locate).c_str(), kind,
                clock_time(c.ts).c_str(), static_cast<unsigned long long>(c.shares),
                price(c.price).c_str());
  }
}

std::uint64_t parse_time(const std::string& s) {
  unsigned h = 0, m = 0, sec = 0;
  if (std::sscanf(s.c_str(), "%u:%u:%u", &h, &m, &sec) != 3) throw std::runtime_error("bad --at");
  return (std::uint64_t{h} * 3600 + m * 60 + sec) * kSecond;
}

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    const std::size_t comma = s.find(',', start);
    const std::size_t end = comma == std::string::npos ? s.size() : comma;
    if (end > start) out.push_back(s.substr(start, end - start));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

int replay(int argc, char** argv) {
  if (argc < 3) return usage();
  std::string book = "vector";
  std::size_t top = 10;
  bool check = false;
  std::uint64_t at = 0;
  std::vector<std::string> symbols;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--book" && i + 1 < argc) {
      book = argv[++i];
    } else if (a == "--top" && i + 1 < argc) {
      top = std::strtoull(argv[++i], nullptr, 10);
    } else if (a == "--verify") {
      check = true;
    } else if (a == "--at" && i + 1 < argc) {
      at = parse_time(argv[++i]);
      check = true;
    } else if (a == "--symbols" && i + 1 < argc) {
      symbols = split(argv[++i]);
    } else {
      return usage();
    }
  }
  if (book != "vector" && book != "map" && book != "both") return usage();

  const Mapping file(argv[2]);
  const auto t0 = Clock::now();
  const itch::Capacity cap = itch::scan_capacity(file.data(), file.size());
  const auto t1 = Clock::now();
  const auto peak = std::accumulate(cap.peak_orders.begin(), cap.peak_orders.end(),
                                    std::uint64_t{0});
  std::printf("%s: %.2f GB; capacity scan %.2f s: %zu locates, %llu peak live orders summed "
              "over instruments, %llu unknown references\n",
              argv[2], static_cast<double>(file.size()) / 1e9, seconds(t0, t1),
              cap.peak_orders.size(), static_cast<unsigned long long>(peak),
              static_cast<unsigned long long>(cap.unknown_refs));

  {
    // Decoding alone, into a handler that keeps nothing: the floor under any book building.
    struct Touch : itch::NullHandler {
      std::uint64_t sum = 0;
      void on_add(const itch::AddOrder& m) { sum += m.ref; }
      void on_executed(const itch::OrderExecuted& m) { sum += m.ref; }
      void on_cancel(const itch::OrderCancel& m) { sum += m.ref; }
      void on_delete(const itch::OrderDelete& m) { sum += m.ref; }
      void on_replace(const itch::OrderReplace& m) { sum += m.new_ref; }
    } touch;
    itch::ParseStats stats;
    const auto p0 = Clock::now();
    itch::parse(file.data(), file.size(), touch, &stats);
    const auto p1 = Clock::now();
    std::printf("parse only: %.1f M messages/s (%.1f ns each)%s\n",
                static_cast<double>(stats.messages) / seconds(p0, p1) / 1e6,
                seconds(p0, p1) * 1e9 / static_cast<double>(stats.messages),
                touch.sum == 42 ? " " : "");
  }

  if (book == "vector" || book == "both") rebuild<VectorBookTraits>("sorted-vector", file, cap, top);
  if (book == "map" || book == "both") rebuild<MapBookTraits>("std::map", file, cap, top);
  if (check) verify(file, cap, at, symbols);
  return 0;
}

int synth(int argc, char** argv) {
  if (argc < 3) return usage();
  std::size_t symbols = 8;
  std::uint64_t requests = 1'000'000;
  std::string workload = "balanced";
  for (int i = 3; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    if (a == "--symbols") {
      symbols = std::strtoull(argv[i + 1], nullptr, 10);
    } else if (a == "--requests") {
      requests = std::strtoull(argv[i + 1], nullptr, 10);
    } else if (a == "--workload") {
      workload = argv[i + 1];
    } else {
      return usage();
    }
  }
  WorkloadSpec base;
  bool found = false;
  for (const auto& w : standard_workloads(requests)) {
    if (w.name == workload) {
      base = w;
      found = true;
    }
  }
  if (!found) return usage();

  std::vector<Workload> streams;
  for (std::size_t k = 0; k < symbols; ++k) {
    WorkloadSpec spec = base;
    spec.seed = base.seed * 1000 + k;
    streams.push_back(generate(spec));
  }
  const itch::SyntheticFeed feed = itch::synthesize(streams);
  std::FILE* f = std::fopen(argv[2], "wb");
  if (!f || std::fwrite(feed.bytes.data(), 1, feed.bytes.size(), f) != feed.bytes.size())
    throw std::runtime_error(std::string("cannot write ") + argv[2]);
  std::fclose(f);
  std::printf("wrote %llu messages (%.1f MB) for %zu symbols to %s\n",
              static_cast<unsigned long long>(feed.messages),
              static_cast<double>(feed.bytes.size()) / 1e6, symbols, argv[2]);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    if (cmd == "replay") return replay(argc, argv);
    if (cmd == "synth") return synth(argc, argv);
    return usage();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lob_itch: %s\n", e.what());
    return 1;
  }
}
