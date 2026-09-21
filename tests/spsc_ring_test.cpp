// M21.1. The fixed-capacity ring both audio callbacks now read and write.
//
// Standard library only, no device, no engines: the ring is the piece the
// real-time callback depends on, so it is tested where it can be run in a
// second rather than only inside a playback session.
#include "audio/spsc_ring.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

std::vector<float> ramp(size_t n, float base) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = base + static_cast<float>(i);
  return v;
}

void test_capacity_is_rounded_up() {
  aii::SpscRing r(1000);
  check(r.capacity() == 1024, "capacity rounds up to a power of two");
  check(r.available() == 0 && r.empty(), "a fresh ring is empty");
  check(r.free_space() == 1024, "a fresh ring is all free");
}

void test_write_then_read_returns_the_same_samples() {
  aii::SpscRing r(16);
  const auto in = ramp(10, 1.0f);
  check(r.write(in.data(), in.size()) == 10, "write takes everything that fits");
  check(r.available() == 10, "available counts what was written");
  std::vector<float> out(10, -1.0f);
  check(r.read(out.data(), out.size()) == 10, "read returns what was there");
  check(out == in, "the samples come back unchanged and in order");
  check(r.available() == 0, "the ring is empty again");
}

void test_it_wraps() {
  aii::SpscRing r(8);  // capacity 8
  std::vector<float> out(8);
  // Six rounds of 5 samples through an 8-slot ring: every round but the first
  // starts part way round and the copy is split in two.
  float base = 0.0f;
  for (int round = 0; round < 6; ++round, base += 100.0f) {
    const auto in = ramp(5, base);
    if (r.write(in.data(), in.size()) != 5) { check(false, "wrapping write fits"); return; }
    if (r.read(out.data(), 5) != 5) { check(false, "wrapping read returns"); return; }
    for (size_t i = 0; i < 5; ++i) {
      if (out[i] != in[i]) { check(false, "wrapping keeps the samples in order"); return; }
    }
  }
  check(true, "a write and read that straddle the end of the buffer are exact");
}

void test_a_full_ring_takes_what_fits_and_the_producer_counts_the_rest() {
  aii::SpscRing r(8);
  const auto in = ramp(20, 1.0f);
  const size_t took = r.write(in.data(), in.size());
  check(took == 8, "a write bigger than the ring takes what fits");
  check(r.dropped() == 0, "and does not itself call the rest lost -- the producer decides");
  r.note_dropped(in.size() - took);  // what a capture callback does
  check(r.dropped() == 12, "a producer that gives up says so, and it is counted");
  check(r.write(in.data(), 4) == 0, "a full ring takes nothing more");
  std::vector<float> out(8);
  r.read(out.data(), 8);
  check(out[0] == 1.0f && out[7] == 8.0f, "what it kept is the front of the chunk");
}

void test_read_of_an_empty_ring_is_zero() {
  aii::SpscRing r(8);
  std::vector<float> out(4, 7.0f);
  check(r.read(out.data(), 4) == 0, "an empty ring reads nothing");
  check(out[0] == 7.0f, "and does not touch the destination");
}

void test_discard_skips_what_was_queued_and_keeps_what_follows() {
  aii::SpscRing r(64);
  const auto old_audio = ramp(20, 1.0f);
  r.write(old_audio.data(), old_audio.size());
  r.request_discard();
  check(r.available() == 0, "a discard empties the ring at once");
  const auto fresh = ramp(5, 900.0f);
  r.write(fresh.data(), fresh.size());
  check(r.available() == 5, "audio pushed after the discard survives it");
  std::vector<float> out(5);
  check(r.read(out.data(), 5) == 5, "and is readable");
  check(out == fresh, "and is the new audio, not the discarded audio");
}

void test_discard_frees_space() {
  aii::SpscRing r(8);
  const auto in = ramp(8, 1.0f);
  r.write(in.data(), in.size());
  check(r.free_space() == 0, "the ring is full");
  r.request_discard();
  check(r.free_space() == 8, "a discard releases the space without the reader running");
  check(r.write(in.data(), 8) == 8, "so the producer can fill it again");
}

void test_discard_mark_moves_once_per_discard() {
  aii::SpscRing r(16);
  const auto in = ramp(4, 1.0f);
  const uint64_t before = r.discard_mark();
  r.write(in.data(), in.size());
  check(r.discard_mark() == before, "writing does not move the discard mark");
  r.request_discard();
  check(r.discard_mark() != before, "a discard does");
}

// The contract that matters at 24 kHz: one producer, one consumer, no lock,
// nothing lost and nothing reordered. Every sample is its own index, so a gap
// or a swap is caught exactly where it happens.
void test_concurrent_producer_and_consumer_lose_nothing() {
  aii::SpscRing r(1024);
  constexpr size_t kTotal = 400000;
  std::atomic<bool> writer_done{false};
  bool ordered = true;
  size_t got = 0;

  std::thread producer([&] {
    size_t sent = 0;
    while (sent < kTotal) {
      float block[97];
      const size_t n = std::min<size_t>(97, kTotal - sent);
      for (size_t i = 0; i < n; ++i) block[i] = static_cast<float>(sent + i);
      size_t at = 0;
      while (at < n) at += r.write(block + at, n - at);  // wait for room
      sent += n;
    }
    writer_done.store(true);
  });

  float block[61];
  while (got < kTotal) {
    const size_t n = r.read(block, 61);
    for (size_t i = 0; i < n; ++i) {
      if (block[i] != static_cast<float>(got + i)) ordered = false;
    }
    got += n;
    if (n == 0 && writer_done.load() && r.available() == 0 && got >= kTotal) break;
  }
  producer.join();
  check(got == kTotal, "every sample crossed the ring");
  check(ordered, "in order, with none duplicated or skipped");
  check(r.dropped() == 0, "and none dropped, because the producer waited for room");
}

}  // namespace

int main() {
  std::printf("spsc_ring_test\n");
  test_capacity_is_rounded_up();
  test_write_then_read_returns_the_same_samples();
  test_it_wraps();
  test_a_full_ring_takes_what_fits_and_the_producer_counts_the_rest();
  test_read_of_an_empty_ring_is_zero();
  test_discard_skips_what_was_queued_and_keeps_what_follows();
  test_discard_frees_space();
  test_discard_mark_moves_once_per_discard();
  test_concurrent_producer_and_consumer_lose_nothing();
  std::printf(failures ? "\n%d failed\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
