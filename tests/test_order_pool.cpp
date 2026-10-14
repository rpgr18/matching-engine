#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "lob/order_pool.hpp"

namespace {

using lob::kNil;
using lob::OrderIdx;
using lob::OrderPool;

TEST(OrderPool, AllocatesEverySlotExactlyOnce) {
  OrderPool pool(64);
  std::vector<OrderIdx> got;
  for (int i = 0; i < 64; ++i) {
    const OrderIdx idx = pool.allocate();
    ASSERT_NE(idx, kNil);
    got.push_back(idx);
  }
  EXPECT_TRUE(pool.full());
  EXPECT_EQ(pool.size(), 64u);
  EXPECT_EQ(pool.allocate(), kNil);

  std::sort(got.begin(), got.end());
  for (OrderIdx i = 0; i < 64; ++i) EXPECT_EQ(got[i], i);
}

TEST(OrderPool, ReusesMostRecentlyReleasedSlotFirst) {
  OrderPool pool(8);
  const OrderIdx a = pool.allocate();
  const OrderIdx b = pool.allocate();
  const OrderIdx c = pool.allocate();
  pool.release(a);
  pool.release(c);
  EXPECT_EQ(pool.allocate(), c);
  EXPECT_EQ(pool.allocate(), a);
  EXPECT_NE(pool.allocate(), b);
  EXPECT_EQ(pool.size(), 4u);
}

TEST(OrderPool, RecoversFromFull) {
  OrderPool pool(2);
  const OrderIdx a = pool.allocate();
  (void)pool.allocate();
  ASSERT_TRUE(pool.full());
  pool.release(a);
  EXPECT_FALSE(pool.full());
  EXPECT_EQ(pool.allocate(), a);
}

TEST(OrderPool, ZeroCapacityIsAlwaysFull) {
  OrderPool pool(0);
  EXPECT_TRUE(pool.full());
  EXPECT_EQ(pool.allocate(), kNil);
  EXPECT_EQ(pool.capacity(), 0u);
}

TEST(OrderPool, SlotsAreWritable) {
  OrderPool pool(4);
  const OrderIdx idx = pool.allocate();
  pool[idx].id = 42;
  pool[idx].qty = 7;
  const OrderPool& view = pool;
  EXPECT_EQ(view[idx].id, 42u);
  EXPECT_EQ(view[idx].qty, 7u);
}

}  // namespace
