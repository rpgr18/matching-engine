// Differential testing: drive the real book and the reference model with the same random request
// stream and require byte-identical event streams after every request, plus identical book
// contents at regular checkpoints.
//
// The default run is sized for CI. For a soak run:
//   LOB_FUZZ_OPS=10000000 LOB_FUZZ_SEED=$RANDOM ./build/tests/lob_tests --gtest_filter='Differential*'

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "book_test_util.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "reference_book.hpp"

namespace {

using namespace lob;
using namespace lob::test;

std::uint64_t env_or(const char* name, std::uint64_t fallback) {
  const char* v = std::getenv(name);
  return v ? std::strtoull(v, nullptr, 10) : fallback;
}

/// The shape of a random workload.
struct Scenario {
  const char* name;
  BookConfig config;
  Price centre;    ///< Prices cluster around here...
  Price spread;    ///< ...within +/- this many ticks...
  Price outliers;  ///< ...except occasional prices anywhere in [min - outliers, max + outliers].
  OwnerId owners;  ///< Owner ids are drawn from [0, owners].
  std::uint64_t ops;
};

enum class OpKind { Add, Cancel, Modify };

struct Op {
  OpKind kind;
  NewOrder order;  // Add
  OrderId id = 0;  // Cancel, Modify
  Price price = 0; // Modify
  Qty qty = 0;     // Modify

  std::string describe() const {
    std::ostringstream os;
    switch (kind) {
      case OpKind::Add:
        os << "add id=" << order.id << ' ' << to_string(order.side) << ' ' << order.qty << '@'
           << order.price << ' ' << to_string(order.type) << ' ' << to_string(order.tif)
           << (order.post_only ? " post-only" : "") << " owner=" << order.owner;
        break;
      case OpKind::Cancel:
        os << "cancel id=" << id;
        break;
      case OpKind::Modify:
        os << "modify id=" << id << ' ' << qty << '@' << price;
        break;
    }
    return os.str();
  }
};

/// Random request generator. It never looks inside either book: it remembers recently used ids
/// and draws cancels and modifies from them, so some target live orders and some target orders
/// that have since filled, which is exactly the race real gateways see.
class OpGenerator {
 public:
  OpGenerator(const Scenario& s, std::uint64_t seed) : s_(s), rng_(seed) {}

  Op next() {
    const auto roll = below(100);
    if (roll < 55 || recent_.empty()) return add();
    if (roll < 80) return Op{OpKind::Cancel, {}, pick_id(), 0, 0};
    const OrderId id = pick_id();
    const Price price = chance(30) ? last_price_[id % kSlots] : price_near(last_price_[id % kSlots]);
    const Qty qty = chance(3) ? 0 : static_cast<Qty>(1 + below(25));
    return Op{OpKind::Modify, {}, id, price, qty};
  }

 private:
  static constexpr std::size_t kSlots = 1024;

  std::uint64_t below(std::uint64_t n) { return rng_() % n; }
  bool chance(std::uint64_t percent) { return below(100) < percent; }

  Price price_near(Price around) {
    if (chance(2)) {
      const Price lo = s_.config.min_price - s_.outliers;
      const Price width = s_.config.max_price - s_.config.min_price + 2 * s_.outliers + 1;
      return lo + static_cast<Price>(below(static_cast<std::uint64_t>(width)));
    }
    const auto width = static_cast<std::uint64_t>(2 * s_.spread + 1);
    return around - s_.spread + static_cast<Price>(below(width));
  }

  OrderId pick_id() {
    if (chance(5)) return chance(50) ? 0 : 1'000'000'000 + below(1000);  // never issued
    return recent_[below(recent_.size())];
  }

  Op add() {
    NewOrder o;
    if (chance(2)) {
      o.id = 0;
    } else if (chance(3) && !recent_.empty()) {
      o.id = recent_[below(recent_.size())];  // likely a duplicate of a live order
    } else {
      o.id = next_id_++;
    }
    o.side = chance(50) ? Side::Buy : Side::Sell;
    o.price = price_near(s_.centre);
    o.qty = chance(2) ? 0 : static_cast<Qty>(1 + below(20));
    o.owner = static_cast<OwnerId>(below(s_.owners + 1));
    o.type = chance(10) ? OrderType::Market : OrderType::Limit;
    const auto tif = below(100);
    o.tif = tif < 70 ? TimeInForce::GTC : tif < 85 ? TimeInForce::IOC : TimeInForce::FOK;
    o.post_only = chance(10);

    if (o.id != 0) {
      if (recent_.size() == 256) recent_.pop_front();
      recent_.push_back(o.id);
      last_price_[o.id % kSlots] = o.price;
    }
    return Op{OpKind::Add, o};
  }

  Scenario s_;
  std::mt19937_64 rng_;
  OrderId next_id_ = 1;
  std::deque<OrderId> recent_;
  Price last_price_[kSlots] = {};
};

/// How often each outcome occurred, so a scenario can assert it reached every code path.
struct Coverage {
  std::uint64_t trades = 0;
  std::uint64_t modifies = 0;
  std::uint64_t rejects[8] = {};
  std::uint64_t cancels[4] = {};

