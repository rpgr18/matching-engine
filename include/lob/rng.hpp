#pragma once

#include <cmath>
#include <cstdint>

namespace lob {

namespace detail {
__extension__ typedef unsigned __int128 uint128;
}  // namespace detail

/// xoshiro256++ (Blackman and Vigna), seeded through SplitMix64.
///
/// The standard library's engines are fully specified but its distributions are not, so the same
/// seed can produce different workloads on libstdc++ and libc++. The integer and uniform draws
/// here are defined bit for bit; geometric() depends only on std::log, which mainstream libms
/// round identically for all practical purposes.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept {
    for (auto& word : s_) word = splitmix64(seed);
  }

  /// Starts from an explicit state, for checking against the reference implementation.
  Rng(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
      : s_{a, b, c, d} {}

  std::uint64_t operator()() noexcept {
    const std::uint64_t result = rotl(s_[0] + s_[3], 23) + s_[0];
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  /// Uniform integer in [0, n), unbiased (Lemire's multiply-and-reject).
  std::uint64_t below(std::uint64_t n) noexcept {
    if (n == 0) return 0;
    detail::uint128 m = static_cast<detail::uint128>((*this)()) * n;
    auto low = static_cast<std::uint64_t>(m);
    if (low < n) {
      const std::uint64_t threshold = (0 - n) % n;
      while (low < threshold) {
        m = static_cast<detail::uint128>((*this)()) * n;
        low = static_cast<std::uint64_t>(m);
      }
    }
    return static_cast<std::uint64_t>(m >> 64);
  }

  /// Uniform double in [0, 1) with 53 bits of randomness.
  double uniform() noexcept { return static_cast<double>((*this)() >> 11) * 0x1.0p-53; }

  bool chance(double p) noexcept { return uniform() < p; }

  /// Geometric on {0, 1, 2, ...} with the given mean.
  std::uint64_t geometric(double mean) noexcept {
    if (mean <= 0) return 0;
    const double u = 1.0 - uniform();  // (0, 1]
    return static_cast<std::uint64_t>(std::floor(std::log(u) / std::log(mean / (mean + 1.0))));
  }

 private:
  static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept {
    return (x << k) | (x >> (64 - k));
  }

  static std::uint64_t splitmix64(std::uint64_t& x) noexcept {
    std::uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  std::uint64_t s_[4];
};

}  // namespace lob
