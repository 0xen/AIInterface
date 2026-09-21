#pragma once
// Microphone capture: the device callback writes samples into a fixed-capacity
// ring; the owner drains it with drain(). Nothing is written to disk.
//
// M21.1 (review finding 7). This used to be a std::vector guarded by a mutex
// the capture callback took while inserting into it, so the callback could be
// caught behind a reallocation -- and the vector grew without limit if nobody
// drained, which is what happens whenever the mic is left open and the reader
// is busy. The ring below is bounded and the callback takes no lock.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/spsc_ring.h"
#include "miniaudio.h"

namespace aii {

class MicIn {
 public:
  MicIn() = default;
  ~MicIn();
  MicIn(const MicIn&) = delete;
  MicIn& operator=(const MicIn&) = delete;

  bool open(int sample_rate);   // opens the default capture device (stopped)
  bool start();                 // begin delivering samples
  void stop();                  // stop delivering samples (device stays open)
  void close();

  // Move everything captured since the last drain into `out` (appends).
  void drain(std::vector<float>& out);
  void discard();               // throw away anything queued
  float peak_and_reset();       // peak absolute sample since last call (level meter)
  std::string device_name() const { return name_; }
  int sample_rate() const { return rate_; }

  // Samples the callback could not fit because nobody drained in time,
  // cumulative since open(). Non-zero means speech was lost, which the old
  // unbounded vector hid by growing instead.
  uint64_t dropped() const { return ring_.dropped(); }

  // Seconds the ring holds. Longer than any utterance this app waits for: the
  // listen timeout is tens of seconds and the reader drains every frame.
  static constexpr int kBufferSeconds = 60;

 private:
  static void callback(ma_device* dev, void* out, const void* in, ma_uint32 frames);

  ma_device device_{};
  bool open_ = false;
  bool running_ = false;
  int rate_ = 0;
  std::string name_;
  SpscRing ring_;
  std::atomic<float> peak_{0.f};
};

}  // namespace aii