  void count(const Event& e) {
    switch (e.kind) {
      case EventKind::Trade: ++trades; break;
      case EventKind::Modify: ++modifies; break;
      case EventKind::Reject: ++rejects[e.reason]; break;
      case EventKind::Cancel: ++cancels[e.reason]; break;
      default: break;
    }
  }
  void merge(const Coverage& o) {
    trades += o.trades;
    modifies += o.modifies;
    for (int i = 0; i < 8; ++i) rejects[i] += o.rejects[i];
    for (int i = 0; i < 4; ++i) cancels[i] += o.cancels[i];
  }
};

template <class Traits>
Coverage run(const Scenario& s, std::uint64_t seed) {
  Coverage cov;
  EventRecorder rec;
  OrderBook<EventRecorder, Traits> book(s.config, rec);
  std::vector<Event> expected;
  ReferenceBook ref(s.config, expected);
  OpGenerator gen(s, seed);

  const std::uint64_t ops = env_or("LOB_FUZZ_OPS", s.ops);
  for (std::uint64_t step = 0; step < ops; ++step) {
    const Op op = gen.next();
    switch (op.kind) {
      case OpKind::Add:
        book.add(op.order);
        ref.add(op.order);
        break;
      case OpKind::Cancel:
        book.cancel(op.id);
        ref.cancel(op.id);
        break;
      case OpKind::Modify:
        book.modify(op.id, op.price, op.qty);
        ref.modify(op.id, op.price, op.qty);
        break;
    }

    EXPECT_EQ(rec.events, expected) << s.name << " seed=" << seed << " step=" << step << ": "
                                    << op.describe();
    if (::testing::Test::HasFailure()) return cov;
    for (const Event& e : expected) cov.count(e);
    rec.events.clear();
    expected.clear();

    // The book must never be left crossed.
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    EXPECT_TRUE(!bid || !ask || *bid < *ask) << "crossed at step " << step;

    if (step % 64 == 0 || step + 1 == ops) {
      EXPECT_EQ(resting(book, Side::Buy), ref.resting(Side::Buy)) << "step " << step;
      EXPECT_EQ(resting(book, Side::Sell), ref.resting(Side::Sell)) << "step " << step;
      EXPECT_EQ(book.order_count(), ref.order_count());
      EXPECT_EQ(book.level_count(Side::Buy), ref.level_count(Side::Buy));
      EXPECT_EQ(book.level_count(Side::Sell), ref.level_count(Side::Sell));
      EXPECT_EQ(bid, ref.best(Side::Buy));
      EXPECT_EQ(ask, ref.best(Side::Sell));
    }
    if (::testing::Test::HasFailure()) return cov;
  }
  // Guard against a generator that silently stops exercising the matching path.
  EXPECT_GT(cov.trades, ops / 20) << s.name;
  return cov;
}

BookConfig make_config(Price lo, Price hi, std::uint32_t max_orders, StpMode stp) {
  BookConfig c;
  c.min_price = lo;
  c.max_price = hi;
  c.max_orders = max_orders;
  c.stp = stp;
  return c;
}

template <class Traits>
class Differential : public ::testing::Test {};
TYPED_TEST_SUITE(Differential, AllBookTraits, BookTraitsName);

/// A narrow band and a small book: constant crossing, sweeps, queue churn and capacity rejects.
TYPED_TEST(Differential, TightBandEveryStpMode) {
  const std::uint64_t seed = env_or("LOB_FUZZ_SEED", 1);
  Coverage total;
  for (StpMode stp : {StpMode::None, StpMode::CancelResting, StpMode::CancelAggressor,
                      StpMode::CancelBoth}) {
    SCOPED_TRACE(to_string(stp));
    const Scenario s{"tight", make_config(1, 64, 48, stp), 32, 6, 3, 3, 40'000};
    total.merge(run<TypeParam>(s, seed + static_cast<std::uint64_t>(stp)));
    if (this->HasFailure()) return;
  }
  // Every reject and cancel path must have been reached at least once.
  for (int r = 0; r < 8; ++r)
    EXPECT_GT(total.rejects[r], 0u) << to_string(static_cast<RejectReason>(r));
  for (int c = 0; c < 4; ++c)
    EXPECT_GT(total.cancels[c], 0u) << to_string(static_cast<CancelReason>(c));
  EXPECT_GT(total.modifies, 0u);
}

/// A deep book: thousands of resting orders, so queues are long and cancels hit the middle.
TYPED_TEST(Differential, DeepBook) {
  const Scenario s{"deep", make_config(1, 400, 4000, StpMode::CancelResting), 200, 40, 10, 50,
                   60'000};
  run<TypeParam>(s, env_or("LOB_FUZZ_SEED", 1) + 100);
}

/// A wide band with a sparse book: the dense ladder's bitset has to climb several layers to find
/// the next level, and the vector ladder leaves its linear-scan window.
TYPED_TEST(Differential, WideSparseBand) {
  const Scenario s{"wide", make_config(-(1 << 20), 1 << 20, 2000, StpMode::None), 0, 5000,
                   1000, 5, 40'000};
  run<TypeParam>(s, env_or("LOB_FUZZ_SEED", 1) + 200);
}

}  // namespace
