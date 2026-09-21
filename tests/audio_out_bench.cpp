// M21.1 measurement harness (review finding 7).
//
// Runs the real playback device twice: once through a copy of the
// mutex-and-vector AudioOut this milestone replaced, once through the ring
// that replaced it. Both are given the same work -- a long chunk, the length
// of a long VOICEVOX sentence, pushed while the device is running -- and both
// count the same things in the device callback:
//
//   max callback   the longest single data callback. Under the old design this
//                  includes waiting on the mutex the pushing thread holds
//                  across a std::vector reallocation and copy; under the new
//                  one there is no lock to wait on.
//   underruns      callbacks that could not be filled while audio was in flight.
//   pad frames     frames of silence written into those callbacks.
//
// Not a unit test and deliberately not in the acceptance list: it needs a real
// output device and it takes twenty seconds. Run it by hand:
//     build-tests\bin\Release\audio_out_bench.exe
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/audio_out.h"
#include "miniaudio.h"

namespace {

constexpr int kRate = 24000;          // both engines' rate
constexpr double kChunkSeconds = 6.0;  // a long VOICEVOX sentence
constexpr int kChunks = 5;
constexpr int kGapMs = 1500;           // synthesis runs ahead of playback
constexpr int kTailMs = 2000;

struct Counters {
  std::atomic<uint64_t> callbacks{0};
  std::atomic<uint64_t> underruns{0};
  std::atomic<uint64_t> pad_frames{0};
  std::atomic<uint64_t> max_callback_us{0};
  std::atomic<uint64_t> total_callback_us{0};

  void observe(uint64_t us) {
    callbacks.fetch_add(1, std::memory_order_relaxed);
    total_callback_us.fetch_add(us, std::memory_order_relaxed);
    uint64_t worst = max_callback_us.load(std::memory_order_relaxed);
    while (us > worst && !max_callback_us.compare_exchange_weak(worst, us,
                                                               std::memory_order_relaxed,
                                                               std::memory_order_relaxed)) {
    }
  }
};

// ---------------------------------------------------------------------------
// The implementation as it stood before M21.1, copied here verbatim apart from
// the counters, so the two numbers come from the same machine, the same device
// and the same chunk rather than from two different runs of the app.
class LegacyAudioOut {
 public:
  ~LegacyAudioOut() { stop(); }

  bool start(int sample_rate) {
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 1;
    cfg.sampleRate = sample_rate;
    cfg.dataCallback = &LegacyAudioOut::callback;
    cfg.pUserData = this;
    if (ma_device_init(nullptr, &cfg, &device_) != MA_SUCCESS) return false;
    if (ma_device_start(&device_) != MA_SUCCESS) { ma_device_uninit(&device_); return false; }
    rate_ = sample_rate;
    started_ = true;
    return true;
  }
  void stop() {
    if (!started_) return;
    ma_device_uninit(&device_);
    started_ = false;
  }
  void push(const float* samples, size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (read_pos_ > 0 && read_pos_ * 2 > buffer_.size()) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(read_pos_));
      read_pos_ = 0;
    }
    buffer_.insert(buffer_.end(), samples, samples + n);
  }
  double pending_seconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rate_ <= 0) return 0.0;
    return double(buffer_.size() - read_pos_) / rate_;
  }
  Counters counters;

 private:
  static void callback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
    auto* self = static_cast<LegacyAudioOut*>(dev->pUserData);
    const auto t0 = std::chrono::steady_clock::now();
    float* dst = static_cast<float*>(out);
    std::lock_guard<std::mutex> lock(self->mutex_);
    size_t avail = self->buffer_.size() - self->read_pos_;
    size_t n = std::min<size_t>(avail, frames);
    if (n > 0) std::memcpy(dst, self->buffer_.data() + self->read_pos_, n * sizeof(float));
    if (n < frames) {
      std::memset(dst + n, 0, (frames - n) * sizeof(float));
      if (n > 0 || self->had_audio_) {
        self->counters.underruns.fetch_add(1, std::memory_order_relaxed);
        self->counters.pad_frames.fetch_add(frames - n, std::memory_order_relaxed);
      }
    }
    self->had_audio_ = n > 0;
    self->read_pos_ += n;
    double sum = 0.0;
    for (ma_uint32 i = 0; i < frames; ++i) sum += double(dst[i]) * double(dst[i]);
    self->level_ = frames ? float(std::sqrt(sum / double(frames))) : 0.0f;
    self->counters.observe(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - t0)
                                        .count()));
  }

  ma_device device_{};
  bool started_ = false;
  int rate_ = 0;
  mutable std::mutex mutex_;
  std::vector<float> buffer_;
  size_t read_pos_ = 0;
  float level_ = 0.f;
  bool had_audio_ = false;
};

std::vector<float> long_chunk() {
  const size_t n = size_t(kRate * kChunkSeconds);
  std::vector<float> v(n);
  // Quiet on purpose: this runs for twenty seconds and nobody asked for a tone
  // at full scale. Loud enough that level() is non-zero, which is how the run
  // shows the device really played it.
  for (size_t i = 0; i < n; ++i)
    v[i] = 0.01f * float(std::sin(2.0 * 3.14159265358979 * 220.0 * double(i) / kRate));
  return v;
}

void report(const char* which, const Counters& c, uint64_t max_push_us) {
  const uint64_t n = c.callbacks.load();
  std::printf("  %-22s callbacks %6llu  underruns %5llu  pad frames %7llu  "
              "max callback %6llu us  mean %4llu us  max push %7llu us\n",
              which, (unsigned long long)n, (unsigned long long)c.underruns.load(),
              (unsigned long long)c.pad_frames.load(),
              (unsigned long long)c.max_callback_us.load(),
              (unsigned long long)(n ? c.total_callback_us.load() / n : 0),
              (unsigned long long)max_push_us);
}

template <typename Out>
uint64_t drive(Out& out, const std::vector<float>& chunk) {
  uint64_t max_push_us = 0;
  for (int i = 0; i < kChunks; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    out.push(chunk.data(), chunk.size());
    const auto us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count());
    max_push_us = std::max(max_push_us, us);
    std::this_thread::sleep_for(std::chrono::milliseconds(kGapMs));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(kTailMs));
  return max_push_us;
}

}  // namespace

int main() {
  const auto chunk = long_chunk();
  std::printf("audio_out_bench: %d chunks of %.1f s at %d Hz, one every %d ms\n\n",
              kChunks, kChunkSeconds, kRate, kGapMs);

  {
    LegacyAudioOut before;
    if (!before.start(kRate)) { std::printf("no playback device\n"); return 1; }
    const uint64_t max_push = drive(before, chunk);
    std::printf("  (queued %.1f s at the end)\n", before.pending_seconds());
    report("before (mutex+vector)", before.counters, max_push);
    before.stop();
  }

  {
    aii::AudioOut after;
    if (!after.start(kRate)) { std::printf("no playback device\n"); return 1; }
    Counters c;  // filled from the class's own atomics after the run
    const uint64_t max_push = drive(after, chunk);
    std::printf("  (queued %.1f s at the end, level %.4f)\n", after.pending_seconds(),
                after.level());
    c.callbacks.store(after.callbacks());
    c.total_callback_us.store(after.total_callback_us());
    c.underruns.store(after.underruns());
    c.pad_frames.store(after.pad_frames());
    c.max_callback_us.store(after.max_callback_us());
    report("after  (spsc ring)", c, max_push);
    std::printf("  dropped samples: %llu\n", (unsigned long long)after.dropped());
    after.stop();
  }
  return 0;
}
