#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace lob {

/// Size used to keep independently written fields on separate cache lines. Apple silicon uses
/// 128-byte lines, and x86 prefetches adjacent 64-byte lines in pairs, so 128 is right for both.
inline constexpr std::size_t kCacheLine = 128;

/// Spin-wait hint: lets a sibling hyperthread run and stops a polling core from hammering the
/// line it is waiting on quite so hard.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("isb" ::: "memory");  // a short, predictable delay; `yield` is a no-op on most cores
#endif
}

/// Bounded single-producer, single-consumer ring buffer.
///
/// - Lock-free and wait-free: push and pop are a handful of loads and stores plus one release
///   store, with no read-modify-write instructions.
/// - Head and tail live on separate cache lines, and each side keeps a private copy of the other
///   side's index. The shared index is re-read only when the cached copy says the queue is full
///   (producer) or empty (consumer), so in steady state each operation touches no line the other
///   thread is writing.
/// - Indices grow without wrapping and are masked on access, which distinguishes full from empty
///   without sacrificing a slot.
/// - The producer can stage several elements and make them visible with one release store
///   (write() then publish()). Every publish hands the tail's cache line to a polling consumer,
///   so publishing once per burst instead of once per element is the difference between paying
///   one cross-core transfer per burst and one per element.
template <class T>
class SpscQueue {
  static_assert(std::is_trivially_copyable_v<T>, "elements are copied with plain stores");

 public:
  explicit SpscQueue(std::size_t capacity)
      : capacity_(std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity)),
        mask_(capacity_ - 1),
        slots_(std::make_unique<T[]>(capacity_ + 2 * kPad)) {}

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  /// Producer only. Stages an element without making it visible. Returns false if full.
  bool try_write(const T& value) noexcept {
    if (write_ - head_cache_ == capacity_) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (write_ - head_cache_ == capacity_) return false;
    }
    slot(write_) = value;
    ++write_;
    return true;
  }

  /// Producer only. Stages an element, publishing what is staged and spinning while full.
  void write(const T& value) noexcept {
    while (!try_write(value)) {
      publish();
      cpu_relax();
    }
  }

  /// Producer only. Makes every staged element visible to the consumer.
  void publish() noexcept { tail_.store(write_, std::memory_order_release); }

  /// Producer only. Stages and publishes one element. Returns false if the queue is full.
  bool try_push(const T& value) noexcept {
    if (!try_write(value)) return false;
    publish();
    return true;
  }

  /// Producer only. Spins until there is room.
  void push(const T& value) noexcept {
    write(value);
    publish();
  }

  /// Consumer only. Returns false if the queue is empty.
  bool try_pop(T& out) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head == tail_cache_) return false;
    }
    out = slot(head);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  /// Consumer only. Calls f(const T&) for up to `max` available elements and releases them with a
  /// single index update. Returns how many were consumed.
  template <class F>
  std::size_t consume(F&& f, std::size_t max) noexcept(noexcept(f(std::declval<const T&>()))) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head == tail_cache_) return 0;
    }
    const std::size_t available = tail_cache_ - head;
    const std::size_t n = available < max ? available : max;
    for (std::size_t i = 0; i < n; ++i) f(slot(head + i));
    head_.store(head + n, std::memory_order_release);
    return n;
  }

  /// Approximate when called concurrently; exact when both sides are quiescent.
  [[nodiscard]] std::size_t size() const noexcept {
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

 private:
  /// Unused slots before and after the ring so neighbouring heap objects never share its lines.
  static constexpr std::size_t kPad = (kCacheLine + sizeof(T) - 1) / sizeof(T);

  T& slot(std::size_t i) noexcept { return slots_[kPad + (i & mask_)]; }

  const std::size_t capacity_;
  const std::size_t mask_;
  const std::unique_ptr<T[]> slots_;

  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // published by the producer
  alignas(kCacheLine) std::size_t write_ = 0;             // producer's next slot, maybe unpublished
  std::size_t head_cache_ = 0;                            // producer's copy of head_
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // written by the consumer
  alignas(kCacheLine) std::size_t tail_cache_ = 0;        // consumer's copy of tail_
  char tail_padding_[kCacheLine - sizeof(std::size_t)] = {};
};

}  // namespace lob
