#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "lob/flat_order_map.hpp"

namespace {

using lob::FlatOrderMap;
using lob::kNil;
using lob::OrderId;
using lob::OrderIdx;

TEST(FlatOrderMap, InsertFindErase) {
  FlatOrderMap m(100);
  EXPECT_EQ(m.find(7), kNil);
  EXPECT_TRUE(m.insert(7, 3));
  EXPECT_FALSE(m.insert(7, 4)) << "duplicate keys must be refused";
  EXPECT_EQ(m.find(7), 3u);
  EXPECT_EQ(m.size(), 1u);
  EXPECT_TRUE(m.erase(7));
  EXPECT_FALSE(m.erase(7));
  EXPECT_EQ(m.find(7), kNil);
  EXPECT_EQ(m.size(), 0u);
}

TEST(FlatOrderMap, SizedForHalfLoad) {
  FlatOrderMap m(1000);
  EXPECT_EQ(m.slot_count(), 2048u);
  FlatOrderMap tiny(1);
  EXPECT_EQ(tiny.slot_count(), 16u);
}

TEST(FlatOrderMap, SequentialIdsSpreadAcrossTable) {
  FlatOrderMap m(1 << 16);
  std::vector<int> hits(m.slot_count(), 0);
  for (OrderId id = 1; id <= (1 << 16); ++id) ++hits[m.home(id)];
  int max_hits = 0;
  for (int h : hits) max_hits = std::max(max_hits, h);
  EXPECT_LE(max_hits, 2) << "Fibonacci hashing should place sequential ids almost perfectly";
}

/// Finds `n` distinct keys that all hash to the same home slot.
std::vector<OrderId> colliding_keys(const FlatOrderMap& m, std::size_t slot, std::size_t n) {
  std::vector<OrderId> keys;
  for (OrderId k = 1; keys.size() < n; ++k)
    if (m.home(k) == slot) keys.push_back(k);
  return keys;
}

TEST(FlatOrderMap, EraseFromMiddleOfProbeRunKeepsLaterKeysReachable) {
  FlatOrderMap m(8);  // 16 slots
  const auto keys = colliding_keys(m, 5, 5);
  for (std::size_t i = 0; i < keys.size(); ++i) ASSERT_TRUE(m.insert(keys[i], OrderIdx(i)));

  ASSERT_TRUE(m.erase(keys[1]));
  ASSERT_TRUE(m.erase(keys[0]));
  EXPECT_EQ(m.find(keys[0]), kNil);
  EXPECT_EQ(m.find(keys[1]), kNil);
  for (std::size_t i = 2; i < keys.size(); ++i) EXPECT_EQ(m.find(keys[i]), OrderIdx(i));
}

TEST(FlatOrderMap, ProbeRunsWrapAroundTheEndOfTheTable) {
  FlatOrderMap m(8);  // 16 slots
  const std::size_t last = m.slot_count() - 1;
  const auto wrap = colliding_keys(m, last, 4);  // occupies 15, 0, 1, 2
  const auto zero = colliding_keys(m, 0, 2);     // displaced to 3, 4
  for (std::size_t i = 0; i < wrap.size(); ++i) ASSERT_TRUE(m.insert(wrap[i], OrderIdx(i)));
  for (std::size_t i = 0; i < zero.size(); ++i) ASSERT_TRUE(m.insert(zero[i], OrderIdx(10 + i)));

  ASSERT_TRUE(m.erase(wrap[0]));  // hole at the last slot; the backward shift must wrap
  for (std::size_t i = 1; i < wrap.size(); ++i) EXPECT_EQ(m.find(wrap[i]), OrderIdx(i));
  for (std::size_t i = 0; i < zero.size(); ++i) EXPECT_EQ(m.find(zero[i]), OrderIdx(10 + i));

  ASSERT_TRUE(m.erase(zero[0]));
  EXPECT_EQ(m.find(zero[1]), 11u);
  for (std::size_t i = 1; i < wrap.size(); ++i) EXPECT_EQ(m.find(wrap[i]), OrderIdx(i));
}

TEST(FlatOrderMap, RandomOperationsMatchStdUnorderedMap) {
  constexpr std::size_t kCapacity = 4096;
  FlatOrderMap m(kCapacity);
  std::unordered_map<OrderId, OrderIdx> ref;
  std::vector<OrderId> live;
  std::mt19937_64 rng(12345);

  for (int step = 0; step < 400'000; ++step) {
    const auto action = rng() % 10;
    if (action < 5 && ref.size() < kCapacity) {
      // A narrow key range forces duplicates and long probe runs.
      const OrderId key = 1 + rng() % 20'000;
      const auto value = static_cast<OrderIdx>(rng() % 1'000'000);
      const bool inserted = ref.emplace(key, value).second;
      ASSERT_EQ(m.insert(key, value), inserted);
      if (inserted) live.push_back(key);
    } else if (action < 9 && !live.empty()) {
      const std::size_t pos = rng() % live.size();
      const OrderId key = live[pos];
      live[pos] = live.back();
      live.pop_back();
      ASSERT_TRUE(m.erase(key));
      ref.erase(key);
    } else {
      const OrderId key = 1 + rng() % 20'000;
      const auto it = ref.find(key);
      ASSERT_EQ(m.find(key), it == ref.end() ? kNil : it->second);
    }
    ASSERT_EQ(m.size(), ref.size());
  }
  for (const auto& [key, value] : ref) EXPECT_EQ(m.find(key), value);
}

}  // namespace
