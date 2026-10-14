#pragma once

#include <cstddef>
#include <cstdint>

#include "lob/command.hpp"
#include "lob/order.hpp"

namespace lob {

/// Tuning for replay(). Distances are in requests ahead of the one being applied; they were
/// chosen by sweeping them on the deep-book benchmark, where gains plateau from about 16 out.
struct ReplayOptions {
  std::ptrdiff_t slot_distance = 16;
  std::ptrdiff_t order_distance = 10;
  std::ptrdiff_t neighbour_distance = 4;
  /// Prefetch only while the book holds at least this many resting orders. Below it the book is
  /// cache resident and the hints are pure overhead: on an M2 Pro (16 MB L2 per cluster) they
  /// cost a third of throughput at 100k orders and add half again at 1M, crossing over between
  /// 100k and 200k.
  std::size_t min_orders = 1u << 17;
};

/// Applies a batch of requests in order, with software-pipelined prefetching once the book is
/// big enough to miss cache.
///
/// Results are identical to applying each request in turn; only timing changes. While request i
/// is applied, the book is asked to start the dependent loads requests further ahead will make:
/// the index slot, then the order that slot points to, then that order's queue neighbours and
/// price level. By the time a request runs, most of its cache misses are already in flight.
template <class Book>
void replay(Book& book, const Command* first, const Command* last, const ReplayOptions& opt = {}) {
  constexpr std::ptrdiff_t kChunk = 1024;
  constexpr std::ptrdiff_t kRing = 64;  // must exceed order_distance - neighbour_distance
  OrderIdx ring[kRing];
  const std::ptrdiff_t n = last - first;
  const std::ptrdiff_t ds = opt.slot_distance;
  const std::ptrdiff_t dor = opt.order_distance;
  const std::ptrdiff_t dn = opt.neighbour_distance;

  for (std::ptrdiff_t base = 0; base < n; base += kChunk) {
    const std::ptrdiff_t stop = base + kChunk < n ? base + kChunk : n;
    if (book.order_count() < opt.min_orders) {
      for (std::ptrdiff_t i = base; i < stop; ++i) apply(book, first[i]);
      continue;
    }
    // Prime the pipeline for the first requests of the chunk.
    for (std::ptrdiff_t j = base; j < base + dor && j < n; ++j)
      ring[j % kRing] = book.prefetch_order(first[j]);
    for (std::ptrdiff_t i = base; i < stop; ++i) {
      if (i + ds < n) book.prefetch_slot(first[i + ds]);
      if (i + dor < n) ring[(i + dor) % kRing] = book.prefetch_order(first[i + dor]);
      if (i + dn < n) book.prefetch_neighbours(first[i + dn], ring[(i + dn) % kRing]);
      apply(book, first[i]);
    }
  }
}

}  // namespace lob
