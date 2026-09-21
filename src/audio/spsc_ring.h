#pragma once
// A fixed-capacity single-producer single-consumer ring of floats.
//
// Why it exists (M21.1, review finding 7): both audio callbacks used to take a
// std::mutex that the other side held across a std::vector reallocation. A
// multi-second VOICEVOX chunk pushed while the device was running meant the
// real-time callback waiting on an allocation and a copy of megabytes, which is
// exactly the thing a device callback may not do. This ring never allocates
// after construction and the consumer never blocks: one atomic load, a memcpy,
// one atomic store.
//
// Threading contract
// ------------------
//   write()            producer thread only
//   read(), skipped()  consumer thread only
//   request_discard()  any thread
//   available(), capacity()  any thread (a snapshot, may be stale by the time
//                            it is read, which is all any caller needs)
//
// The counters are monotonic 64-bit positions into an infinite stream; the
// storage index is the position masked by a power-of-two capacity. At 24 kHz a
// 64-bit position takes 24 million years to wrap, so the usual "distinguish
// full from empty" contortions are not needed.
//
// Discard without a lock
// ----------------------
// `clear()` on the old AudioOut ran on neither the producer nor the consumer
// thread (barge-in arrives from the UI), and having a third thread move the
// read cursor races with the callback that also moves it. Instead a discard
// publishes a *skip* position: the write position at the moment it was asked
// for. The consumer reads from max(read, skip), so everything already queued is
// passed over, everything pushed afterwards still plays, and no cursor ever
// moves backwards. The region between the old read position and the skip
// position is unreadable from that moment, so the producer is free to reuse it.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace aii {

class SpscRing {
 public:
  SpscRing() = default;
  explicit SpscRing(size_t min_capacity) { reset(min_capacity); }

  // Allocates (rounding up to a power of two) and empties the ring. Not safe
  // against a running producer or consumer: call it before the device starts.
  void reset(size_t min_capacity) {
    size_t cap = 1;
    while (cap < min_capacity) cap <<= 1;
    buf_.assign(cap, 0.0f);
    mask_ = cap - 1;
    write_.store(0, std::memory_order_relaxed);
    read_.store(0, std::memory_order_relaxed);
    skip_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
  }

  size_t capacity() const { return buf_.size(); }
  bool empty() const { return available() == 0; }

  // Samples a reader would get right now, discards already accounted for.
  size_t available() const {
    const uint64_t w = write_.load(std::memory_order_acquire);
    const uint64_t r = effective_read();
    return static_cast<size_t>(w > r ? w - r : 0);
  }
  size_t free_space() const { return capacity() - available(); }

  // Producer. Writes as much as fits and returns how much that was. Never
  // allocates, never blocks. What to do with a short write is the producer's
  // to decide -- the capture callback has to give up and calls note_dropped(),
  // the speech thread waits and calls it again -- so this does not count a
  // short write as loss on its own.
  size_t write(const float* src, size_t n) {
    if (buf_.empty() || n == 0) return 0;
    const uint64_t w = write_.load(std::memory_order_relaxed);
    const uint64_t r = effective_read();
    const size_t used = static_cast<size_t>(w > r ? w - r : 0);
    const size_t room = capacity() - used;
    const size_t take = std::min(n, room);
    if (take == 0) return 0;
    const size_t at = static_cast<size_t>(w & mask_);
    const size_t first = std::min(take, capacity() - at);
    std::memcpy(buf_.data() + at, src, first * sizeof(float));
    if (take > first) std::memcpy(buf_.data(), src + first, (take - first) * sizeof(float));
    write_.store(w + take, std::memory_order_release);
    return take;
  }

  // Consumer. Copies up to n samples into dst and returns how many. Never
  // allocates, never blocks, takes no lock.
  size_t read(float* dst, size_t n) {
    if (buf_.empty() || n == 0) return 0;
    const uint64_t w = write_.load(std::memory_order_acquire);
    const uint64_t r = effective_read();
    const size_t avail = static_cast<size_t>(w > r ? w - r : 0);
    const size_t take = std::min(n, avail);
    if (take > 0) {
      const size_t at = static_cast<size_t>(r & mask_);
      const size_t first = std::min(take, capacity() - at);
      std::memcpy(dst, buf_.data() + at, first * sizeof(float));
      if (take > first) std::memcpy(dst + first, buf_.data(), (take - first) * sizeof(float));
    }
    read_.store(r + take, std::memory_order_release);
    return take;
  }

  // Any thread. Everything queued at this instant becomes unreadable; anything
  // written afterwards is unaffected.
  void request_discard() {
    const uint64_t w = write_.load(std::memory_order_acquire);
    uint64_t s = skip_.load(std::memory_order_relaxed);
    while (s < w && !skip_.compare_exchange_weak(s, w, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
    }
  }

  // The producer says here how many samples it gave up on. Cumulative, never
  // reset, so a caller that cares about an interval takes the difference.
  void note_dropped(size_t n) { dropped_.fetch_add(n, std::memory_order_relaxed); }
  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

  // A discard sequence: changes exactly when a discard is requested, so a
  // producer part-way through a long chunk can notice a barge-in landed.
  uint64_t discard_mark() const { return skip_.load(std::memory_order_acquire); }

 private:
  uint64_t effective_read() const {
    return std::max(read_.load(std::memory_order_acquire), skip_.load(std::memory_order_acquire));
  }

  std::vector<float> buf_;
  size_t mask_ = 0;
  std::atomic<uint64_t> write_{0};
  std::atomic<uint64_t> read_{0};
  std::atomic<uint64_t> skip_{0};
  std::atomic<uint64_t> dropped_{0};
};

}  // namespace aii
