// lob_itch: rebuild every order book from a NASDAQ TotalView-ITCH 5.0 file, or make a synthetic one.
//
//   lob_itch replay <file> [--book vector|map|both] [--top N]
//   lob_itch synth <out> [--symbols K] [--requests N] [--workload NAME]
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

int replay(int argc, char** argv) {
  if (argc < 3) return usage();
  std::string book = "vector";
  std::size_t top = 10;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--book" && i + 1 < argc) {
      book = argv[++i];
    } else if (a == "--top" && i + 1 < argc) {
      top = std::strtoull(argv[++i], nullptr, 10);
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
