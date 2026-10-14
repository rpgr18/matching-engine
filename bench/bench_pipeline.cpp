// lob_pipeline: end-to-end latency through a three-thread matching pipeline.
//
//   gateway thread --SPSC--> engine thread --SPSC--> drop-copy thread
//
// The gateway replays a pre-generated multi-symbol request stream at a fixed offered rate, or flat
// out, and records for each request the time it was *scheduled* to be sent. The engine thread
// applies requests to a multi-symbol Engine and publishes every execution report and market-data
// update to the outbound queue. The drop-copy thread consumes them and, on each request's final
// message, records scheduled-send to receipt.
//
// Measuring from the schedule rather than from the actual send matters: if any stage stalls, the
// gateway falls behind its schedule and every request delayed by the stall is charged for it. A
// benchmark that timestamps at the actual send hides exactly those stalls (coordinated omission).
//
// Before the pipeline runs, the queue itself is measured on its own: one-way throughput and the
// round-trip time of a single message between two threads.
//
//   lob_pipeline [--requests N] [--symbols K] [--workload NAME] [--rates 1e6,5e6,max]
//                [--queue N] [--batch N] [--cpus a,b,c] [--json PATH]

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "lob/clock.hpp"
#include "lob/command.hpp"
#include "lob/engine.hpp"
#include "lob/events.hpp"
#include "lob/latency_histogram.hpp"
#include "lob/spsc_queue.hpp"
#include "lob/workload.hpp"
#include "platform.hpp"

namespace {

using namespace lob;

/// One inbound message: a request and the time the gateway was scheduled to send it. The time
/// travels with the request, so no stage has to read a timestamp another core wrote elsewhere.
struct InMsg {
  Command command;
  std::uint64_t sent;
};

/// One outbound message: an event, plus the send time of the request that caused it with the top
/// bit marking that request's final message.
struct OutMsg {
  static constexpr std::uint64_t kLast = std::uint64_t{1} << 63;
  Event event;
  std::uint64_t sent_and_last;

  [[nodiscard]] bool last() const noexcept { return (sent_and_last & kLast) != 0; }
  [[nodiscard]] std::uint64_t sent() const noexcept { return sent_and_last & ~kLast; }
};
static_assert(sizeof(OutMsg) == 64);

/// Engine listener that publishes every event to the outbound queue. It holds each event back
/// by one so that the last event of a request can be flagged before it is sent.
struct Publisher {
  SpscQueue<OutMsg>* out;
  std::uint64_t sent = 0;  ///< send time of the request being processed
  bool live = false;       ///< false while the starting book is being built
  bool have = false;
  OutMsg pending{};

  void emit(const Event& e) {
    if (!live) return;
    if (have) out->write(pending);
    pending.event = e;
    pending.sent_and_last = sent;
    have = true;
  }
  /// Ends the current request: flags its last event and stages it. The caller publishes.
  void finish() {
    if (!live) return;
    pending.sent_and_last |= OutMsg::kLast;
    out->write(pending);
    have = false;
  }

  void on_accept(SymbolId s, OrderId id) { emit(Event::accept(id).on(s)); }
  void on_reject(SymbolId s, OrderId id, RejectReason r) { emit(Event::reject(id, r).on(s)); }
  void on_trade(SymbolId s, const Trade& t) { emit(Event::trade(t).on(s)); }
  void on_cancel(SymbolId s, OrderId id, Qty qty, CancelReason r) {
    emit(Event::cancel(id, qty, r).on(s));
  }
  void on_modify(SymbolId s, OrderId id, Price price, Qty qty) {
    emit(Event::modify(id, price, qty).on(s));
  }
  void on_level(SymbolId s, Side side, Price price, std::uint64_t total, std::uint32_t count) {
    emit(Event::level(side, price, total, count).on(s));
  }
};

struct Options {
  std::uint64_t requests = 4'000'000;
  int symbols = 4;
  std::string workload = "balanced";
  std::vector<double> rates = {1e6, 5e6, 10e6, 0};  // 0 = flat out
  std::size_t queue = 1 << 16;
  std::size_t batch = 32;
  int cpus[3] = {-1, -1, -1};
  std::string json;
};

[[noreturn]] void die(const std::string& msg) {
  std::fprintf(stderr, "lob_pipeline: %s\n", msg.c_str());
  std::exit(1);
}

Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) die("missing value for " + a);
      return argv[++i];
    };
    auto list = [](const std::string& s) {
      std::vector<std::string> out;
      std::stringstream ss(s);
      for (std::string item; std::getline(ss, item, ',');) out.push_back(item);
      return out;
    };
    if (a == "--requests") {
      o.requests = std::strtoull(value().c_str(), nullptr, 10);
    } else if (a == "--symbols") {
      o.symbols = std::max(1, std::atoi(value().c_str()));
    } else if (a == "--workload") {
      o.workload = value();
    } else if (a == "--rates") {
      o.rates.clear();
      for (const auto& r : list(value())) o.rates.push_back(r == "max" ? 0 : std::atof(r.c_str()));
    } else if (a == "--batch") {
      o.batch = std::max<std::size_t>(1, std::strtoull(value().c_str(), nullptr, 10));
    } else if (a == "--queue") {
      o.queue = std::strtoull(value().c_str(), nullptr, 10);
    } else if (a == "--cpus") {
      const auto cpus = list(value());
      if (cpus.size() != 3) die("--cpus takes three ids: gateway,engine,drop-copy");
      for (int k = 0; k < 3; ++k) o.cpus[k] = std::atoi(cpus[static_cast<std::size_t>(k)].c_str());
    } else if (a == "--json") {
      o.json = value();
    } else if (a == "--help" || a == "-h") {
      std::printf(
          "usage: lob_pipeline [--requests N] [--symbols K] [--workload NAME]\n"
          "                    [--rates 1e6,5e6,max] [--queue N] [--batch N] [--cpus a,b,c]\n"
          "                    [--json PATH]\n");
      std::exit(0);
    } else {
      die("unknown argument " + a + " (try --help)");
    }
  }
  return o;
}

