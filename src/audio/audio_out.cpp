#include "audio/audio_out.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

namespace aii {
namespace {
// How long push() is willing to wait for the device to make room before it
// gives up on the rest of a chunk. Only reachable if the device has stopped
// delivering callbacks without saying so; a normal drain frees a block every
// few milliseconds.
constexpr int kPushWaitMs = 20000;
}  // namespace

AudioOut::~AudioOut() { stop(); }

bool AudioOut::start(int sample_rate) {
  if (started_) return true;
  // Allocated before the device exists, so nothing is ever allocated with a
  // callback running.
  ring_.reset(static_cast<size_t>(sample_rate) * kBufferSeconds);
  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = 1;
  cfg.sampleRate = sample_rate;  // miniaudio resamples to the device's native rate
  cfg.dataCallback = &AudioOut::callback;
  cfg.pUserData = this;
  if (ma_device_init(nullptr, &cfg, &device_) != MA_SUCCESS) return false;
  if (ma_device_start(&device_) != MA_SUCCESS) {
    ma_device_uninit(&device_);
    return false;
  }
  rate_ = sample_rate;
  name_ = device_.playback.name;
  started_ = true;
  return true;
}

void AudioOut::stop() {
  if (!started_) return;
  started_ = false;  // released first so a waiting push() stops waiting
  ma_device_uninit(&device_);
}

void AudioOut::push(const float* samples, size_t n) {
  if (!started_) {
    ring_.note_dropped(n);
    return;
  }
  const uint64_t mark = ring_.discard_mark();
  size_t at = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPushWaitMs);
  while (at < n) {
    at += ring_.write(samples + at, n - at);
    if (at >= n) break;
    // Full. Wait for the device rather than drop the tail of a sentence.
    const bool gave_up = !started_ || ring_.discard_mark() != mark ||
                         std::chrono::steady_clock::now() > deadline;
    if (gave_up) {  // stopped, barge-in (the rest is stale), or the device died
      ring_.note_dropped(n - at);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void AudioOut::clear() { ring_.request_discard(); }

double AudioOut::pending_seconds() const {
  if (rate_ <= 0) return 0.0;
  return static_cast<double>(ring_.available()) / rate_;
}

void AudioOut::callback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
  auto* self = static_cast<AudioOut*>(dev->pUserData);
  const auto t0 = std::chrono::steady_clock::now();
  float* dst = static_cast<float*>(out);
  // No lock and no allocation: one atomic load, a memcpy, one atomic store.
  const size_t n = self->ring_.read(dst, frames);
  if (n < frames) {
    std::memset(dst + n, 0, (frames - n) * sizeof(float));
    // A block with nothing in it at all, following a block that was also
    // empty, is not an underrun: it is the app not speaking. What counts is a
    // block that ran short while audio was in flight -- the audible glitch.
    if (n > 0 || self->had_audio_) {
      self->underruns_.fetch_add(1, std::memory_order_relaxed);
      self->pad_frames_.fetch_add(frames - n, std::memory_order_relaxed);
    }
  }
  self->had_audio_ = n > 0;
  // Over the whole block, silence included: a half-filled block is genuinely
  // half as loud, and the tail of a reply has to fall to zero rather than hold
  // the last full block's level.
  double sum = 0.0;
  for (ma_uint32 i = 0; i < frames; ++i) sum += double(dst[i]) * double(dst[i]);
  self->level_.store(frames ? static_cast<float>(std::sqrt(sum / double(frames))) : 0.0f,
                     std::memory_order_relaxed);
  const auto us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
          .count());
  self->callbacks_.fetch_add(1, std::memory_order_relaxed);
  self->total_callback_us_.fetch_add(us, std::memory_order_relaxed);
  uint64_t worst = self->max_callback_us_.load(std::memory_order_relaxed);
  while (us > worst && !self->max_callback_us_.compare_exchange_weak(
                           worst, us, std::memory_order_relaxed, std::memory_order_relaxed)) {
  }
}

}  // namespace aii
