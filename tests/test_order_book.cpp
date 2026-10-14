#include <gtest/gtest.h>

#include "book_test_util.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"

namespace {

using namespace lob;
using namespace lob::test;

using E = Event;
using Book = OrderBook<EventRecorder>;

template <class Traits>
class OrderBookTest : public ::testing::Test {
 protected:
  static BookConfig config() {
    BookConfig c;
    c.min_price = 1;
    c.max_price = 1000;
    c.max_orders = 64;
    return c;
  }

  Events submit(const NewOrder& o) {
    book.add(o);
    return rec.take();
  }
  Events cancel(OrderId id) {
    book.cancel(id);
    return rec.take();
  }
  Events modify(OrderId id, Price price, Qty qty) {
    book.modify(id, price, qty);
    return rec.take();
  }

  EventRecorder rec;
  OrderBook<EventRecorder, Traits> book{config(), rec};
};

TYPED_TEST_SUITE(OrderBookTest, AllBookTraits, BookTraitsName);

Trade fill(OrderId aggressor, OrderId resting_id, Price px, Qty qty, Qty leaves, Qty resting_leaves,
           Side side) {
  return Trade{aggressor, resting_id, px, qty, leaves, resting_leaves, side};
}

// ---------------------------------------------------------------------------------------------
// Resting and basic queries

TYPED_TEST(OrderBookTest, EmptyBook) {
  EXPECT_FALSE(this->book.best_bid());
  EXPECT_FALSE(this->book.best_ask());
  EXPECT_EQ(this->book.order_count(), 0u);
  EXPECT_EQ(this->book.level_count(Side::Buy), 0u);
  EXPECT_TRUE(this->book.depth(Side::Sell, 5).empty());
}

TYPED_TEST(OrderBookTest, NonCrossingOrdersRest) {
  EXPECT_EQ(this->submit(buy(1, 100, 10)), (Events{E::accept(1), E::level(Side::Buy, 100, 10, 1)}));
  EXPECT_EQ(this->submit(sell(2, 105, 7)), (Events{E::accept(2), E::level(Side::Sell, 105, 7, 1)}));
  EXPECT_EQ(this->book.best_bid(), 100);
  EXPECT_EQ(this->book.best_ask(), 105);
  EXPECT_EQ(this->book.order_count(), 2u);
  ASSERT_NE(this->book.find(1), nullptr);
  EXPECT_EQ(this->book.find(1)->qty, 10u);
  EXPECT_EQ(this->book.find(1)->side, Side::Buy);
}

TYPED_TEST(OrderBookTest, LevelsAggregateOrdersAtTheSamePrice) {
  this->submit(buy(1, 100, 10));
  EXPECT_EQ(this->submit(buy(2, 100, 5)), (Events{E::accept(2), E::level(Side::Buy, 100, 15, 2)}));
  EXPECT_EQ(this->book.level(Side::Buy, 100), (LevelInfo{100, 15, 2}));
  EXPECT_EQ(this->book.level(Side::Buy, 99), (LevelInfo{99, 0, 0}));
  EXPECT_EQ(this->book.level_count(Side::Buy), 1u);
}

TYPED_TEST(OrderBookTest, DepthIsBestFirstOnBothSides) {
  this->submit(buy(1, 98, 1));
  this->submit(buy(2, 100, 2));
  this->submit(buy(3, 99, 3));
  this->submit(sell(4, 103, 4));
  this->submit(sell(5, 101, 5));
  this->submit(sell(6, 102, 6));
  EXPECT_EQ(this->book.depth(Side::Buy, 10),
            (std::vector<LevelInfo>{{100, 2, 1}, {99, 3, 1}, {98, 1, 1}}));
  EXPECT_EQ(this->book.depth(Side::Sell, 2), (std::vector<LevelInfo>{{101, 5, 1}, {102, 6, 1}}));
}

// ---------------------------------------------------------------------------------------------
// Matching

TYPED_TEST(OrderBookTest, CrossingOrderTradesAtTheRestingPrice) {
  this->submit(sell(1, 100, 5));
  EXPECT_EQ(this->submit(buy(2, 103, 5)),
            (Events{E::accept(2), E::trade(fill(2, 1, 100, 5, 0, 0, Side::Buy)),
                    E::level(Side::Sell, 100, 0, 0)}));
  EXPECT_FALSE(this->book.best_ask());
  EXPECT_FALSE(this->book.best_bid());
  EXPECT_EQ(this->book.order_count(), 0u);
  EXPECT_EQ(this->book.find(1), nullptr);
}

TYPED_TEST(OrderBookTest, SellAggressorTradesAgainstHighestBidFirst) {
  this->submit(buy(1, 99, 5));
  this->submit(buy(2, 101, 5));
  this->submit(buy(3, 100, 5));
  EXPECT_EQ(this->submit(sell(4, 100, 8)),
            (Events{E::accept(4), E::trade(fill(4, 2, 101, 5, 3, 0, Side::Sell)),
                    E::level(Side::Buy, 101, 0, 0), E::trade(fill(4, 3, 100, 3, 0, 2, Side::Sell)),
                    E::level(Side::Buy, 100, 2, 1)}));
  EXPECT_EQ(this->book.best_bid(), 100);
}

TYPED_TEST(OrderBookTest, PriceThenTimePriority) {
  this->submit(sell(1, 11, 5));
  this->submit(sell(2, 10, 5));
  this->submit(sell(3, 10, 5));
  EXPECT_EQ(this->submit(buy(4, 11, 12)),
            (Events{E::accept(4), E::trade(fill(4, 2, 10, 5, 7, 0, Side::Buy)),
                    E::trade(fill(4, 3, 10, 5, 2, 0, Side::Buy)), E::level(Side::Sell, 10, 0, 0),
                    E::trade(fill(4, 1, 11, 2, 0, 3, Side::Buy)), E::level(Side::Sell, 11, 3, 1)}));
  EXPECT_EQ(resting(this->book, Side::Sell), (std::vector<Resting>{{1, 11, 3}}));
}

TYPED_TEST(OrderBookTest, UnfilledRemainderRestsAtItsLimit) {
  this->submit(sell(1, 10, 3));
  EXPECT_EQ(this->submit(buy(2, 10, 5)),
            (Events{E::accept(2), E::trade(fill(2, 1, 10, 3, 2, 0, Side::Buy)),
                    E::level(Side::Sell, 10, 0, 0), E::level(Side::Buy, 10, 2, 1)}));
  EXPECT_EQ(this->book.best_bid(), 10);
  EXPECT_EQ(this->book.find(2)->qty, 2u);
}

TYPED_TEST(OrderBookTest, LimitNeverTradesThroughItsPrice) {
  this->submit(sell(1, 10, 1));
  this->submit(sell(2, 11, 1));
  this->submit(buy(3, 10, 2));
  EXPECT_EQ(this->book.best_bid(), 10);
  EXPECT_EQ(this->book.best_ask(), 11);
  EXPECT_EQ(resting(this->book, Side::Sell), (std::vector<Resting>{{2, 11, 1}}));
  EXPECT_EQ(resting(this->book, Side::Buy), (std::vector<Resting>{{3, 10, 1}}));
}

TYPED_TEST(OrderBookTest, PartialFillKeepsRestingOrderAtFrontOfQueue) {
  this->submit(sell(1, 10, 10));
  this->submit(sell(2, 10, 10));
  this->submit(buy(3, 10, 4));
  EXPECT_EQ(resting(this->book, Side::Sell), (std::vector<Resting>{{1, 10, 6}, {2, 10, 10}}));
  this->submit(buy(4, 10, 7));
  EXPECT_EQ(resting(this->book, Side::Sell), (std::vector<Resting>{{2, 10, 9}}));
}

TYPED_TEST(OrderBookTest, SweepClearsSeveralLevelsAndRestsTheRemainder) {
  for (OrderId id = 1; id <= 5; ++id) this->submit(sell(id, 100 + static_cast<Price>(id), 2));
  this->submit(buy(10, 103, 10));
  EXPECT_EQ(this->book.best_ask(), 104);
  EXPECT_EQ(this->book.best_bid(), 103);
  EXPECT_EQ(this->book.find(10)->qty, 4u);
  EXPECT_EQ(this->book.level_count(Side::Sell), 2u);
}

// ---------------------------------------------------------------------------------------------
// Cancel

TYPED_TEST(OrderBookTest, CancelRemovesOrderAndReportsLevel) {
  this->submit(buy(1, 100, 10));
  this->submit(buy(2, 100, 4));
  EXPECT_EQ(this->cancel(1), (Events{E::cancel(1, 10, CancelReason::Requested),
                               E::level(Side::Buy, 100, 4, 1)}));
  EXPECT_EQ(this->cancel(2), (Events{E::cancel(2, 4, CancelReason::Requested),
                               E::level(Side::Buy, 100, 0, 0)}));
  EXPECT_FALSE(this->book.best_bid());
  EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, CancelUnknownIsRejected) {
  EXPECT_EQ(this->cancel(99), (Events{E::reject(99, RejectReason::UnknownOrder)}));
  EXPECT_EQ(this->cancel(0), (Events{E::reject(0, RejectReason::UnknownOrder)}));
}

TYPED_TEST(OrderBookTest, CancelAfterFullFillIsRejected) {
  this->submit(sell(1, 10, 5));
  this->submit(buy(2, 10, 5));
  EXPECT_EQ(this->cancel(1), (Events{E::reject(1, RejectReason::UnknownOrder)}));
}

TYPED_TEST(OrderBookTest, CancelFromAnyQueuePositionPreservesTheRest) {
  for (OrderId id = 1; id <= 5; ++id) this->submit(sell(id, 10, Qty(id)));
  this->cancel(3);  // middle
  this->cancel(1);  // head
  this->cancel(5);  // tail
  EXPECT_EQ(resting(this->book, Side::Sell), (std::vector<Resting>{{2, 10, 2}, {4, 10, 4}}));
  this->submit(sell(6, 10, 6));
  EXPECT_EQ(resting(this->book, Side::Sell),
            (std::vector<Resting>{{2, 10, 2}, {4, 10, 4}, {6, 10, 6}}));
  EXPECT_EQ(this->book.level(Side::Sell, 10), (LevelInfo{10, 12, 3}));
}

TYPED_TEST(OrderBookTest, CancellingBestLevelPromotesNextLevel) {
  this->submit(sell(1, 101, 1));
  this->submit(sell(2, 105, 1));
  this->submit(sell(3, 103, 1));
  this->cancel(1);
  EXPECT_EQ(this->book.best_ask(), 103);
  this->cancel(3);
  EXPECT_EQ(this->book.best_ask(), 105);
  this->cancel(2);
  EXPECT_FALSE(this->book.best_ask());
}

// ---------------------------------------------------------------------------------------------
// Modify (cancel/replace)

TYPED_TEST(OrderBookTest, ReducingQuantityKeepsQueuePriority) {
  this->submit(sell(1, 10, 5));
  this->submit(sell(2, 10, 5));
  EXPECT_EQ(this->modify(1, 10, 3), (Events{E::modify(1, 10, 3), E::level(Side::Sell, 10, 8, 2)}));
  EXPECT_EQ(resting(this->book, Side::Sell), (std::vector<Resting>{{1, 10, 3}, {2, 10, 5}}));
  this->submit(buy(3, 10, 3));
  EXPECT_EQ(resting(this->book, Side::Sell), (std::vector<Resting>{{2, 10, 5}}));
}

TYPED_TEST(OrderBookTest, IncreasingQuantityLosesQueuePriority) {
  this->submit(sell(1, 10, 5));
  this->submit(sell(2, 10, 5));
  EXPECT_EQ(this->modify(1, 10, 8), (Events{E::modify(1, 10, 8), E::level(Side::Sell, 10, 13, 2)}));
  EXPECT_EQ(resting(this->book, Side::Sell), (std::vector<Resting>{{2, 10, 5}, {1, 10, 8}}));
}

TYPED_TEST(OrderBookTest, ModifyToSameTermsIsAcceptedInPlace) {
  this->submit(sell(1, 10, 5));
  this->submit(sell(2, 10, 5));
  EXPECT_EQ(this->modify(1, 10, 5), (Events{E::modify(1, 10, 5), E::level(Side::Sell, 10, 10, 2)}));
  EXPECT_EQ(resting(this->book, Side::Sell), (std::vector<Resting>{{1, 10, 5}, {2, 10, 5}}));
}

TYPED_TEST(OrderBookTest, PriceChangeMovesOrderToBackOfNewLevel) {
  this->submit(sell(1, 10, 5));
  this->submit(sell(2, 11, 4));
  EXPECT_EQ(this->modify(1, 11, 5), (Events{E::modify(1, 11, 5), E::level(Side::Sell, 10, 0, 0),
                                      E::level(Side::Sell, 11, 9, 2)}));
  EXPECT_EQ(resting(this->book, Side::Sell), (std::vector<Resting>{{2, 11, 4}, {1, 11, 5}}));
  EXPECT_EQ(this->book.best_ask(), 11);
  EXPECT_EQ(this->book.level_count(Side::Sell), 1u);
}

TYPED_TEST(OrderBookTest, PriceChangeThatCrossesTradesThenRests) {
  this->submit(buy(1, 100, 5));
  this->submit(sell(2, 105, 3));
  EXPECT_EQ(this->modify(1, 105, 5),
            (Events{E::modify(1, 105, 5), E::level(Side::Buy, 100, 0, 0),
                    E::trade(fill(1, 2, 105, 3, 2, 0, Side::Buy)), E::level(Side::Sell, 105, 0, 0),
                    E::level(Side::Buy, 105, 2, 1)}));
  EXPECT_EQ(this->book.best_bid(), 105);
  EXPECT_EQ(this->book.find(1)->qty, 2u);
}

TYPED_TEST(OrderBookTest, PriceChangeThatFillsCompletelyRemovesTheOrder) {
  this->submit(buy(1, 100, 5));
  this->submit(sell(2, 105, 9));
  this->modify(1, 106, 5);
  EXPECT_EQ(this->book.find(1), nullptr);
  EXPECT_EQ(this->book.order_count(), 1u);
  EXPECT_EQ(this->book.find(2)->qty, 4u);
  EXPECT_EQ(this->cancel(1), (Events{E::reject(1, RejectReason::UnknownOrder)}));
}

TYPED_TEST(OrderBookTest, InvalidModifyLeavesOrderUntouched) {
  this->submit(buy(1, 100, 5));
  EXPECT_EQ(this->modify(2, 100, 5), (Events{E::reject(2, RejectReason::UnknownOrder)}));
  EXPECT_EQ(this->modify(1, 100, 0), (Events{E::reject(1, RejectReason::InvalidQuantity)}));
  EXPECT_EQ(this->modify(1, 5000, 5), (Events{E::reject(1, RejectReason::PriceOutOfBand)}));
  EXPECT_EQ(resting(this->book, Side::Buy), (std::vector<Resting>{{1, 100, 5}}));
}

TYPED_TEST(OrderBookTest, PostOnlyReplaceThatWouldCrossIsRejected) {
  this->submit(post_only(1, Side::Buy, 100, 5));
  this->submit(sell(2, 103, 5));
  EXPECT_EQ(this->modify(1, 103, 5), (Events{E::reject(1, RejectReason::WouldCross)}));
  EXPECT_EQ(resting(this->book, Side::Buy), (std::vector<Resting>{{1, 100, 5}}));
  EXPECT_EQ(this->modify(1, 102, 5), (Events{E::modify(1, 102, 5), E::level(Side::Buy, 100, 0, 0),
                                       E::level(Side::Buy, 102, 5, 1)}));
}

TYPED_TEST(OrderBookTest, MovingTheBestLevelAwayPromotesTheNext) {
  this->submit(sell(1, 10, 1));
  this->submit(sell(2, 12, 1));
  this->modify(1, 13, 1);
  EXPECT_EQ(this->book.best_ask(), 12);
  EXPECT_EQ(this->book.depth(Side::Sell, 5), (std::vector<LevelInfo>{{12, 1, 1}, {13, 1, 1}}));
}

// ---------------------------------------------------------------------------------------------
// Market and immediate-or-cancel

TYPED_TEST(OrderBookTest, MarketOrderSweepsAndCancelsTheRemainder) {
  this->submit(sell(1, 101, 2));
  this->submit(sell(2, 102, 3));
  EXPECT_EQ(this->submit(market(3, Side::Buy, 10)),
            (Events{E::accept(3), E::trade(fill(3, 1, 101, 2, 8, 0, Side::Buy)),
                    E::level(Side::Sell, 101, 0, 0), E::trade(fill(3, 2, 102, 3, 5, 0, Side::Buy)),
                    E::level(Side::Sell, 102, 0, 0), E::cancel(3, 5, CancelReason::Unfilled)}));
  EXPECT_FALSE(this->book.best_bid()) << "market orders never rest";
  EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, MarketOrderIntoEmptySideIsCancelled) {
  this->submit(buy(1, 100, 5));
  EXPECT_EQ(this->submit(market(2, Side::Buy, 5)),
            (Events{E::accept(2), E::cancel(2, 5, CancelReason::Unfilled)}));
}

TYPED_TEST(OrderBookTest, MarketOrderIgnoresPriceAndBand) {
  this->submit(buy(1, 1, 5));
  NewOrder m = market(2, Side::Sell, 5);
  m.price = 999'999;  // far outside the band; irrelevant for a market order
  EXPECT_EQ(this->submit(m), (Events{E::accept(2), E::trade(fill(2, 1, 1, 5, 0, 0, Side::Sell)),
                               E::level(Side::Buy, 1, 0, 0)}));
}

TYPED_TEST(OrderBookTest, MarketOrderWithGtcStillNeverRests) {
  NewOrder m = market(1, Side::Buy, 5);
  m.tif = TimeInForce::GTC;
  EXPECT_EQ(this->submit(m), (Events{E::accept(1), E::cancel(1, 5, CancelReason::Unfilled)}));
  EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, IocTradesWhatItCanAtItsLimitAndCancelsTheRest) {
  this->submit(sell(1, 100, 3));
  this->submit(sell(2, 101, 3));
  EXPECT_EQ(this->submit(ioc(3, Side::Buy, 100, 5)),
            (Events{E::accept(3), E::trade(fill(3, 1, 100, 3, 2, 0, Side::Buy)),
                    E::level(Side::Sell, 100, 0, 0), E::cancel(3, 2, CancelReason::Unfilled)}));
  EXPECT_EQ(this->book.best_ask(), 101);
  EXPECT_FALSE(this->book.best_bid());
}

TYPED_TEST(OrderBookTest, NonCrossingIocIsCancelledWhole) {
  this->submit(sell(1, 105, 3));
  EXPECT_EQ(this->submit(ioc(2, Side::Buy, 104, 5)),
            (Events{E::accept(2), E::cancel(2, 5, CancelReason::Unfilled)}));
}

TEST(OrderBookCapacity, IocNeedsNoCapacityBecauseItCannotRest) {
  BookConfig c;
  c.max_orders = 1;
  EventRecorder rec;
  OrderBook<EventRecorder> book(c, rec);
  book.add(sell(1, 100, 5));
  rec.take();
  book.add(ioc(2, Side::Buy, 100, 2));
  EXPECT_EQ(rec.take(), (Events{E::accept(2), E::trade(Trade{2, 1, 100, 2, 0, 3, Side::Buy}),
                                E::level(Side::Sell, 100, 3, 1)}));
}

// ---------------------------------------------------------------------------------------------
// Fill or kill

TYPED_TEST(OrderBookTest, FokFillsInFullAcrossLevels) {
  this->submit(sell(1, 100, 2));
  this->submit(sell(2, 101, 3));
  this->submit(sell(3, 102, 9));
  EXPECT_EQ(this->submit(fok(4, Side::Buy, 101, 5)),
            (Events{E::accept(4), E::trade(fill(4, 1, 100, 2, 3, 0, Side::Buy)),
                    E::level(Side::Sell, 100, 0, 0), E::trade(fill(4, 2, 101, 3, 0, 0, Side::Buy)),
                    E::level(Side::Sell, 101, 0, 0)}));
  EXPECT_EQ(this->book.best_ask(), 102);
}

TYPED_TEST(OrderBookTest, FokThatCannotFillInFullLeavesTheBookUntouched) {
  this->submit(sell(1, 100, 2));
  this->submit(sell(2, 101, 3));
  this->submit(sell(3, 102, 9));  // beyond the limit, so it must not count
  const auto before = resting(this->book, Side::Sell);
  EXPECT_EQ(this->submit(fok(4, Side::Buy, 101, 6)),
            (Events{E::accept(4), E::cancel(4, 6, CancelReason::FillOrKill)}));
  EXPECT_EQ(resting(this->book, Side::Sell), before);
}

TYPED_TEST(OrderBookTest, MarketFokCountsEveryLevel) {
  this->submit(buy(1, 100, 2));
  this->submit(buy(2, 50, 3));
  EXPECT_EQ(this->submit(fok(3, Side::Sell, 0, 6)).back(), E::reject(3, RejectReason::PriceOutOfBand))
      << "a limit FOK still needs a price in the band";
  NewOrder m = market(3, Side::Sell, 6);
  m.tif = TimeInForce::FOK;
  EXPECT_EQ(this->submit(m), (Events{E::accept(3), E::cancel(3, 6, CancelReason::FillOrKill)}));
  m.qty = 5;
  EXPECT_EQ(this->submit(m).size(), 5u);  // accept, two trades, two level updates
  EXPECT_FALSE(this->book.best_bid());
}

// ---------------------------------------------------------------------------------------------
// Post-only

TYPED_TEST(OrderBookTest, PostOnlyRestsWhenItWouldNotTrade) {
  this->submit(sell(1, 101, 5));
  EXPECT_EQ(this->submit(post_only(2, Side::Buy, 100, 5)),
            (Events{E::accept(2), E::level(Side::Buy, 100, 5, 1)}));
  EXPECT_EQ(this->book.find(2)->flags, order_flags::kPostOnly);
}

TYPED_TEST(OrderBookTest, PostOnlyThatWouldTakeLiquidityIsRejected) {
  this->submit(sell(1, 101, 5));
  EXPECT_EQ(this->submit(post_only(2, Side::Buy, 101, 5)),
            (Events{E::reject(2, RejectReason::WouldCross)}));
  EXPECT_EQ(this->submit(post_only(3, Side::Buy, 150, 5)),
            (Events{E::reject(3, RejectReason::WouldCross)}));
  EXPECT_EQ(this->book.find(1)->qty, 5u);
}

TYPED_TEST(OrderBookTest, PostOnlyRequiresARestingOrderType) {
  NewOrder a = post_only(1, Side::Buy, 100, 5);
  a.tif = TimeInForce::IOC;
  EXPECT_EQ(this->submit(a), (Events{E::reject(1, RejectReason::InvalidOrderType)}));
  NewOrder b = market(2, Side::Buy, 5);
  b.post_only = true;
  EXPECT_EQ(this->submit(b), (Events{E::reject(2, RejectReason::InvalidOrderType)}));
}

// ---------------------------------------------------------------------------------------------
// Validation

TYPED_TEST(OrderBookTest, RejectsInvalidRequests) {
  EXPECT_EQ(this->submit(buy(0, 100, 1)), (Events{E::reject(0, RejectReason::InvalidOrderId)}));
  EXPECT_EQ(this->submit(buy(1, 100, 0)), (Events{E::reject(1, RejectReason::InvalidQuantity)}));
  EXPECT_EQ(this->submit(buy(1, 0, 1)), (Events{E::reject(1, RejectReason::PriceOutOfBand)}));
  EXPECT_EQ(this->submit(buy(1, 1001, 1)), (Events{E::reject(1, RejectReason::PriceOutOfBand)}));
  EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, PriceBandIsInclusive) {
  EXPECT_EQ(this->submit(buy(1, 1, 1)).size(), 2u);
  EXPECT_EQ(this->submit(sell(2, 1000, 1)).size(), 2u);
  EXPECT_EQ(this->book.best_bid(), 1);
  EXPECT_EQ(this->book.best_ask(), 1000);
}

TYPED_TEST(OrderBookTest, RejectsDuplicateLiveId) {
  this->submit(buy(7, 100, 1));
  EXPECT_EQ(this->submit(sell(7, 200, 1)), (Events{E::reject(7, RejectReason::DuplicateOrderId)}));
  EXPECT_FALSE(this->book.best_ask());
}

TYPED_TEST(OrderBookTest, IdMayBeReusedOnceTheOrderIsGone) {
  this->submit(sell(7, 100, 1));
  this->submit(buy(8, 100, 1));  // fills 7
  EXPECT_EQ(this->submit(sell(7, 100, 1)), (Events{E::accept(7), E::level(Side::Sell, 100, 1, 1)}));
}

TEST(OrderBookCapacity, RejectsWhenNoCapacityToRest) {
  BookConfig c;
  c.max_orders = 2;
  EventRecorder rec;
  Book book(c, rec);
  book.add(buy(1, 100, 1));
  book.add(buy(2, 100, 1));
  rec.take();
  book.add(buy(3, 100, 1));
  EXPECT_EQ(rec.take(), (Events{E::reject(3, RejectReason::BookFull)}));
  book.cancel(1);
  rec.take();
  book.add(buy(3, 100, 1));
  EXPECT_EQ(rec.take(), (Events{E::accept(3), E::level(Side::Buy, 100, 2, 2)}));
}

TEST(OrderBookConfig, RejectsInvertedBand) {
  BookConfig c;
  c.min_price = 10;
  c.max_price = 9;
  EventRecorder rec;
  EXPECT_THROW({ Book book(c, rec); }, std::invalid_argument);
}

TEST(OrderBookConfig, SupportsNegativePrices) {
  // Spreads and some futures trade at negative prices; the band is just a range of ticks.
  BookConfig c;
  c.min_price = -500;
  c.max_price = 500;
  EventRecorder rec;
  Book book(c, rec);
  book.add(sell(1, -20, 3));
  book.add(buy(2, -10, 5));
  EXPECT_EQ(book.best_bid(), -10);
  EXPECT_FALSE(book.best_ask());
  EXPECT_EQ(book.find(2)->qty, 2u);
}

}  // namespace
