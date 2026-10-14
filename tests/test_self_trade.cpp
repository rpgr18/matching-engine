#include <gtest/gtest.h>

#include "book_test_util.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"

namespace {

using namespace lob;
using namespace lob::test;

using E = Event;

constexpr OwnerId kAlice = 7;
constexpr OwnerId kBob = 8;

template <class Traits>
struct StpBook {
  explicit StpBook(StpMode mode) : book(make_config(mode), rec) {}

  static BookConfig make_config(StpMode mode) {
    BookConfig c;
    c.min_price = 1;
    c.max_price = 1000;
    c.max_orders = 64;
    c.stp = mode;
    return c;
  }

  Events submit(const NewOrder& o) {
    book.add(o);
    return rec.take();
  }

  EventRecorder rec;
  OrderBook<EventRecorder, Traits> book;
};

template <class Traits>
class SelfTrade : public ::testing::Test {};
TYPED_TEST_SUITE(SelfTrade, AllBookTraits, BookTraitsName);

TYPED_TEST(SelfTrade, AllowedWhenDisabled) {
  StpBook<TypeParam> b(StpMode::None);
  b.submit(sell(1, 10, 5, kAlice));
  EXPECT_EQ(b.submit(buy(2, 10, 5, kAlice)),
            (Events{E::accept(2), E::trade(Trade{2, 1, 10, 5, 0, 0, Side::Buy}),
                    E::level(Side::Sell, 10, 0, 0)}));
}

TYPED_TEST(SelfTrade, AnonymousOrdersAreNeverPrevented) {
  StpBook<TypeParam> b(StpMode::CancelBoth);
  b.submit(sell(1, 10, 5, 0));
  EXPECT_EQ(b.submit(buy(2, 10, 5, 0)).size(), 3u);  // accept, trade, level
}

TYPED_TEST(SelfTrade, DifferentOwnersTradeNormally) {
  StpBook<TypeParam> b(StpMode::CancelBoth);
  b.submit(sell(1, 10, 5, kBob));
  EXPECT_EQ(b.submit(buy(2, 10, 5, kAlice)).size(), 3u);
}

TYPED_TEST(SelfTrade, CancelRestingRemovesOwnOrdersAndKeepsMatching) {
  StpBook<TypeParam> b(StpMode::CancelResting);
  b.submit(sell(1, 10, 5, kAlice));
  b.submit(sell(2, 10, 5, kBob));
  b.submit(sell(3, 11, 5, kAlice));
  EXPECT_EQ(b.submit(buy(4, 11, 8, kAlice)),
            (Events{E::accept(4), E::cancel(1, 5, CancelReason::SelfTrade),
                    E::trade(Trade{4, 2, 10, 5, 3, 0, Side::Buy}), E::level(Side::Sell, 10, 0, 0),
                    E::cancel(3, 5, CancelReason::SelfTrade), E::level(Side::Sell, 11, 0, 0),
                    E::level(Side::Buy, 11, 3, 1)}));
  EXPECT_FALSE(b.book.best_ask());
  EXPECT_EQ(b.book.best_bid(), 11);
}

TYPED_TEST(SelfTrade, CancelAggressorStopsAtOwnOrderAndCancelsTheRemainder) {
  StpBook<TypeParam> b(StpMode::CancelAggressor);
  b.submit(sell(1, 10, 2, kBob));
  b.submit(sell(2, 10, 5, kAlice));
  b.submit(sell(3, 10, 5, kBob));
  EXPECT_EQ(b.submit(buy(4, 10, 5, kAlice)),
            (Events{E::accept(4), E::trade(Trade{4, 1, 10, 2, 3, 0, Side::Buy}),
                    E::level(Side::Sell, 10, 10, 2), E::cancel(4, 3, CancelReason::SelfTrade)}));
  EXPECT_FALSE(b.book.best_bid()) << "a GTC aggressor cancelled by STP does not rest";
  EXPECT_EQ(resting(b.book, Side::Sell), (std::vector<Resting>{{2, 10, 5}, {3, 10, 5}}));
}

TYPED_TEST(SelfTrade, CancelAggressorAtHeadOfLevelReportsNoLevelChange) {
  StpBook<TypeParam> b(StpMode::CancelAggressor);
  b.submit(sell(1, 10, 5, kAlice));
  EXPECT_EQ(b.submit(buy(2, 10, 5, kAlice)),
            (Events{E::accept(2), E::cancel(2, 5, CancelReason::SelfTrade)}));
}

TYPED_TEST(SelfTrade, CancelBothRemovesOneRestingOrderAndTheAggressor) {
  StpBook<TypeParam> b(StpMode::CancelBoth);
  b.submit(sell(1, 10, 5, kAlice));
  b.submit(sell(2, 10, 5, kAlice));
  EXPECT_EQ(b.submit(buy(3, 10, 5, kAlice)),
            (Events{E::accept(3), E::cancel(1, 5, CancelReason::SelfTrade),
                    E::level(Side::Sell, 10, 5, 1), E::cancel(3, 5, CancelReason::SelfTrade)}));
  EXPECT_EQ(resting(b.book, Side::Sell), (std::vector<Resting>{{2, 10, 5}}));
}

TYPED_TEST(SelfTrade, AppliesToReplacedOrdersTradingOnTheWayIn) {
  StpBook<TypeParam> b(StpMode::CancelAggressor);
  b.submit(buy(1, 9, 5, kAlice));
  b.submit(sell(2, 10, 5, kAlice));
  b.book.modify(1, 10, 5);
  EXPECT_EQ(b.rec.take(), (Events{E::modify(1, 10, 5), E::level(Side::Buy, 9, 0, 0),
                                  E::cancel(1, 5, CancelReason::SelfTrade)}));
  EXPECT_EQ(b.book.find(1), nullptr);
  EXPECT_EQ(b.book.order_count(), 1u);
}

TYPED_TEST(SelfTrade, FokDoesNotCountLiquidityBehindOwnOrderWhenAggressorWouldStop) {
  StpBook<TypeParam> b(StpMode::CancelAggressor);
  b.submit(sell(1, 10, 2, kBob));
  b.submit(sell(2, 10, 5, kAlice));
  b.submit(sell(3, 10, 5, kBob));
  EXPECT_EQ(b.submit(fok(4, Side::Buy, 10, 3, kAlice)),
            (Events{E::accept(4), E::cancel(4, 3, CancelReason::FillOrKill)}));
  EXPECT_EQ(b.book.order_count(), 3u) << "a killed FOK has no side effects";
}

TYPED_TEST(SelfTrade, FokSkipsOwnOrdersWhenTheyWouldBeCancelled) {
  StpBook<TypeParam> b(StpMode::CancelResting);
  b.submit(sell(1, 10, 2, kBob));
  b.submit(sell(2, 10, 5, kAlice));
  b.submit(sell(3, 10, 5, kBob));
  EXPECT_EQ(b.submit(fok(4, Side::Buy, 10, 8, kAlice)),
            (Events{E::accept(4), E::cancel(4, 8, CancelReason::FillOrKill)}))
      << "only 7 lots of liquidity belong to other owners";
  EXPECT_EQ(b.submit(fok(5, Side::Buy, 10, 7, kAlice)),
            (Events{E::accept(5), E::trade(Trade{5, 1, 10, 2, 5, 0, Side::Buy}),
                    E::cancel(2, 5, CancelReason::SelfTrade),
                    E::trade(Trade{5, 3, 10, 5, 0, 0, Side::Buy}),
                    E::level(Side::Sell, 10, 0, 0)}));
}

}  // namespace
