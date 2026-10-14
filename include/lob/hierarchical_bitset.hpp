#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace lob {

/// A bitset with a summary tree on top: bit b of word w in layer k+1 is set iff word 64*w+b of
/// layer k is non-zero. Finding the next or previous set bit from any position touches at most
/// two words per layer, so it costs O(log64 n): three layers cover 262,144 bits, four cover
/// 16.7 million.
///
/// The price ladder uses this to find the next populated price level after the best level
/// empties, without scanning the empty slots in between.
class HierarchicalBitset {
 public:
  static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

  explicit HierarchicalBitset(std::size_t bits) : size_(bits) {
    std::size_t n = bits;
    do {
      n = n > 64 ? (n + 63) / 64 : 1;
      layers_.emplace_back(n, 0);
    } while (n > 1);
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  [[nodiscard]] bool test(std::size_t i) const noexcept {
    assert(i < size_);
    return (layers_[0][i >> 6] >> (i & 63)) & 1u;
  }

  void set(std::size_t i) noexcept {
    assert(i < size_);
    for (auto& layer : layers_) {
      std::uint64_t& word = layer[i >> 6];
      const bool was_empty = word == 0;
      word |= bit(i & 63);
      if (!was_empty) return;  // the summary bits above are already set
      i >>= 6;
    }
  }

  void reset(std::size_t i) noexcept {
    assert(i < size_);
    for (auto& layer : layers_) {
      std::uint64_t& word = layer[i >> 6];
      word &= ~bit(i & 63);
      if (word != 0) return;  // the word is still non-empty, so its summary bit stays
      i >>= 6;
    }
  }

  /// Smallest set index >= i, or npos.
  [[nodiscard]] std::size_t find_next(std::size_t i) const noexcept {
    if (i >= size_) return npos;
    std::size_t k = 0;
    // Climb until some word has a set bit at or after the position.
    for (;;) {
      const auto& layer = layers_[k];
      const std::size_t w = i >> 6;
      const std::uint64_t bits = layer[w] & (~std::uint64_t{0} << (i & 63));
      if (bits != 0) {
        i = (w << 6) | static_cast<std::size_t>(std::countr_zero(bits));
        break;
      }
      if (++k == layers_.size()) return npos;
      i = w + 1;  // first word after w, as a bit index in the layer above
      if ((i >> 6) >= layers_[k].size()) return npos;
    }
    // Descend, taking the lowest set bit at each layer.
    while (k > 0) {
      --k;
      i = (i << 6) | static_cast<std::size_t>(std::countr_zero(layers_[k][i]));
    }
    return i;
  }

  /// Largest set index <= i, or npos. Any i >= size() searches from the last bit.
  [[nodiscard]] std::size_t find_prev(std::size_t i) const noexcept {
    if (size_ == 0) return npos;
    if (i >= size_) i = size_ - 1;
    std::size_t k = 0;
    for (;;) {
      const auto& layer = layers_[k];
      const std::size_t w = i >> 6;
      const std::size_t b = i & 63;
      const std::uint64_t mask = b == 63 ? ~std::uint64_t{0} : bit(b + 1) - 1;
      const std::uint64_t bits = layer[w] & mask;
      if (bits != 0) {
        i = (w << 6) | (63 - static_cast<std::size_t>(std::countl_zero(bits)));
        break;
      }
      if (w == 0 || ++k == layers_.size()) return npos;
      i = w - 1;
    }
    while (k > 0) {
      --k;
      i = (i << 6) | (63 - static_cast<std::size_t>(std::countl_zero(layers_[k][i])));
    }
    return i;
  }

  [[nodiscard]] bool none() const noexcept { return layers_.back()[0] == 0; }

 private:
  static constexpr std::uint64_t bit(std::size_t b) noexcept { return std::uint64_t{1} << b; }

  std::size_t size_;
  std::vector<std::vector<std::uint64_t>> layers_;  // layers_[0] holds the bits themselves
};

}  // namespace lob
