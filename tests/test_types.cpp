#include <gtest/gtest.h>

#include "lob/types.hpp"

namespace {

using namespace lob;

TEST(Types, OppositeSide) {
  static_assert(opposite(Side::Buy) == Side::Sell);
  static_assert(opposite(Side::Sell) == Side::Buy);
  EXPECT_EQ(opposite(opposite(Side::Buy)), Side::Buy);
}

TEST(Types, NamesAreStable) {
  // These strings appear in tool output and CSV files, so they are part of the interface.
  EXPECT_EQ(to_string(Side::Buy), "BUY");
  EXPECT_EQ(to_string(Side::Sell), "SELL");
  EXPECT_EQ(to_string(TimeInForce::FOK), "FOK");
  EXPECT_EQ(to_string(StpMode::CancelBoth), "cancel-both");
  EXPECT_EQ(to_string(RejectReason::WouldCross), "would-cross");
  EXPECT_EQ(to_string(CancelReason::SelfTrade), "self-trade");
}

}  // namespace
