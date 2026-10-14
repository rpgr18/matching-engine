#include <gtest/gtest.h>

#include "book_test_util.hpp"
#include "lob/command.hpp"
#include "lob/engine.hpp"

namespace {

using namespace lob;
using namespace lob::test;

using E = Event;

BookConfig band(Price lo, Price hi) {
  BookConfig c;
  c.min_price = lo;
  c.max_price = hi;
  c.max_orders = 128;
  return c;
}

TEST(Engine, RoutesBySymbolAndTagsEvents) {
  EngineEventRecorder rec;
  Engine<EngineEventRecorder> engine(rec);
  const SymbolId aapl = engine.add_symbol(band(1, 1000));
  const SymbolId msft = engine.add_symbol(band(1, 5000));
  EXPECT_EQ(aapl, 0);
  EXPECT_EQ(msft, 1);
  EXPECT_EQ(engine.symbols(), 2u);

  engine.apply(Command::add(sell(1, 100, 5), aapl));
  engine.apply(Command::add(sell(1, 4000, 5), msft));  // the same id in another book is fine
  rec.take();

  engine.apply(Command::add(buy(2, 100, 3), aapl));
  EXPECT_EQ(rec.take(), (Events{E::accept(2).on(aapl),
                                E::trade(Trade{2, 1, 100, 3, 0, 2, Side::Buy}).on(aapl),
                                E::level(Side::Sell, 100, 2, 1).on(aapl)}));
  EXPECT_EQ(engine.book(aapl).find(1)->qty, 2u);
  EXPECT_EQ(engine.book(msft).find(1)->qty, 5u);

  engine.apply(Command::cancel(1, msft));
  EXPECT_EQ(rec.take(), (Events{E::cancel(1, 5, CancelReason::Requested).on(msft),
                                E::level(Side::Sell, 4000, 0, 0).on(msft)}));
  EXPECT_EQ(engine.book(aapl).order_count(), 1u);
}

TEST(Engine, BooksKeepTheirOwnConfiguration) {
  EngineEventRecorder rec;
  Engine<EngineEventRecorder> engine(rec);
  const SymbolId narrow = engine.add_symbol(band(1, 10));
  const SymbolId wide = engine.add_symbol(band(1, 100));
  engine.apply(Command::add(buy(1, 50, 1), narrow));
  engine.apply(Command::add(buy(1, 50, 1), wide));
  const auto events = rec.take();
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[0], E::reject(1, RejectReason::PriceOutOfBand).on(narrow));
  EXPECT_EQ(events[1], E::accept(1).on(wide));
}

TEST(Engine, RejectsUnknownSymbols) {
  EngineEventRecorder rec;
  Engine<EngineEventRecorder> engine(rec);
  engine.add_symbol(band(1, 10));
  engine.apply(Command::add(buy(9, 5, 1), 3));
  engine.apply(Command::cancel(9, 1));
  EXPECT_EQ(rec.take(), (Events{E::reject(9, RejectReason::UnknownSymbol).on(3),
                                E::reject(9, RejectReason::UnknownSymbol).on(1)}));
  EXPECT_THROW((void)engine.book(5), std::out_of_range);
}

TEST(Engine, ManySymbolsStayIndependent) {
  EngineEventRecorder rec;
  Engine<EngineEventRecorder> engine(rec);
  for (int i = 0; i < 200; ++i) engine.add_symbol(band(1, 100));
  for (SymbolId s = 0; s < 200; ++s)
    engine.apply(Command::add(buy(1, 1 + s % 100, s + 1u), s));
  for (SymbolId s = 0; s < 200; ++s) {
    EXPECT_EQ(engine.book(s).best_bid(), 1 + s % 100);
    EXPECT_EQ(engine.book(s).find(1)->qty, s + 1u);
  }
}

}  // namespace