double ns(std::uint64_t ticks) { return clock::to_ns(ticks); }

std::string with_commas(double v) {
  auto n = static_cast<std::uint64_t>(v + 0.5);
  std::string s = std::to_string(n);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
    s.insert(static_cast<std::size_t>(i), ",");
  return s;
}

// ---------------------------------------------------------------------------------------------
// The queue on its own

struct QueueResult {
  double mmsg_s = 0;
  LatencyHistogram rtt;
};

QueueResult measure_queue(const Options& opt) {
  QueueResult r;
  struct Msg {
    std::uint64_t v[8];
  };

  {  // One-way throughput with 64-byte messages.
    constexpr std::uint64_t kCount = 50'000'000;
    SpscQueue<Msg> q(opt.queue);
    std::atomic<bool> go{false};
    std::thread consumer([&] {
      bench::place_current_thread(opt.cpus[2]);
      while (!go.load(std::memory_order_acquire)) cpu_relax();
      std::uint64_t got = 0;
      std::uint64_t sum = 0;
      while (got < kCount) {
        const std::size_t n = q.consume([&](const Msg& m) { sum += m.v[0]; }, 256);
        if (n == 0) cpu_relax();
        got += n;
      }
      bench::keep(sum);
    });
    bench::place_current_thread(opt.cpus[0]);
    go.store(true, std::memory_order_release);
    const auto t0 = clock::now();
    for (std::uint64_t i = 0; i < kCount; ++i) q.push(Msg{{i, 0, 0, 0, 0, 0, 0, 0}});
    consumer.join();
    const auto t1 = clock::now();
    r.mmsg_s = static_cast<double>(kCount) / (ns(t1 - t0) * 1e-9) / 1e6;
  }

  {  // Round trip of one message: A -> B -> A.
    constexpr int kTrips = 1'000'000;
    SpscQueue<std::uint64_t> there(1024);
    SpscQueue<std::uint64_t> back(1024);
    std::thread echo([&] {
      bench::place_current_thread(opt.cpus[1]);
      std::uint64_t v;
      for (int i = 0; i < kTrips; ++i) {
        while (!there.try_pop(v)) {
        }
        back.push(v);
      }
    });
    bench::place_current_thread(opt.cpus[0]);
    std::uint64_t v;
    for (int i = 0; i < kTrips; ++i) {
      const auto t0 = clock::now();
      there.push(static_cast<std::uint64_t>(i));
      while (!back.try_pop(v)) {
      }
      const auto t1 = clock::now();
      if (i >= kTrips / 100) r.rtt.record(t1 - t0);
    }
    echo.join();
  }
  return r;
}

// ---------------------------------------------------------------------------------------------
// The pipeline

struct Stream {
  std::vector<WorkloadSpec> specs;  ///< one per symbol
  std::vector<Command> prefill;     ///< builds every symbol's starting book
  std::vector<Command> requests;    ///< the measured stream, symbols interleaved
};

