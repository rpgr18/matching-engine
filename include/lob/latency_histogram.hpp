#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace lob {

/// A log-linear histogram in the style of HdrHistogram: values below 256 get exact buckets, and
/// each power of two above that is split into 128 equal buckets, so any recorded value is known
/// to within 1/128 (0.8%) of itself across the whole uint64 range.
///
/// Recording is an increment into a fixed 58 KiB array: no allocation, no sorting, and cheap
/// enough to sit inside a timed loop without dominating what it measures.
class LatencyHistogram {
 public:
  static constexpr int kSubBits = 7;
  static constexpr std::uint64_t kSub = std::uint64_t{1} << kSubBits;
  static constexpr std::size_t kBuckets = kSub + (64 - kSubBits) * kSub;

  LatencyHistogram() : counts_(kBuckets, 0) {}

  void record(std::uint64_t v) noexcept {
    ++counts_[index(v)];
    ++count_;
    sum_ += v;
    min_ = std::min(min_, v);
    max_ = std::max(max_, v);
  }

  void merge(const LatencyHistogram& other) noexcept {
    for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += other.counts_[i];
    count_ += other.count_;
    sum_ += other.sum_;
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
  }

  void clear() noexcept {
    std::fill(counts_.begin(), counts_.end(), 0);
    count_ = 0;
    sum_ = 0;
    min_ = std::numeric_limits<std::uint64_t>::max();
    max_ = 0;
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  [[nodiscard]] std::uint64_t min() const noexcept { return count_ ? min_ : 0; }
  [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
  [[nodiscard]] double mean() const noexcept {
    return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
  }

  /// The value at quantile q in [0, 1]: the upper edge of the bucket holding the ceil(q*n)-th
  /// smallest sample, never more than the true maximum. Reporting the upper edge means a quoted
  /// percentile is never optimistic.
  [[nodiscard]] std::uint64_t quantile(double q) const noexcept {
    if (count_ == 0) return 0;
    const double clamped = std::clamp(q, 0.0, 1.0);
    const auto rank = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(std::ceil(clamped * static_cast<double>(count_))));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < kBuckets; ++i) {
      seen += counts_[i];
      if (seen >= rank) return std::min(upper_edge(i), max_);
    }
    return max_;
  }

  [[nodiscard]] static constexpr std::size_t index(std::uint64_t v) noexcept {
    if (v < kSub) return static_cast<std::size_t>(v);
    const int msb = 63 - std::countl_zero(v);
    const int shift = msb - kSubBits;
    const std::uint64_t sub = (v >> shift) - kSub;
    return static_cast<std::size_t>(kSub + static_cast<std::uint64_t>(shift) * kSub + sub);
  }

  /// The largest value that maps to bucket i.
  [[nodiscard]] static constexpr std::uint64_t upper_edge(std::size_t i) noexcept {
    if (i < kSub) return i;
    const std::uint64_t shift = (i - kSub) / kSub;
    const std::uint64_t sub = (i - kSub) % kSub;
    return ((kSub + sub + 1) << shift) - 1;
  }

 private:
  std::vector<std::uint64_t> counts_;
  std::uint64_t count_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_ = 0;
};

}  // namespace lob
