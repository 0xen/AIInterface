#pragma once
// Microphone capture: the device callback appends samples to an in-memory
// queue; the owner drains it with drain(). Nothing is written to disk.
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

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

 private:
  static void callback(ma_device* dev, void* out, const void* in, ma_uint32 frames);

  ma_device device_{};
  bool open_ = false;
  bool running_ = false;
  int rate_ = 0;
  std::string name_;
  std::mutex mutex_;
  std::vector<float> queue_;
  float peak_ = 0.f;
};

}  // namespace aii