Stream build_stream(const Options& opt) {
  WorkloadSpec base;
  bool found = false;
  for (const auto& s : standard_workloads(0)) {
    if (s.name == opt.workload) {
      base = s;
      found = true;
    }
  }
  if (!found) die("unknown workload " + opt.workload);

  Stream st;
  std::vector<Workload> per_symbol;
  const auto k = static_cast<std::uint64_t>(opt.symbols);
  for (std::uint64_t s = 0; s < k; ++s) {
    WorkloadSpec spec = base;
    spec.requests = opt.requests / k;
    spec.seed = base.seed * 1000 + s;
    st.specs.push_back(spec);
    per_symbol.push_back(generate(spec));
  }
  for (std::uint64_t s = 0; s < k; ++s) {
    const Workload& w = per_symbol[s];
    for (std::size_t i = 0; i < w.prefill; ++i) {
      Command c = w.commands[i];
      c.symbol = static_cast<SymbolId>(s);
      st.prefill.push_back(c);
    }
  }
  // Round-robin interleave, so consecutive requests usually hit different books.
  for (std::size_t i = 0;; ++i) {
    bool any = false;
    for (std::uint64_t s = 0; s < k; ++s) {
      const Workload& w = per_symbol[s];
      if (w.prefill + i < w.commands.size()) {
        Command c = w.commands[w.prefill + i];
        c.symbol = static_cast<SymbolId>(s);
        st.requests.push_back(c);
        any = true;
      }
    }
    if (!any) break;
  }
  return st;
}

struct PipelineResult {
  double offered = 0;  ///< requests per second; 0 = flat out
  double achieved = 0;
  double events_per_request = 0;
  LatencyHistogram latency;
};

PipelineResult run_pipeline(const Options& opt, const Stream& st, double rate) {
  PipelineResult r;
  r.offered = rate;
  const std::size_t n = st.requests.size();
  const std::size_t warmup = n / 100;  // excluded from the histogram

  const std::size_t kBatch = opt.batch;
  SpscQueue<InMsg> inbound(opt.queue);
  SpscQueue<OutMsg> outbound(opt.queue);
  std::atomic<int> ready{0};

  std::thread engine_thread([&] {
    bench::place_current_thread(opt.cpus[1]);
    Publisher pub{&outbound};
    auto engine = std::make_unique<Engine<Publisher>>(pub);
    for (const WorkloadSpec& spec : st.specs) engine->add_symbol(book_config_for(spec));
    for (const Command& c : st.prefill) engine->apply(c);
    pub.live = true;
    ready.fetch_add(1, std::memory_order_release);
    std::size_t done = 0;
    while (done < n) {
      const std::size_t got = inbound.consume(
          [&](const InMsg& m) {
            pub.sent = m.sent;
            engine->apply(m.command);
            pub.finish();
          },
          kBatch);
      // One release store for everything this batch produced. When the gateway is ahead, the
      // batch holds many requests and the drop copy takes the tail's line once for all of them;
      // when it is not, the batch is a single request and nothing waits.
      outbound.publish();
      if (got == 0) cpu_relax();
      done += got;
    }
  });

  std::uint64_t events = 0;
  std::uint64_t first_send = 0;
  std::uint64_t last_receive = 0;
  std::thread drop_copy([&] {
    bench::place_current_thread(opt.cpus[2]);
    ready.fetch_add(1, std::memory_order_release);
    std::size_t received = 0;
    while (received < n) {
      const std::size_t got = outbound.consume(
          [&](const OutMsg& m) {
            ++events;
            if (!m.last()) return;
            const std::uint64_t now = clock::now();
            if (received >= warmup) r.latency.record(now - m.sent());
            ++received;
            last_receive = now;
          },
          256);
      if (got == 0) cpu_relax();
    }
  });

  // The calling thread is the gateway.
  bench::place_current_thread(opt.cpus[0]);
  while (ready.load(std::memory_order_acquire) < 2) cpu_relax();
  const double ticks_per_request = rate > 0 ? clock::ticks_per_ns() * 1e9 / rate : 0;
  const std::uint64_t start = clock::now() + static_cast<std::uint64_t>(clock::ticks_per_ns() * 1e6);
  for (std::size_t i = 0; i < n; ++i) {
    std::uint64_t t;
    if (rate > 0) {
      t = start + static_cast<std::uint64_t>(static_cast<double>(i) * ticks_per_request);
      while (clock::now() < t) {
      }
    } else {
      t = clock::now();
    }
    if (i == 0) first_send = t;
    inbound.push(InMsg{st.requests[i], t});
  }

  engine_thread.join();
  drop_copy.join();
  r.achieved = static_cast<double>(n) / (ns(last_receive - first_send) * 1e-9);
  r.events_per_request = static_cast<double>(events) / static_cast<double>(n);
  return r;
}

