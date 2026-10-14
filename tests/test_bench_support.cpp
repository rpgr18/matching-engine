#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "lob/clock.hpp"
#include "lob/command.hpp"
#include "lob/events.hpp"
#include "lob/latency_histogram.hpp"
#include "lob/order_book.hpp"
#include "lob/rng.hpp"
#include "lob/workload.hpp"

namespace {

using namespace lob;

// ---------------------------------------------------------------------------------------------
// Rng

TEST(Rng, MatchesTheXoshiro256PlusPlusReference) {
  Rng r(1, 2, 3, 4);
  const std::uint64_t expected[] = {41943041ull, 58720359ull, 3588806011781223ull,
                                    3591011842654386ull, 9228616714210784205ull};
  for (std::uint64_t e : expected) EXPECT_EQ(r(), e);
}

TEST(Rng, SameSeedSameStream) {
  Rng a(99), b(99), c(100);
  bool differs = false;
  for (int i = 0; i < 1000; ++i) {
    const auto x = a();
    EXPECT_EQ(x, b());
    differs |= x != c();
  }
  EXPECT_TRUE(differs);
}

TEST(Rng, BelowIsInRangeAndRoughlyUniform) {
  Rng r(5);
  std::vector<int> hist(7, 0);
  for (int i = 0; i < 70'000; ++i) {
    const auto v = r.below(7);
    ASSERT_LT(v, 7u);
    ++hist[v];
  }
  for (int h : hist) EXPECT_NEAR(h, 10'000, 500);
  EXPECT_EQ(r.below(1), 0u);
  EXPECT_EQ(r.below(0), 0u);
}

TEST(Rng, GeometricHasTheRequestedMean) {
  Rng r(6);
  for (double mean : {0.5, 3.0, 40.0}) {
    double sum = 0;
    constexpr int kN = 200'000;
    for (int i = 0; i < kN; ++i) sum += static_cast<double>(r.geometric(mean));
    EXPECT_NEAR(sum / kN, mean, mean * 0.03 + 0.01) << "mean " << mean;
  }
  EXPECT_EQ(r.geometric(0), 0u);
}

TEST(Rng, UniformIsInUnitInterval) {
  Rng r(7);
  for (int i = 0; i < 100'000; ++i) {
    const double u = r.uniform();
    ASSERT_GE(u, 0.0);
    ASSERT_LT(u, 1.0);
  }
}

// ---------------------------------------------------------------------------------------------
// LatencyHistogram

TEST(LatencyHistogram, SmallValuesAreExact) {
  LatencyHistogram h;
  for (std::uint64_t v = 0; v < 256; ++v) {
    EXPECT_EQ(LatencyHistogram::upper_edge(LatencyHistogram::index(v)), v);
  }
  for (std::uint64_t v = 1; v <= 100; ++v) h.record(v);
  EXPECT_EQ(h.count(), 100u);
  EXPECT_EQ(h.min(), 1u);
  EXPECT_EQ(h.max(), 100u);
  EXPECT_DOUBLE_EQ(h.mean(), 50.5);
  EXPECT_EQ(h.quantile(0.5), 50u);
  EXPECT_EQ(h.quantile(0.99), 99u);
  EXPECT_EQ(h.quantile(1.0), 100u);
  EXPECT_EQ(h.quantile(0.0), 1u);
}

TEST(LatencyHistogram, BucketsTileTheRangeWithoutGaps) {
  for (std::size_t i = 1; i < LatencyHistogram::kBuckets - 1; ++i) {
    const std::uint64_t lo = LatencyHistogram::upper_edge(i - 1) + 1;
    ASSERT_EQ(LatencyHistogram::index(lo), i);
    ASSERT_EQ(LatencyHistogram::index(LatencyHistogram::upper_edge(i)), i);
  }
  EXPECT_EQ(LatencyHistogram::index(~std::uint64_t{0}), LatencyHistogram::kBuckets - 1);
}

TEST(LatencyHistogram, QuantilesAreWithinBucketPrecisionOfExact) {
  Rng r(8);
  LatencyHistogram h;
  std::vector<std::uint64_t> samples;
  for (int i = 0; i < 100'000; ++i) {
    // Heavy-tailed, like real latency: mostly tens, occasionally tens of thousands.
    const auto v = static_cast<std::uint64_t>(20.0 * std::exp(3.0 * r.uniform() * r.uniform()));
    const std::uint64_t x = r.chance(0.001) ? v * 1000 : v;
    samples.push_back(x);
    h.record(x);
  }
  std::sort(samples.begin(), samples.end());
  for (double q : {0.5, 0.9, 0.99, 0.999, 0.9999}) {
    const auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(samples.size())));
    const auto exact = static_cast<double>(samples[rank - 1]);
    const auto got = static_cast<double>(h.quantile(q));
    EXPECT_GE(got, exact) << "never optimistic, q=" << q;
    EXPECT_LE(got, exact * (1.0 + 1.0 / 128) + 1) << "q=" << q;
  }
  EXPECT_EQ(h.max(), samples.back());
}

