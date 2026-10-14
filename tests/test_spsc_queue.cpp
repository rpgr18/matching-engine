#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <vector>

#include "lob/spsc_queue.hpp"

namespace {

using lob::SpscQueue;

TEST(SpscQueue, CapacityRoundsUpToAPowerOfTwo) {
  EXPECT_EQ(SpscQueue<int>(5).capacity(), 8u);
  EXPECT_EQ(SpscQueue<int>(8).capacity(), 8u);
  EXPECT_EQ(SpscQueue<int>(0).capacity(), 2u);
}

TEST(SpscQueue, FifoUntilFullThenRefuses) {
  SpscQueue<int> q(4);
  EXPECT_TRUE(q.empty());
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
  EXPECT_FALSE(q.try_push(99)) << "every slot is usable and a full queue refuses";
  EXPECT_EQ(q.size(), 4u);
  int v = -1;
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, i);
  }
  EXPECT_FALSE(q.try_pop(v));
}

TEST(SpscQueue, WrapsAroundManyTimes) {
  SpscQueue<std::uint64_t> q(8);
  std::uint64_t next_in = 0;
  std::uint64_t next_out = 0;
  for (int round = 0; round < 1000; ++round) {
    while (q.try_push(next_in)) ++next_in;
    std::uint64_t v;
    for (int i = 0; i < 5 && q.try_pop(v); ++i) ASSERT_EQ(v, next_out++);
  }
  std::uint64_t v;
  while (q.try_pop(v)) ASSERT_EQ(v, next_out++);
  EXPECT_EQ(next_in, next_out);
}

TEST(SpscQueue, ConsumeTakesABatch) {
  SpscQueue<int> q(16);
  for (int i = 0; i < 10; ++i) q.push(i);
  std::vector<int> got;
  EXPECT_EQ(q.consume([&](int v) { got.push_back(v); }, 4), 4u);
  EXPECT_EQ(q.consume([&](int v) { got.push_back(v); }, 100), 6u);
  EXPECT_EQ(q.consume([&](int v) { got.push_back(v); }, 100), 0u);
  EXPECT_EQ(got, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
}

struct Payload {
  std::uint64_t seq;
  std::uint64_t check;
  char pad[48];
};

TEST(SpscQueue, TwoThreadsTransferEveryElementInOrder) {
  constexpr std::uint64_t kCount = 2'000'000;
  SpscQueue<Payload> q(1024);
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kCount; ++i) q.push(Payload{i, i * 0x9E3779B97F4A7C15ull, {}});
  });
  std::uint64_t received = 0;
  bool ok = true;
  while (received < kCount) {
    q.consume(
        [&](const Payload& p) {
          ok &= p.seq == received && p.check == p.seq * 0x9E3779B97F4A7C15ull;
          ++received;
        },
        64);
  }
  producer.join();
  EXPECT_TRUE(ok);
  EXPECT_TRUE(q.empty());
}

}  // namespace