std::string rate_label(double rate) {
  if (rate <= 0) return "flat out";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f M/s", rate / 1e6);
  return buf;
}

std::string json_hist(const LatencyHistogram& h) {
  std::ostringstream os;
  os.precision(6);
  os << "{\"count\": " << h.count() << ", \"mean\": " << clock::to_ns(1) * h.mean()
     << ", \"p50\": " << ns(h.quantile(0.5)) << ", \"p90\": " << ns(h.quantile(0.9))
     << ", \"p99\": " << ns(h.quantile(0.99)) << ", \"p999\": " << ns(h.quantile(0.999))
     << ", \"p9999\": " << ns(h.quantile(0.9999)) << ", \"max\": " << ns(h.max()) << "}";
  return os.str();
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse(argc, argv);
  std::printf("lob_pipeline | %s | %s | %s\n", bench::cpu_name().c_str(), bench::os_name().c_str(),
              bench::compiler_name().c_str());
  const std::string placement = bench::place_current_thread(opt.cpus[0]);
  std::printf("timer: %.2f ns resolution | threads: %s\n\n", clock::resolution_ns(),
              placement.c_str());

  const QueueResult q = measure_queue(opt);
  std::printf("SPSC queue, 64-byte messages: %.1f M msgs/s one way\n", q.mmsg_s);
  std::printf("  round trip between two threads: p50 %.0f ns, p99 %.0f ns, p99.9 %.0f ns\n\n",
              ns(q.rtt.quantile(0.5)), ns(q.rtt.quantile(0.99)), ns(q.rtt.quantile(0.999)));

  const Stream st = build_stream(opt);
  std::printf(
      "pipeline: %s flow over %d symbols, %s requests, queue capacity %s, engine batch %zu\n",
      opt.workload.c_str(), opt.symbols,
      with_commas(static_cast<double>(st.requests.size())).c_str(),
      with_commas(static_cast<double>(opt.queue)).c_str(), opt.batch);
  std::printf("  %-10s %10s %10s | %7s %7s %7s %8s %8s %10s   gateway to drop copy, ns\n",
              "offered", "achieved", "events/req", "p50", "p90", "p99", "p99.9", "p99.99", "max");

  std::vector<PipelineResult> results;
  for (double rate : opt.rates) {
    results.push_back(run_pipeline(opt, st, rate));
    const PipelineResult& r = results.back();
    const auto& h = r.latency;
    std::printf("  %-10s %8.2f M/s %10.2f | %7.0f %7.0f %7.0f %8.0f %8.0f %10s\n",
                rate_label(rate).c_str(), r.achieved / 1e6, r.events_per_request,
                ns(h.quantile(0.5)), ns(h.quantile(0.9)), ns(h.quantile(0.99)),
                ns(h.quantile(0.999)), ns(h.quantile(0.9999)), with_commas(ns(h.max())).c_str());
    std::fflush(stdout);
  }

  if (!opt.json.empty()) {
    std::ofstream out(opt.json);
    if (!out) die("cannot write " + opt.json);
    const std::time_t now = std::time(nullptr);
    char date[32];
    std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&now));
    out.precision(6);
    out << "{\n  \"machine\": {\"cpu\": \"" << bench::cpu_name() << "\", \"os\": \""
        << bench::os_name() << "\", \"compiler\": \"" << bench::compiler_name()
        << "\", \"timer_resolution_ns\": " << clock::resolution_ns() << ", \"placement\": \""
        << placement << "\", \"date\": \"" << date << "\"},\n";
    out << "  \"queue\": {\"mmsg_s\": " << q.mmsg_s << ", \"rtt_ns\": " << json_hist(q.rtt)
        << "},\n";
    out << "  \"pipeline\": {\"workload\": \"" << opt.workload << "\", \"symbols\": "
        << opt.symbols << ", \"requests\": " << st.requests.size() << ", \"batch\": " << opt.batch
        << ", \"runs\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
      const auto& r = results[i];
      out << "    {\"offered\": " << r.offered << ", \"achieved\": " << r.achieved
          << ", \"events_per_request\": " << r.events_per_request
          << ", \"latency_ns\": " << json_hist(r.latency) << "}"
          << (i + 1 < results.size() ? "," : "") << "\n";
    }
    out << "  ]}\n}\n";
    std::printf("\nwrote %s\n", opt.json.c_str());
  }
  return 0;
}