TEST(LatencyHistogram, MergeAndClear) {
  LatencyHistogram a, b;
  a.record(10);
  b.record(1000);
  b.record(5);
  a.merge(b);
  EXPECT_EQ(a.count(), 3u);
  EXPECT_EQ(a.min(), 5u);
  EXPECT_EQ(a.max(), 1000u);
  a.clear();
  EXPECT_EQ(a.count(), 0u);
  EXPECT_EQ(a.quantile(0.5), 0u);
  EXPECT_EQ(a.min(), 0u);
}

// ---------------------------------------------------------------------------------------------
// Clock

TEST(Clock, IsMonotonicAndCalibrated) {
  const auto t0 = clock::now();
  const auto t1 = clock::now();
  EXPECT_GE(t1, t0);
  EXPECT_GT(clock::ticks_per_ns(), 0.0);
  EXPECT_GT(clock::resolution_ns(), 0.0);
}

// ---------------------------------------------------------------------------------------------
// Command

TEST(Command, RoundTripsANewOrder) {
  NewOrder o;
  o.id = 77;
  o.side = Side::Sell;
  o.price = -12;
  o.qty = 300;
  o.owner = 4;
  o.type = OrderType::Limit;
  o.tif = TimeInForce::FOK;
  o.post_only = false;
  const NewOrder back = Command::add(o).to_new_order();
  EXPECT_EQ(back.id, o.id);
  EXPECT_EQ(back.side, o.side);
  EXPECT_EQ(back.price, o.price);
  EXPECT_EQ(back.qty, o.qty);
  EXPECT_EQ(back.owner, o.owner);
  EXPECT_EQ(back.tif, o.tif);
  EXPECT_FALSE(back.post_only);
  NewOrder p = o;
  p.tif = TimeInForce::GTC;
  p.post_only = true;
  EXPECT_TRUE(Command::add(p).to_new_order().post_only);
}

TEST(Command, ApplyDispatchesEveryType) {
  EventRecorder rec;
  OrderBook<EventRecorder> book(BookConfig{}, rec);
  NewOrder o;
  o.id = 1;
  o.price = 100;
  o.qty = 10;
  apply(book, Command::add(o));
  apply(book, Command::modify(1, 100, 4));
  EXPECT_EQ(book.find(1)->qty, 4u);
  apply(book, Command::cancel(1));
  EXPECT_EQ(book.find(1), nullptr);
}

// ---------------------------------------------------------------------------------------------
// Workload

TEST(Workload, IsDeterministic) {
  WorkloadSpec spec;
  spec.requests = 20'000;
  const Workload a = generate(spec);
  const Workload b = generate(spec);
  ASSERT_EQ(a.commands.size(), b.commands.size());
  EXPECT_EQ(0, std::memcmp(a.commands.data(), b.commands.data(),
                           a.commands.size() * sizeof(Command)));
  spec.seed = 2;
  const Workload c = generate(spec);
  EXPECT_NE(0, std::memcmp(a.commands.data(), c.commands.data(),
                           a.commands.size() * sizeof(Command)));
}

TEST(Workload, EveryCancelAndModifyTargetsALiveOrder) {
  for (const WorkloadSpec& base : standard_workloads(30'000)) {
    WorkloadSpec spec = base;
    if (spec.target_depth > 100'000) continue;  // the deep book is covered by the benchmark
    const Workload w = generate(spec);
    EventRecorder rec;
    OrderBook<EventRecorder> book(book_config_for(spec), rec);
    std::uint64_t unknown = 0;
    for (const Command& c : w.commands) {
      apply(book, c);
      for (const Event& e : rec.events)
        unknown += e.kind == EventKind::Reject &&
                   e.reason == static_cast<std::uint8_t>(RejectReason::UnknownOrder);
      rec.events.clear();
    }
    EXPECT_EQ(unknown, 0u) << spec.name;
    EXPECT_EQ(w.stats.rejects, 0u) << spec.name;
    EXPECT_GT(w.stats.trades, 0u) << spec.name;
    EXPECT_EQ(w.stats.adds + w.stats.takes + w.stats.cancels + w.stats.modifies, spec.requests);
  }
}

TEST(Workload, SettlesNearTargetDepth) {
  WorkloadSpec spec;
  spec.requests = 200'000;
  spec.target_depth = 3'000;
  const Workload w = generate(spec);
  EXPECT_GT(w.stats.mean_depth, 1'500);
  EXPECT_LT(w.stats.mean_depth, 6'000);
}

}  // namespace
