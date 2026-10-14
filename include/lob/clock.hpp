#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace lob::clock {

/// Reads the CPU's invariant timestamp counter: TSC on x86-64, the generic timer (CNTVCT_EL0) on
/// AArch64, steady_clock elsewhere. The read is fenced so it cannot drift across the code being
/// timed. Convert to nanoseconds with ticks_per_ns().
///
/// Resolution is platform dependent and matters when reading the results. A TSC ticks at the
/// nominal core clock (well under a nanosecond). Apple silicon's generic timer runs at 24 MHz, so
/// every sample there is a multiple of 41.67 ns: per-operation means stay unbiased, but individual
/// percentiles are quantized to that grid.
[[nodiscard]] inline std::uint64_t now() noexcept {
#if defined(__aarch64__)
  std::uint64_t v;
  asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
  return v;
#elif defined(__x86_64__) || defined(_M_X64)
  _mm_lfence();
  const std::uint64_t v = __rdtsc();
  _mm_lfence();
  return v;
#else
  return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

namespace detail {

inline double calibrate() {
#if defined(__aarch64__)
  std::uint64_t freq;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  return static_cast<double>(freq) / 1e9;
#elif defined(__x86_64__) || defined(_M_X64)
  // Fit the TSC against steady_clock over a short window; take the median of a few trials.
  std::vector<double> rates;
  for (int i = 0; i < 5; ++i) {
    const auto w0 = std::chrono::steady_clock::now();
    const std::uint64_t t0 = now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::uint64_t t1 = now();
    const auto w1 = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration<double, std::nano>(w1 - w0).count();
    rates.push_back(static_cast<double>(t1 - t0) / ns);
  }
  std::sort(rates.begin(), rates.end());
  return rates[rates.size() / 2];
#else
  using period = std::chrono::steady_clock::period;
  return static_cast<double>(period::den) / (static_cast<double>(period::num) * 1e9);
#endif
}

}  // namespace detail

/// Counter ticks per nanosecond, measured once.
[[nodiscard]] inline double ticks_per_ns() {
  static const double rate = detail::calibrate();
  return rate;
}

[[nodiscard]] inline double to_ns(std::uint64_t ticks) {
  return static_cast<double>(ticks) / ticks_per_ns();
}

/// Length of one counter tick in nanoseconds: the finest difference two reads can show.
[[nodiscard]] inline double resolution_ns() { return 1.0 / ticks_per_ns(); }

/// Average cost of one now() call, in nanoseconds.
[[nodiscard]] inline double overhead_ns() {
  constexpr int kReads = 1 << 16;
  const std::uint64_t t0 = now();
  for (int i = 0; i < kReads; ++i) (void)now();
  const std::uint64_t t1 = now();
  return to_ns(t1 - t0) / kReads;
}

}  // namespace lob::clock
