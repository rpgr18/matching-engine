#pragma once

// NASDAQ TotalView-ITCH 5.0: the order-level messages, a zero-copy parser and a writer.
//
// ITCH is NASDAQ's market-by-order feed: every displayed order's add, execution, cancel, delete
// and replace, for every listed instrument, in one sequenced stream. Fields are big-endian and
// fixed width; prices are unsigned 32-bit with four implied decimals. NASDAQ's historical files
// frame each message with a two-byte big-endian length, the same framing SoupBinTCP uses on the
// wire, and that is the framing parse() expects.
//
// Only the messages that change the order book are decoded into structs. Everything else is
// skipped by length, so a full day's file parses without a table of every message type.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "lob/types.hpp"

namespace lob::itch {

// ---- Message types ----------------------------------------------------------------------------

inline constexpr char kSystemEvent = 'S';
inline constexpr char kStockDirectory = 'R';
inline constexpr char kAddOrder = 'A';
inline constexpr char kAddOrderAttributed = 'F';
inline constexpr char kOrderExecuted = 'E';
inline constexpr char kOrderExecutedWithPrice = 'C';
inline constexpr char kOrderCancel = 'X';
inline constexpr char kOrderDelete = 'D';
inline constexpr char kOrderReplace = 'U';
inline constexpr char kTrade = 'P';
inline constexpr char kCrossTrade = 'Q';

/// Body length in bytes of every ITCH 5.0 message type, or 0 for a type the spec does not define.
[[nodiscard]] constexpr std::size_t message_length(char type) noexcept {
  switch (type) {
    case 'S': return 12;
    case 'R': return 39;
    case 'H': return 25;
    case 'Y': return 20;
    case 'L': return 26;
    case 'V': return 35;
    case 'W': return 12;
    case 'K': return 28;
    case 'J': return 35;
    case 'h': return 21;
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    case 'P': return 44;
    case 'Q': return 40;
    case 'B': return 19;
    case 'I': return 50;
    case 'N': return 20;
    case 'O': return 48;
    default: return 0;
  }
}

// ---- Decoded messages -------------------------------------------------------------------------

/// The fields every message starts with.
struct Header {
  char type;
  std::uint16_t locate;     ///< Stock locate code: a small integer naming the instrument.
  std::uint16_t tracking;
  std::uint64_t timestamp;  ///< Nanoseconds since midnight.
};

struct SystemEvent {
  Header h;
  char code;  ///< 'O' start of messages, 'S' start of system hours, 'Q' market open, ...
};

struct StockDirectory {
  Header h;
  char stock[8];  ///< Space padded.
  std::uint32_t round_lot;

