#pragma once
// Speaker output: a ring buffer of mono float samples drained by miniaudio.
// Nothing is ever written to disk; samples are consumed by the device callback
// and gone.
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include "miniaudio.h"

namespace aii {

class AudioOut {
 public:
  AudioOut() = default;
  ~AudioOut();
  AudioOut(const AudioOut&) = delete;
  AudioOut& operator=(const AudioOut&) = delete;

  bool start(int sample_rate);
  void stop();

  void push(const float* samples, size_t n);
  void clear();                       // drop everything queued (barge-in)
  double pending_seconds() const;     // audio queued but not yet played
  bool idle() const { return pending_seconds() <= 0.0; }
  // RMS of the block the device last asked for, 0..1. It is how loud the app
  // is *actually* speaking right now rather than how much is queued, which is
  // what the avatar's bounce has to land on (M2.4). Written from the audio
  // callback and read from the frame loop, so it is an atomic rather than
  // another thing behind mutex_ — a frame that reads it one block stale is of
  // no consequence, and blocking the device callback would be.
  float level() const { return level_.load(std::memory_order_relaxed); }
  int sample_rate() const { return rate_; }
  std::string device_name() const { return name_; }

 private:
  static void callback(ma_device* dev, void* out, const void* in, ma_uint32 frames);

  ma_device device_{};
  bool started_ = false;
  int rate_ = 0;
  std::string name_;
  mutable std::mutex mutex_;
  std::vector<float> buffer_;
  size_t read_pos_ = 0;
  std::atomic<float> level_{0.0f};
};

}  // namespace aii
