#pragma once
// Speaker output: a fixed-capacity ring of mono float samples drained by
// miniaudio. Nothing is ever written to disk; samples are consumed by the
// device callback and gone.
//
// M21.1 (review finding 7) replaced the vector-behind-a-mutex this used to be.
// `push` erased and inserted on a multi-second vector while holding the same
// mutex the device callback took, so a long VOICEVOX chunk could stall the
// callback across a reallocation. The ring below never allocates after start()
// and the callback takes no lock at all. See src/audio/spsc_ring.h for the
// threading contract.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "audio/spsc_ring.h"
#include "miniaudio.h"

namespace aii {

class AudioOut {
 public:
  AudioOut() = default;
  ~AudioOut();
  AudioOut(const AudioOut&) = delete;
  AudioOut& operator=(const AudioOut&) = delete;

  // The Windows default playback device unless `id` names one (M18.4,
  // `audio/device_pick.h`).
  bool start(int sample_rate, const ma_device_id* id = nullptr);
  void stop();

  // Called from the speech thread. The ring holds kBufferSeconds of audio; a
  // reply long enough to fill it makes this wait for the device to drain
  // rather than drop the tail, because dropping it would cut the assistant off
  // mid-sentence with nothing said about it. It gives up if the device is not
  // running, and abandons the rest of the chunk if a clear() lands while it
  // waits -- which is what a barge-in wants anyway.
  void push(const float* samples, size_t n);
  void clear();                       // drop everything queued (barge-in)
  double pending_seconds() const;     // audio queued but not yet played
  bool idle() const { return pending_seconds() <= 0.0; }
  // RMS of the block the device last asked for, 0..1. It is how loud the app
  // is *actually* speaking right now rather than how much is queued, which is
  // what the avatar's bounce has to land on (M2.4). Written from the audio
  // callback and read from the frame loop, so it is an atomic rather than
  // another thing behind a lock -- a frame that reads it one block stale is of
  // no consequence, and blocking the device callback would be.
  float level() const { return level_.load(std::memory_order_relaxed); }
  int sample_rate() const { return rate_; }
  std::string device_name() const { return name_; }

  // M21.1 instrumentation. Cumulative since start().
  // underruns: callbacks that could not be filled completely.
  // pad_frames: frames of silence written because the ring was short.
  // dropped: samples push() gave up on (device stopped, or a barge-in).
  // max_callback_us: the longest single device callback seen.
  uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
  uint64_t pad_frames() const { return pad_frames_.load(std::memory_order_relaxed); }
  uint64_t dropped() const { return ring_.dropped(); }
  uint64_t max_callback_us() const { return max_callback_us_.load(std::memory_order_relaxed); }
  uint64_t callbacks() const { return callbacks_.load(std::memory_order_relaxed); }
  uint64_t total_callback_us() const { return total_callback_us_.load(std::memory_order_relaxed); }

  // Seconds of audio the ring holds. Generous on purpose: synthesis runs
  // several times faster than playback, so a long reply queues ahead.
  static constexpr int kBufferSeconds = 45;

 private:
  static void callback(ma_device* dev, void* out, const void* in, ma_uint32 frames);

  ma_device device_{};
  bool started_ = false;
  int rate_ = 0;
  std::string name_;
  SpscRing ring_;
  std::atomic<float> level_{0.0f};
  std::atomic<uint64_t> underruns_{0};
  std::atomic<uint64_t> pad_frames_{0};
  std::atomic<uint64_t> max_callback_us_{0};
  std::atomic<uint64_t> callbacks_{0};
  std::atomic<uint64_t> total_callback_us_{0};
  bool had_audio_ = false;  // callback thread only
};

}  // namespace aii