  [[nodiscard]] std::string_view symbol() const noexcept {
    std::size_t n = 8;
    while (n > 0 && stock[n - 1] == ' ') --n;
    return {stock, n};
  }
};

/// 'A' and 'F': a new displayed order.
struct AddOrder {
  Header h;
  std::uint64_t ref;  ///< Order reference number, unique for the day across all instruments.
  Side side;
  std::uint32_t shares;
  char stock[8];
  std::uint32_t price;  ///< Four implied decimals.
};

/// 'E' and 'C': a resting order traded. `price` is set only for 'C', which prints at a price
/// other than the order's own.
struct OrderExecuted {
  Header h;
  std::uint64_t ref;
  std::uint32_t shares;
  std::uint64_t match;
  bool with_price;
  bool printable;
  std::uint32_t price;
};

/// 'X': part of a resting order was cancelled.
struct OrderCancel {
  Header h;
  std::uint64_t ref;
  std::uint32_t shares;
};

/// 'D': a resting order was removed in full.
struct OrderDelete {
  Header h;
  std::uint64_t ref;
};

/// 'U': a resting order was replaced by a new one with a new reference, size and price. The new
/// order keeps the side and instrument and goes to the back of its price level.
struct OrderReplace {
  Header h;
  std::uint64_t old_ref;
  std::uint64_t new_ref;
  std::uint32_t shares;
  std::uint32_t price;
};

/// Default handler: ignores everything. Derive from it and hide the methods you care about; the
/// parser calls them by name, so there is no virtual dispatch.
struct NullHandler {
  void on_system_event(const SystemEvent&) {}
  void on_stock_directory(const StockDirectory&) {}
  void on_add(const AddOrder&) {}
  void on_executed(const OrderExecuted&) {}
  void on_cancel(const OrderCancel&) {}
  void on_delete(const OrderDelete&) {}
  void on_replace(const OrderReplace&) {}
  void on_other(char /*type*/, const std::uint8_t* /*body*/, std::size_t /*length*/) {}
};

// ---- Parsing ----------------------------------------------------------------------------------

namespace detail {

inline std::uint16_t be16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline std::uint32_t be32(const std::uint8_t* p) noexcept {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
         std::uint32_t{p[3]};
}
inline std::uint64_t be48(const std::uint8_t* p) noexcept {
  return (std::uint64_t{be16(p)} << 32) | be32(p + 2);
}
inline std::uint64_t be64(const std::uint8_t* p) noexcept {
  return (std::uint64_t{be32(p)} << 32) | be32(p + 4);
}

inline Header header(const std::uint8_t* p) noexcept {
  return Header{static_cast<char>(p[0]), be16(p + 1), be16(p + 3), be48(p + 5)};
}

}  // namespace detail

/// Decodes one message body (without its length prefix) and calls the matching handler method.
/// Returns false, calling nothing, if `length` is shorter than the type requires.
template <class Handler>
bool dispatch(const std::uint8_t* p, std::size_t length, Handler& h) {
  using namespace detail;
  if (length == 0) return false;
  const char type = static_cast<char>(p[0]);
  const std::size_t need = message_length(type);
  if (need != 0 && length < need) return false;
  switch (type) {
    case kAddOrder:
    case kAddOrderAttributed: {
      AddOrder m;
      m.h = header(p);
      m.ref = be64(p + 11);
      m.side = p[19] == 'S' ? Side::Sell : Side::Buy;
      m.shares = be32(p + 20);
      std::memcpy(m.stock, p + 24, 8);
      m.price = be32(p + 32);
      h.on_add(m);
      return true;
    }
    case kOrderExecuted:
    case kOrderExecutedWithPrice: {
      OrderExecuted m;
      m.h = header(p);
      m.ref = be64(p + 11);
      m.shares = be32(p + 19);
      m.match = be64(p + 23);
      m.with_price = type == kOrderExecutedWithPrice;
      m.printable = m.with_price ? p[31] == 'Y' : true;
      m.price = m.with_price ? be32(p + 32) : 0;
      h.on_executed(m);
      return true;
    }
    case kOrderCancel: {
      h.on_cancel(OrderCancel{header(p), be64(p + 11), be32(p + 19)});
      return true;
    }
    case kOrderDelete: {
      h.on_delete(OrderDelete{header(p), be64(p + 11)});
      return true;
    }
    case kOrderReplace: {
      h.on_replace(OrderReplace{header(p), be64(p + 11), be64(p + 19), be32(p + 27), be32(p + 31)});
      return true;
    }
    case kStockDirectory: {
      StockDirectory m;
      m.h = header(p);
      std::memcpy(m.stock, p + 11, 8);
      m.round_lot = be32(p + 21);
      h.on_stock_directory(m);
      return true;
    }
    case kSystemEvent: {
      h.on_system_event(SystemEvent{header(p), static_cast<char>(p[11])});
      return true;
    }
    default:
      h.on_other(type, p, length);
      return true;
  }
}

struct ParseStats {
  std::uint64_t messages = 0;
  std::uint64_t malformed = 0;  ///< Shorter than their type requires.
  std::uint64_t by_type[128] = {};
};

/// Parses length-prefixed messages from a buffer until it runs out or a message is incomplete,
/// and returns the number of bytes consumed. Feeding a file in chunks works: carry the
/// unconsumed tail into the next call.
template <class Handler>
std::size_t parse(const std::uint8_t* data, std::size_t size, Handler& h,
                  ParseStats* stats = nullptr) {
  std::size_t pos = 0;
  while (pos + 2 <= size) {
    const std::size_t len = detail::be16(data + pos);
    if (pos + 2 + len > size) break;
    const std::uint8_t* body = data + pos + 2;
    const bool ok = dispatch(body, len, h);
    if (stats) {
      ++stats->messages;
      stats->malformed += !ok;
      if (len > 0 && body[0] < 128) ++stats->by_type[body[0]];
    }
    pos += 2 + len;
  }
  return pos;
}

// ---- Writing ----------------------------------------------------------------------------------

/// Builds a length-prefixed ITCH 5.0 stream. Used to synthesize feeds for tests and benchmarks;
/// every message it writes is byte-for-byte what the spec defines.
class Writer {
 public:
  void system_event(std::uint64_t ts, char code) {
    begin(kSystemEvent, 0, ts);
    u8(static_cast<std::uint8_t>(code));
  }

