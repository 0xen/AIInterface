#include "audio/mic_in.h"

#include <cmath>

namespace aii {

MicIn::~MicIn() { close(); }

bool MicIn::open(int sample_rate) {
  if (open_) return true;
  ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
  cfg.capture.format = ma_format_f32;
  cfg.capture.channels = 1;
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
  std::lock_guard<std::mutex> lock(mutex_);
  out.insert(out.end(), queue_.begin(), queue_.end());
  queue_.clear();
}

void MicIn::discard() {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.clear();
}

float MicIn::peak_and_reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  float p = peak_;
  peak_ = 0.f;
  return p;
}

void MicIn::callback(ma_device* dev, void*, const void* in, ma_uint32 frames) {
  auto* self = static_cast<MicIn*>(dev->pUserData);
  const float* src = static_cast<const float*>(in);
  std::lock_guard<std::mutex> lock(self->mutex_);
  for (ma_uint32 i = 0; i < frames; ++i) {
    float a = std::fabs(src[i]);
    if (a > self->peak_) self->peak_ = a;
  }
  self->queue_.insert(self->queue_.end(), src, src + frames);
}

}  // namespace aii
