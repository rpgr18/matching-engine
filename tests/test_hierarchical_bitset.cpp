#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <set>

#include "lob/hierarchical_bitset.hpp"

namespace {

using lob::HierarchicalBitset;
constexpr auto npos = HierarchicalBitset::npos;

std::size_t ref_next(const std::set<std::size_t>& s, std::size_t i) {
  const auto it = s.lower_bound(i);
  return it == s.end() ? npos : *it;
}

std::size_t ref_prev(const std::set<std::size_t>& s, std::size_t i) {
  auto it = s.upper_bound(i);
  return it == s.begin() ? npos : *--it;
}

TEST(HierarchicalBitset, EmptyFindsNothing) {
  HierarchicalBitset b(1000);
  EXPECT_TRUE(b.none());
  EXPECT_EQ(b.find_next(0), npos);
  EXPECT_EQ(b.find_prev(999), npos);
  EXPECT_EQ(b.find_next(1000), npos);
}

TEST(HierarchicalBitset, ZeroSize) {
  HierarchicalBitset b(0);
  EXPECT_TRUE(b.none());
  EXPECT_EQ(b.find_next(0), npos);
  EXPECT_EQ(b.find_prev(0), npos);
}

TEST(HierarchicalBitset, SingleBitIsFoundFromEitherDirection) {
  HierarchicalBitset b(300'000);  // four layers
  b.set(123'456);
  EXPECT_TRUE(b.test(123'456));
  EXPECT_EQ(b.find_next(0), 123'456u);
  EXPECT_EQ(b.find_next(123'456), 123'456u);
  EXPECT_EQ(b.find_next(123'457), npos);
  EXPECT_EQ(b.find_prev(299'999), 123'456u);
  EXPECT_EQ(b.find_prev(123'456), 123'456u);
  EXPECT_EQ(b.find_prev(123'455), npos);
  b.reset(123'456);
  EXPECT_TRUE(b.none());
  EXPECT_EQ(b.find_next(0), npos);
}

TEST(HierarchicalBitset, ResetKeepsSummaryWhileWordIsNonEmpty) {
  HierarchicalBitset b(4096);
  b.set(64);
  b.set(65);
  b.reset(64);
  EXPECT_EQ(b.find_next(0), 65u);
  EXPECT_EQ(b.find_prev(4095), 65u);
  b.reset(65);
  EXPECT_TRUE(b.none());
}

TEST(HierarchicalBitset, WordBoundaries) {
  HierarchicalBitset b(64 * 64 * 64 + 1);
  for (std::size_t i : {0u, 63u, 64u, 4095u, 4096u, 262'143u, 262'144u}) b.set(i);
  EXPECT_EQ(b.find_next(1), 63u);
  EXPECT_EQ(b.find_next(65), 4095u);
  EXPECT_EQ(b.find_next(4097), 262'143u);
  EXPECT_EQ(b.find_next(262'144), 262'144u);
  EXPECT_EQ(b.find_prev(262'142), 4096u);
  EXPECT_EQ(b.find_prev(4094), 64u);
  EXPECT_EQ(b.find_prev(62), 0u);
  EXPECT_EQ(b.find_prev(npos), 262'144u) << "positions past the end clamp to the last bit";
}

class HierarchicalBitsetRandom : public ::testing::TestWithParam<std::size_t> {};

TEST_P(HierarchicalBitsetRandom, MatchesStdSet) {
  const std::size_t n = GetParam();
  HierarchicalBitset b(n);
  std::set<std::size_t> ref;
  std::mt19937_64 rng(n);

  for (int step = 0; step < 200'000; ++step) {
    const std::size_t i = rng() % n;
    switch (rng() % 4) {
      case 0:
        b.set(i);
        ref.insert(i);
        break;
      case 1:
        b.reset(i);
        ref.erase(i);
        break;
      case 2:
        ASSERT_EQ(b.find_next(i), ref_next(ref, i)) << "n=" << n << " i=" << i;
        break;
      default:
        ASSERT_EQ(b.find_prev(i), ref_prev(ref, i)) << "n=" << n << " i=" << i;
        break;
    }
    if (step % 1024 == 0) {
      ASSERT_EQ(b.none(), ref.empty());
      // Sparse regimes exercise the multi-layer climb; clear everything periodically.
      if (rng() % 8 == 0) {
        for (std::size_t v : ref) b.reset(v);
        ref.clear();
        ASSERT_TRUE(b.none());
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Sizes, HierarchicalBitsetRandom,
                         ::testing::Values(1, 63, 64, 65, 4095, 4096, 4097, 262'145, 1 << 22));

}  // namespace