  void stock_directory(std::uint16_t locate, std::uint64_t ts, std::string_view symbol,
                       std::uint32_t round_lot = 100) {
    begin(kStockDirectory, locate, ts);
    stock(symbol);
    u8('Q');  // market category: NASDAQ Global Select
    u8('N');  // financial status: normal
    u32(round_lot);
    u8('N');  // round lots only
    u8('C');  // issue classification: common stock
    raw("Z ", 2);
    u8('P');  // authenticity: live
    u8('N');  // short sale threshold
    u8('N');  // IPO flag
    u8('1');  // LULD tier
    u8('N');  // ETP flag
    u32(0);   // ETP leverage
    u8('N');  // inverse
  }

  void add_order(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, Side side,
                 std::uint32_t shares, std::string_view symbol, std::uint32_t price) {
    begin(kAddOrder, locate, ts);
    u64(ref);
    u8(side == Side::Buy ? 'B' : 'S');
    u32(shares);
    stock(symbol);
    u32(price);
  }

  void executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares,
                std::uint64_t match) {
    begin(kOrderExecuted, locate, ts);
    u64(ref);
    u32(shares);
    u64(match);
  }

  void cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
    begin(kOrderCancel, locate, ts);
    u64(ref);
    u32(shares);
  }

  void del(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref) {
    begin(kOrderDelete, locate, ts);
    u64(ref);
  }

  void replace(std::uint16_t locate, std::uint64_t ts, std::uint64_t old_ref,
               std::uint64_t new_ref, std::uint32_t shares, std::uint32_t price) {
    begin(kOrderReplace, locate, ts);
    u64(old_ref);
    u64(new_ref);
    u32(shares);
    u32(price);
  }

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return buf_; }
  [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(buf_); }

 private:
  void begin(char type, std::uint16_t locate, std::uint64_t ts) {
    const std::size_t len = message_length(type);
    u16(static_cast<std::uint16_t>(len));
    u8(static_cast<std::uint8_t>(type));
    u16(locate);
    u16(0);  // tracking number
    u16(static_cast<std::uint16_t>(ts >> 32));
    u32(static_cast<std::uint32_t>(ts));
  }
  void u8(std::uint8_t v) { buf_.push_back(v); }
  void u16(std::uint16_t v) {
    u8(static_cast<std::uint8_t>(v >> 8));
    u8(static_cast<std::uint8_t>(v));
  }
  void u32(std::uint32_t v) {
    u16(static_cast<std::uint16_t>(v >> 16));
    u16(static_cast<std::uint16_t>(v));
  }
  void u64(std::uint64_t v) {
    u32(static_cast<std::uint32_t>(v >> 32));
    u32(static_cast<std::uint32_t>(v));
  }
  void raw(const char* p, std::size_t n) { buf_.insert(buf_.end(), p, p + n); }
  void stock(std::string_view s) {
    char padded[8];
    std::memset(padded, ' ', 8);
    std::memcpy(padded, s.data(), s.size() < 8 ? s.size() : 8);
    raw(padded, 8);
  }

  std::vector<std::uint8_t> buf_;
};

}  // namespace lob::itch
