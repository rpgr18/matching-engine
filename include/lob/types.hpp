#pragma once

#include <cstdint>
#include <string_view>

namespace lob {

/// Order identifier, unique among live orders. Zero is reserved and never valid.
using OrderId = std::uint64_t;
/// Price in integer ticks. Floating point never touches the matching path.
using Price = std::int64_t;
/// Open quantity in lots.
using Qty = std::uint32_t;
/// Participant identifier used for self-trade prevention. Zero means anonymous.
using OwnerId = std::uint32_t;
/// Instrument identifier.
using SymbolId = std::uint16_t;

enum class Side : std::uint8_t { Buy, Sell };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

enum class OrderType : std::uint8_t {
  Limit,   ///< Trades at the limit price or better.
  Market,  ///< Trades at any price. Never rests: any remainder is cancelled, as for IOC.
};

enum class TimeInForce : std::uint8_t {
  GTC,  ///< Good till cancelled: any unfilled remainder rests on the book.
  IOC,  ///< Immediate or cancel: any unfilled remainder is cancelled.
  FOK,  ///< Fill or kill: executes in full immediately or not at all.
};

/// What happens when an incoming order would trade with a resting order from the same owner.
enum class StpMode : std::uint8_t {
  None,             ///< Self trades are allowed.
  CancelResting,    ///< Cancel the resting order and keep matching.
  CancelAggressor,  ///< Cancel the incoming order's remainder.
  CancelBoth,       ///< Cancel the resting order and the incoming order's remainder.
};

enum class RejectReason : std::uint8_t {
  InvalidOrderId,    ///< Order id zero.
  InvalidQuantity,   ///< Zero quantity.
  InvalidOrderType,  ///< Contradictory instructions, e.g. a post-only IOC.
  PriceOutOfBand,    ///< Limit price outside the book's price band.
  DuplicateOrderId,  ///< Another live order already has this id.
  BookFull,          ///< No capacity left to rest the order.
  WouldCross,        ///< A post-only order would have taken liquidity.
  UnknownOrder,      ///< Cancel or modify for an id that is not resting.
  UnknownSymbol,     ///< Engine-level: no book for the request's symbol.
};

enum class CancelReason : std::uint8_t {
  Requested,   ///< Explicit cancel.
  Unfilled,    ///< IOC or market remainder that found no more liquidity.
  FillOrKill,  ///< FOK order that could not fill in full.
  SelfTrade,   ///< Removed by self-trade prevention.
};

[[nodiscard]] constexpr std::string_view to_string(Side s) noexcept {
  return s == Side::Buy ? "BUY" : "SELL";
}

[[nodiscard]] constexpr std::string_view to_string(OrderType t) noexcept {
  return t == OrderType::Limit ? "LIMIT" : "MARKET";
}

[[nodiscard]] constexpr std::string_view to_string(TimeInForce t) noexcept {
  switch (t) {
    case TimeInForce::GTC: return "GTC";
    case TimeInForce::IOC: return "IOC";
    case TimeInForce::FOK: return "FOK";
  }
  return "?";
}

[[nodiscard]] constexpr std::string_view to_string(StpMode m) noexcept {
  switch (m) {
    case StpMode::None: return "none";
    case StpMode::CancelResting: return "cancel-resting";
    case StpMode::CancelAggressor: return "cancel-aggressor";
    case StpMode::CancelBoth: return "cancel-both";
  }
  return "?";
}

[[nodiscard]] constexpr std::string_view to_string(RejectReason r) noexcept {
  switch (r) {
    case RejectReason::InvalidOrderId: return "invalid-order-id";
    case RejectReason::InvalidQuantity: return "invalid-quantity";
    case RejectReason::InvalidOrderType: return "invalid-order-type";
    case RejectReason::PriceOutOfBand: return "price-out-of-band";
    case RejectReason::DuplicateOrderId: return "duplicate-order-id";
    case RejectReason::BookFull: return "book-full";
    case RejectReason::WouldCross: return "would-cross";
    case RejectReason::UnknownOrder: return "unknown-order";
    case RejectReason::UnknownSymbol: return "unknown-symbol";
  }
  return "?";
}

[[nodiscard]] constexpr std::string_view to_string(CancelReason r) noexcept {
  switch (r) {
    case CancelReason::Requested: return "requested";
    case CancelReason::Unfilled: return "unfilled";
    case CancelReason::FillOrKill: return "fill-or-kill";
    case CancelReason::SelfTrade: return "self-trade";
  }
  return "?";
}

}  // namespace lob
