#include <gtest/gtest.h>

#include <stdexcept>
#include <tuple>
#include <vector>

#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "lob/replay.hpp"
#include "lob/workload.hpp"

namespace {

using namespace lob;

std::vector<Event> sequential(const Workload& w) {
  EventRecorder rec;
  OrderBook<EventRecorder> book(book_config_for(w.spec), rec);
  for (const Command& c : w.commands) apply(book, c);
  return rec.events;
}

std::vector<Event> pipelined(const Workload& w, const ReplayOptions& opt) {
  EventRecorder rec;
  OrderBook<EventRecorder> book(book_config_for(w.spec), rec);
  replay(book, w.commands.data(), w.commands.data() + w.commands.size(), opt);
  return rec.events;
}

TEST(Replay, PrefetchingNeverChangesResults) {
  for (WorkloadSpec spec : standard_workloads(40'000)) {
    if (spec.target_depth > 100'000) {
      spec.target_depth = 20'000;
      spec.prefill = 20'000;
    }
    const Workload w = generate(spec);
    const auto expected = sequential(w);

    ReplayOptions always;
    always.min_orders = 0;
    EXPECT_EQ(pipelined(w, always), expected) << spec.name;

    ReplayOptions odd = always;  // distances that do not divide the chunk or the ring evenly
    odd.slot_distance = 37;
    odd.order_distance = 23;
    odd.neighbour_distance = 1;
    EXPECT_EQ(pipelined(w, odd), expected) << spec.name;

    EXPECT_EQ(pipelined(w, ReplayOptions{}), expected) << spec.name;
  }
}

TEST(Replay, RejectsLookaheadThatWouldReadOutOfOrderOrOutOfBounds) {
  EventRecorder rec;
  OrderBook<EventRecorder> book(BookConfig{}, rec);
  for (auto [slot, order, neighbour] :
       {std::tuple{16, 4, 10}, std::tuple{16, 10, -1}, std::tuple{16, 10, 0},
        std::tuple{4, 10, 2}, std::tuple{100, 90, 2}}) {
    ReplayOptions o;
    o.slot_distance = slot;
    o.order_distance = order;
    o.neighbour_distance = neighbour;
    EXPECT_THROW(replay(book, nullptr, nullptr, o), std::invalid_argument)
        << slot << ',' << order << ',' << neighbour;
  }
  ReplayOptions edge;
  edge.slot_distance = edge.order_distance = edge.neighbour_distance = 1;
  EXPECT_NO_THROW(replay(book, nullptr, nullptr, edge));
}

TEST(Replay, HandlesEmptyAndTinyBatches) {
  EventRecorder rec;
  OrderBook<EventRecorder> book(BookConfig{}, rec);
  ReplayOptions always;
  always.min_orders = 0;
  replay(book, nullptr, nullptr, always);
  EXPECT_TRUE(rec.events.empty());

  NewOrder o;
  o.id = 1;
  o.price = 10;
  o.qty = 1;
  const Command one[] = {Command::add(o)};
  replay(book, one, one + 1, always);
  EXPECT_EQ(book.order_count(), 1u);
}

}  // namespace
