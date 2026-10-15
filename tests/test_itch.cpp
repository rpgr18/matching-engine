#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "book_test_util.hpp"
#include "lob/itch.hpp"
#include "lob/itch_book.hpp"
#include "lob/order_book.hpp"
#include "lob/workload.hpp"

namespace {

using namespace lob;
using namespace lob::itch;

struct Capture : NullHandler {
  std::vector<AddOrder> adds;
  std::vector<OrderExecuted> execs;
  std::vector<OrderCancel> cancels;
  std::vector<OrderDelete> deletes;
  std::vector<OrderReplace> replaces;
  std::vector<StockDirectory> dirs;
  std::vector<SystemEvent> events;
  std::vector<char> other;

  void on_add(const AddOrder& m) { adds.push_back(m); }
  void on_executed(const OrderExecuted& m) { execs.push_back(m); }
  void on_cancel(const OrderCancel& m) { cancels.push_back(m); }
  void on_delete(const OrderDelete& m) { deletes.push_back(m); }
  void on_replace(const OrderReplace& m) { replaces.push_back(m); }
  void on_stock_directory(const StockDirectory& m) { dirs.push_back(m); }
  void on_system_event(const SystemEvent& m) { events.push_back(m); }
  void on_other(char type, const std::uint8_t*, std::size_t) { other.push_back(type); }
};

TEST(Itch, DecodesAnAddOrderLaidOutByHand) {
  // 'A', locate 0x0102, tracking 0, timestamp 0x000102030405, ref 0x1122334455667788, 'S',
  // 300 shares, "AAPL    ", price 1234500 ($123.45), framed with its two-byte length.
  const std::vector<std::uint8_t> msg = {
      0x00, 0x24, 'A',  0x01, 0x02, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 'S',  0x00, 0x00, 0x01, 0x2C,
      'A',  'A',  'P',  'L',  ' ',  ' ',  ' ',  ' ',  0x00, 0x12, 0xD6, 0x44};
  Capture c;
  ParseStats stats;
  EXPECT_EQ(parse(msg.data(), msg.size(), c, &stats), msg.size());
  ASSERT_EQ(c.adds.size(), 1u);
  const AddOrder& a = c.adds[0];
  EXPECT_EQ(a.h.locate, 0x0102);
  EXPECT_EQ(a.h.timestamp, 0x000102030405u);
  EXPECT_EQ(a.ref, 0x1122334455667788u);
  EXPECT_EQ(a.side, Side::Sell);
  EXPECT_EQ(a.shares, 300u);
  EXPECT_EQ(std::string(a.stock, 8), "AAPL    ");
  EXPECT_EQ(a.price, 1'234'500u);
  EXPECT_EQ(stats.messages, 1u);
  EXPECT_EQ(stats.by_type[static_cast<int>('A')], 1u);
}

TEST(Itch, WriterAndParserAgreeOnEveryOrderMessage) {
  Writer w;
  w.system_event(1, 'O');
  w.stock_directory(7, 2, "MSFT", 100);
  w.add_order(7, 3, 42, Side::Buy, 500, "MSFT", 4'100'000);
  w.executed(7, 4, 42, 200, 9001);
  w.cancel(7, 5, 42, 100);
  w.replace(7, 6, 42, 43, 250, 4'099'900);
  w.del(7, 7, 43);

  Capture c;
  ParseStats stats;
  EXPECT_EQ(parse(w.bytes().data(), w.bytes().size(), c, &stats), w.bytes().size());
  EXPECT_EQ(stats.messages, 7u);
  EXPECT_EQ(stats.malformed, 0u);
  ASSERT_EQ(c.events.size(), 1u);
  EXPECT_EQ(c.events[0].code, 'O');
  ASSERT_EQ(c.dirs.size(), 1u);
  EXPECT_EQ(c.dirs[0].symbol(), "MSFT");
  EXPECT_EQ(c.dirs[0].round_lot, 100u);
  ASSERT_EQ(c.adds.size(), 1u);
  EXPECT_EQ(c.adds[0].ref, 42u);
  EXPECT_EQ(c.adds[0].side, Side::Buy);
  EXPECT_EQ(c.adds[0].price, 4'100'000u);
  ASSERT_EQ(c.execs.size(), 1u);
  EXPECT_EQ(c.execs[0].shares, 200u);
  EXPECT_EQ(c.execs[0].match, 9001u);
  EXPECT_FALSE(c.execs[0].with_price);
  ASSERT_EQ(c.cancels.size(), 1u);
  EXPECT_EQ(c.cancels[0].shares, 100u);
  ASSERT_EQ(c.replaces.size(), 1u);
  EXPECT_EQ(c.replaces[0].old_ref, 42u);
  EXPECT_EQ(c.replaces[0].new_ref, 43u);
  EXPECT_EQ(c.replaces[0].shares, 250u);
  EXPECT_EQ(c.replaces[0].price, 4'099'900u);
  ASSERT_EQ(c.deletes.size(), 1u);
  EXPECT_EQ(c.deletes[0].h.timestamp, 7u);

  // Every message the writer produced is exactly the length the spec defines.
  std::size_t pos = 0;
  const auto& b = w.bytes();
  while (pos < b.size()) {
    const std::size_t len = static_cast<std::size_t>((b[pos] << 8) | b[pos + 1]);
    EXPECT_EQ(len, message_length(static_cast<char>(b[pos + 2])));
    pos += 2 + len;
  }
}

TEST(Itch, SkipsUnknownTypesAndStopsAtAnIncompleteMessage) {
  Writer w;
  w.del(1, 1, 5);
  std::vector<std::uint8_t> bytes = w.bytes();
  // A trade message ('P') the handler does not decode, then a truncated delete.
  std::vector<std::uint8_t> trade(2 + 44, 0);
  trade[1] = 44;
  trade[2] = 'P';
  bytes.insert(bytes.end(), trade.begin(), trade.end());
  const std::size_t complete = bytes.size();
  bytes.insert(bytes.end(), w.bytes().begin(), w.bytes().begin() + 10);

  Capture c;
  EXPECT_EQ(parse(bytes.data(), bytes.size(), c), complete);
  EXPECT_EQ(c.deletes.size(), 1u);
  EXPECT_EQ(c.other, std::vector<char>{'P'});
}

TEST(Itch, CountsMessagesTooShortForTheirType) {
  const std::vector<std::uint8_t> short_add = {0x00, 0x05, 'A', 0, 1, 0, 0};
  Capture c;
  ParseStats stats;
  parse(short_add.data(), short_add.size(), c, &stats);
  EXPECT_EQ(stats.malformed, 1u);
  EXPECT_TRUE(c.adds.empty());
}

TEST(Itch, ChunkedParsingMatchesWholeBufferParsing) {
  WorkloadSpec spec;
  spec.requests = 20'000;
  const SyntheticFeed feed = synthesize({generate(spec)});
  ParseStats whole;
  NullHandler none;
  parse(feed.bytes.data(), feed.bytes.size(), none, &whole);

  ParseStats chunked;
  std::vector<std::uint8_t> carry;
  for (std::size_t off = 0; off < feed.bytes.size(); off += 997) {
    const std::size_t end = std::min(feed.bytes.size(), off + 997);
    carry.insert(carry.end(), feed.bytes.begin() + static_cast<std::ptrdiff_t>(off),
                 feed.bytes.begin() + static_cast<std::ptrdiff_t>(end));
    const std::size_t used = parse(carry.data(), carry.size(), none, &chunked);
    carry.erase(carry.begin(), carry.begin() + static_cast<std::ptrdiff_t>(used));
  }
  EXPECT_TRUE(carry.empty());
  EXPECT_EQ(chunked.messages, whole.messages);
  for (int t = 0; t < 128; ++t) EXPECT_EQ(chunked.by_type[t], whole.by_type[t]);
}

/// The central check: run request streams through matching books, publish what happened as an
/// ITCH feed, rebuild the books from the feed alone, and require every instrument's book to match
/// the original order for order, in priority order.
TEST(Itch, RebuiltBooksMatchTheBooksThatProducedTheFeed) {
  std::vector<Workload> streams;
  for (WorkloadSpec spec : standard_workloads(25'000)) {
    if (spec.target_depth > 100'000) {
      spec.target_depth = 20'000;
      spec.prefill = 20'000;
    }
    streams.push_back(generate(spec));
  }
  const SyntheticFeed feed = synthesize(streams);

  const Capacity cap = scan_capacity(feed.bytes.data(), feed.bytes.size());
  EXPECT_EQ(cap.unknown_refs, 0u);
  ASSERT_EQ(cap.peak_orders.size(), streams.size() + 1);

  NullEngineListener none;
  BookBuilder<> builder(none, cap);
  ParseStats stats;
  EXPECT_EQ(parse(feed.bytes.data(), feed.bytes.size(), builder, &stats), feed.bytes.size());
  EXPECT_EQ(builder.inconsistencies(), 0u);
  EXPECT_GT(stats.by_type[static_cast<int>('E')], 0u);
  EXPECT_GT(stats.by_type[static_cast<int>('X')], 0u);
  EXPECT_GT(stats.by_type[static_cast<int>('U')], 0u);
  EXPECT_GT(stats.by_type[static_cast<int>('D')], 0u);

  std::uint64_t live = 0;
  for (std::size_t k = 0; k < streams.size(); ++k) {
    EventRecorder rec;
    OrderBook<EventRecorder> truth(book_config_for(streams[k].spec), rec);
    for (const Command& c : streams[k].commands) apply(truth, c);

    const auto locate = static_cast<std::uint16_t>(k + 1);
    EXPECT_EQ(builder.name(locate), "SYM" + std::to_string(k));
    const auto& rebuilt = builder.engine().book(locate);
    for (Side side : {Side::Buy, Side::Sell}) {
      std::vector<test::Resting> expect;
      truth.for_each_order(side, [&](const Order& o) {
        expect.push_back({feed.ref_of[k].at(o.id), o.price, o.qty});
      });
      EXPECT_EQ(test::resting(rebuilt, side), expect) << streams[k].spec.name;
      live += expect.size();
    }
    EXPECT_EQ(rebuilt.depth(Side::Buy, 10), truth.depth(Side::Buy, 10));
    EXPECT_EQ(rebuilt.depth(Side::Sell, 10), truth.depth(Side::Sell, 10));
  }
  EXPECT_EQ(cap.live_at_end, live);
}

TEST(Itch, SynthesizedFeedOmitsAddsTheBookRejected) {
  // A stream that reuses a live order's id, including after that order was re-priced: the
  // duplicate adds are rejected by the book and must not reach the feed.
  Workload w;
  w.spec.min_price = 1;
  w.spec.max_price = 1000;
  w.spec.target_depth = 100;
  NewOrder a;
  a.id = 5;
  a.side = Side::Buy;
  a.price = 100;
  a.qty = 10;
  w.commands.push_back(Command::add(a));
  w.commands.push_back(Command::add(a));  // duplicate while live
  w.commands.push_back(Command::modify(5, 99, 10));  // re-priced: new ITCH reference
  w.commands.push_back(Command::add(a));  // duplicate again
  NewOrder s = a;
  s.id = 6;
  s.side = Side::Sell;
  s.price = 99;
  s.qty = 4;
  w.commands.push_back(Command::add(s));  // trades against the re-priced order

  const SyntheticFeed feed = synthesize({w});
  const Capacity cap = scan_capacity(feed.bytes.data(), feed.bytes.size());
  EXPECT_EQ(cap.unknown_refs, 0u);
  NullEngineListener none;
  BookBuilder<> builder(none, cap);
  ParseStats stats;
  parse(feed.bytes.data(), feed.bytes.size(), builder, &stats);
  EXPECT_EQ(builder.inconsistencies(), 0u);
  EXPECT_EQ(stats.by_type[static_cast<int>('A')], 1u);
  const auto& book = builder.engine().book(1);
  ASSERT_EQ(book.order_count(), 1u);
  EXPECT_EQ(test::resting(book, Side::Buy),
            (std::vector<test::Resting>{{feed.ref_of[0].at(5), 99, 6}}));
}

TEST(Itch, PrefetchingParseGivesIdenticalBooks) {
  std::vector<Workload> streams;
  for (WorkloadSpec spec : standard_workloads(20'000)) {
    if (spec.target_depth > 100'000) {
      spec.target_depth = 10'000;
      spec.prefill = 10'000;
    }
    streams.push_back(generate(spec));
  }
  const SyntheticFeed feed = synthesize(streams);
  const Capacity cap = scan_capacity(feed.bytes.data(), feed.bytes.size());
  NullEngineListener none;
  BookBuilder<> plain(none, cap);
  BookBuilder<> ahead(none, cap);
  ParseStats a, b;
  EXPECT_EQ(parse(feed.bytes.data(), feed.bytes.size(), plain, &a), feed.bytes.size());
  EXPECT_EQ(parse_prefetching(feed.bytes.data(), feed.bytes.size(), ahead, &b), feed.bytes.size());
  EXPECT_EQ(a.messages, b.messages);
  EXPECT_EQ(ahead.inconsistencies(), 0u);
  for (std::uint16_t l = 1; l <= streams.size(); ++l) {
    for (Side side : {Side::Buy, Side::Sell})
      EXPECT_EQ(test::resting(ahead.engine().book(l), side),
                test::resting(plain.engine().book(l), side));
  }
  // An incomplete trailing message is left unconsumed, as parse() does.
  const std::size_t cut = feed.bytes.size() - 7;
  NullHandler ignore;
  EXPECT_EQ(parse_prefetching(feed.bytes.data(), cut, ignore), parse(feed.bytes.data(), cut, ignore));
}

TEST(Itch, MirrorsACrossedBookWithoutMatching) {
  Writer w;
  w.stock_directory(1, 1, "HALT");
  w.add_order(1, 2, 10, Side::Sell, 100, "HALT", 1'000'000);
  w.add_order(1, 3, 11, Side::Buy, 100, "HALT", 1'010'000);  // through the offer: an auction book
  const Capacity cap = scan_capacity(w.bytes().data(), w.bytes().size());
  NullEngineListener none;
  BookBuilder<> builder(none, cap);
  parse(w.bytes().data(), w.bytes().size(), builder);
  const auto& book = builder.engine().book(1);
  EXPECT_EQ(book.best_bid(), 1'010'000);
  EXPECT_EQ(book.best_ask(), 1'000'000);
  EXPECT_EQ(book.order_count(), 2u);
  EXPECT_EQ(builder.inconsistencies(), 0u);
}

TEST(Itch, CountsReferencesTheBookHasNeverSeen) {
  Writer w;
  w.add_order(1, 1, 10, Side::Buy, 100, "X", 100);
  w.executed(1, 2, 999, 10, 1);
  w.replace(1, 3, 998, 1000, 10, 100);
  w.del(5, 4, 10);  // locate the scan never saw
  const Capacity cap = scan_capacity(w.bytes().data(), w.bytes().size());
  EXPECT_EQ(cap.unknown_refs, 2u) << "the scan tracks references, not locates";
  NullEngineListener none;
  BookBuilder<> builder(none, cap);
  parse(w.bytes().data(), w.bytes().size(), builder);
  EXPECT_EQ(builder.inconsistencies(), 3u);
}

}  // namespace
