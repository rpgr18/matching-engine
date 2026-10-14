#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "lob/dense_ladder.hpp"
#include "lob/map_ladder.hpp"
#include "lob/vector_ladder.hpp"

namespace {

using namespace lob;

template <template <Side> class L, Side S>
struct LadderCase {
  using Ladder = L<S>;
  static constexpr Side side = S;
};

template <class Case>
class LadderTest : public ::testing::Test {
 protected:
  using Ladder = typename Case::Ladder;
  static constexpr Side kSide = Case::side;
  static constexpr Price kMin = -100;
  static constexpr Price kMax = 5000;

  /// Best-first order, as both sides of a book see it.
  static bool better(Price a, Price b) { return kSide == Side::Buy ? a > b : a < b; }

  std::vector<Price> prices(const Ladder& l) const {
    std::vector<Price> out;
    l.for_each([&](Price p, const Level&) {
      out.push_back(p);
      return true;
    });
    return out;
  }

  Ladder ladder{kMin, kMax};
};

using Cases = ::testing::Types<
    LadderCase<DenseLadder, Side::Buy>, LadderCase<DenseLadder, Side::Sell>,
    LadderCase<MapLadder, Side::Buy>, LadderCase<MapLadder, Side::Sell>,
    LadderCase<VectorLadder, Side::Buy>, LadderCase<VectorLadder, Side::Sell>>;

struct CaseName {
  template <class T>
  static std::string GetName(int i) {
    static const char* names[] = {"DenseBid", "DenseAsk", "MapBid", "MapAsk", "VectorBid",
                                  "VectorAsk"};
    return names[i];
  }
};

TYPED_TEST_SUITE(LadderTest, Cases, CaseName);

TYPED_TEST(LadderTest, StartsEmpty) {
  EXPECT_TRUE(this->ladder.empty());
  EXPECT_EQ(this->ladder.size(), 0u);
  EXPECT_EQ(this->ladder.find(10), nullptr);
}

TYPED_TEST(LadderTest, BestIsTheMostAggressivePrice) {
  for (Price p : {50, 10, 70, 30}) this->ladder.insert(p).count = 1;
  const Price best = TestFixture::kSide == Side::Buy ? 70 : 10;
  EXPECT_EQ(this->ladder.best_price(), best);
  EXPECT_EQ(this->ladder.size(), 4u);
  const auto expect = TestFixture::kSide == Side::Buy ? std::vector<Price>{70, 50, 30, 10}
                                                      : std::vector<Price>{10, 30, 50, 70};
  EXPECT_EQ(this->prices(this->ladder), expect);
}

TYPED_TEST(LadderTest, InsertReturnsTheExistingLevel) {
  Level& a = this->ladder.insert(42);
  a.count = 3;
  a.total = 99;
  Level& b = this->ladder.insert(42);
  EXPECT_EQ(b.count, 3u);
  EXPECT_EQ(b.total, 99u);
  EXPECT_EQ(this->ladder.size(), 1u);
  EXPECT_EQ(&this->ladder.at(42), &b);
}

TYPED_TEST(LadderTest, ErasingTheBestPromotesTheNext) {
  for (Price p : {1, 2, 3}) this->ladder.insert(p).count = 1;
  const Price first = this->ladder.best_price();
  this->ladder.at(first).count = 0;
  this->ladder.erase_best();
  EXPECT_EQ(this->ladder.best_price(), 2);
  EXPECT_EQ(this->ladder.find(first), nullptr);
}

TYPED_TEST(LadderTest, BandEdgesAreUsable) {
  this->ladder.insert(TestFixture::kMin).count = 1;
  this->ladder.insert(TestFixture::kMax).count = 1;
  EXPECT_NE(this->ladder.find(TestFixture::kMin), nullptr);
  EXPECT_NE(this->ladder.find(TestFixture::kMax), nullptr);
  EXPECT_EQ(this->ladder.find(TestFixture::kMax + 1), nullptr);
  EXPECT_EQ(this->ladder.find(TestFixture::kMin - 1), nullptr);
}

TYPED_TEST(LadderTest, RandomOperationsMatchStdMap) {
  using Fixture = TestFixture;
  std::map<Price, int> ref;
  std::mt19937_64 rng(7);
  auto& lad = this->ladder;

  for (int step = 0; step < 100'000; ++step) {
    // Mostly near the middle, sometimes anywhere: exercises the vector's linear and binary search
    // paths and the bitset's multi-layer climb.
    const Price p = rng() % 4 == 0
                        ? Fixture::kMin + static_cast<Price>(rng() % (Fixture::kMax - Fixture::kMin + 1))
                        : 2000 + static_cast<Price>(rng() % 40);
    if (rng() % 2 == 0) {
      lad.insert(p).count = 1;
      ref[p] = 1;
    } else if (ref.contains(p)) {
      lad.at(p).count = 0;
      lad.erase(p);
      ref.erase(p);
    }
    ASSERT_EQ(lad.size(), ref.size());
    ASSERT_EQ(lad.empty(), ref.empty());
    if (!ref.empty()) {
      const Price best = Fixture::kSide == Side::Buy ? ref.rbegin()->first : ref.begin()->first;
      ASSERT_EQ(lad.best_price(), best);
    }
    ASSERT_EQ(lad.find(p) != nullptr, ref.contains(p));
  }

  std::vector<Price> expect;
  for (const auto& [p, unused] : ref) expect.push_back(p);
  if (Fixture::kSide == Side::Buy) std::reverse(expect.begin(), expect.end());
  EXPECT_EQ(this->prices(lad), expect);
}

TEST(DenseLadder, RejectsBandsItCannotHold) {
  EXPECT_THROW((DenseLadder<Side::Buy>(10, 9)), std::invalid_argument);
  EXPECT_THROW((DenseLadder<Side::Buy>(0, Price{1} << 40)), std::invalid_argument);
  EXPECT_NO_THROW((DenseLadder<Side::Sell>(5, 5)));
}

}  // namespace
