#include "audio/mic_in.h"

#include <cmath>

namespace aii {

MicIn::~MicIn() { close(); }

bool MicIn::open(int sample_rate, const ma_device_id* id) {
  if (open_) return true;
  // Allocated before the device exists, so nothing is allocated under a
  // running callback.
  ring_.reset(static_cast<size_t>(sample_rate) * kBufferSeconds);
  ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
  cfg.capture.format = ma_format_f32;
  cfg.capture.channels = 1;
  cfg.capture.pDeviceID = id;  // null is the default device
  cfg.sampleRate = sample_rate;
  cfg.dataCallback = &MicIn::callback;
  cfg.pUserData = this;
  if (ma_device_init(nullptr, &cfg, &device_) != MA_SUCCESS) return false;
  rate_ = sample_rate;
  name_ = device_.capture.name;
  open_ = true;
  return true;
}

bool MicIn::start() {
  if (!open_) return false;
  if (running_) return true;
  discard();
  if (ma_device_start(&device_) != MA_SUCCESS) return false;
  running_ = true;
  return true;
}

void MicIn::stop() {
  if (!running_) return;
  ma_device_stop(&device_);
  running_ = false;
}

void MicIn::close() {
  if (!open_) return;
  stop();
  ma_device_uninit(&device_);
  open_ = false;
  discard();
}

void MicIn::drain(std::vector<float>& out) {
  const size_t want = ring_.available();
  if (want == 0) return;
  const size_t at = out.size();
  out.resize(at + want);  // the reader's thread, not the callback's
  const size_t got = ring_.read(out.data() + at, want);
  out.resize(at + got);
}

void MicIn::discard() { ring_.request_discard(); }

float MicIn::peak_and_reset() { return peak_.exchange(0.f, std::memory_order_acq_rel); }

void MicIn::callback(ma_device* dev, void*, const void* in, ma_uint32 frames) {
  auto* self = static_cast<MicIn*>(dev->pUserData);
  const float* src = static_cast<const float*>(in);
  float block_peak = 0.f;
  for (ma_uint32 i = 0; i < frames; ++i) {
    const float a = std::fabs(src[i]);
    if (a > block_peak) block_peak = a;
  }
  // The meter keeps the loudest sample since the last read, so this raises the
  // published value and never lowers it; peak_and_reset() is what clears it.
  float seen = self->peak_.load(std::memory_order_relaxed);
  while (block_peak > seen && !self->peak_.compare_exchange_weak(seen, block_peak,
                                                                std::memory_order_relaxed,
                                                                std::memory_order_relaxed)) {
  }
  // Anything that does not fit is counted rather than silently growing the
  // queue; dropped() is how a caller finds out.
  const size_t taken = self->ring_.write(src, frames);
  if (taken < frames) self->ring_.note_dropped(frames - taken);
}

}  // namespace aii
