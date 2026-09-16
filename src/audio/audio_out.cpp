#include "audio/audio_out.h"

#include <algorithm>
#include <cstring>

namespace aii {

AudioOut::~AudioOut() { stop(); }

bool AudioOut::start(int sample_rate) {
  if (started_) return true;
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
  ma_device_uninit(&device_);
  started_ = false;
}

void AudioOut::push(const float* samples, size_t n) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Compact once the consumed prefix is large.
  if (read_pos_ > 0 && read_pos_ * 2 > buffer_.size()) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(read_pos_));
    read_pos_ = 0;
  }
  buffer_.insert(buffer_.end(), samples, samples + n);
}

void AudioOut::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  buffer_.clear();
  read_pos_ = 0;
}

double AudioOut::pending_seconds() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (rate_ <= 0) return 0.0;
  return static_cast<double>(buffer_.size() - read_pos_) / rate_;
}

void AudioOut::callback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
  auto* self = static_cast<AudioOut*>(dev->pUserData);
  float* dst = static_cast<float*>(out);
  std::lock_guard<std::mutex> lock(self->mutex_);
  size_t avail = self->buffer_.size() - self->read_pos_;
  size_t n = std::min<size_t>(avail, frames);
  if (n > 0) std::memcpy(dst, self->buffer_.data() + self->read_pos_, n * sizeof(float));
  if (n < frames) std::memset(dst + n, 0, (frames - n) * sizeof(float));
  self->read_pos_ += n;
}

}  // namespace aii
