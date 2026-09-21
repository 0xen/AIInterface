#include "audio/resample.h"

#include "miniaudio.h"

namespace aii {

bool resample_mono(const std::vector<float>& in, int in_rate, int out_rate,
                   std::vector<float>& out) {
  out.clear();
  if (in_rate <= 0 || out_rate <= 0) return false;
  if (in.empty()) return true;
  if (in_rate == out_rate) {
    out = in;
    return true;
  }

  ma_resampler_config cfg = ma_resampler_config_init(ma_format_f32, 1, (ma_uint32)in_rate,
                                                     (ma_uint32)out_rate, ma_resample_algorithm_linear);
  ma_resampler rs;
  if (ma_resampler_init(&cfg, nullptr, &rs) != MA_SUCCESS) return false;

  ma_uint64 in_frames = in.size();
  ma_uint64 out_frames = 0;
  if (ma_resampler_get_expected_output_frame_count(&rs, in_frames, &out_frames) != MA_SUCCESS) {
    ma_resampler_uninit(&rs, nullptr);
    return false;
  }
  out.resize((size_t)out_frames);
  ma_uint64 consumed = in_frames;
  ma_uint64 produced = out_frames;
  const ma_result r = ma_resampler_process_pcm_frames(&rs, in.data(), &consumed, out.data(), &produced);
  ma_resampler_uninit(&rs, nullptr);
  if (r != MA_SUCCESS) {
    out.clear();
    return false;
  }
  out.resize((size_t)produced);
  return true;
}

}  // namespace aii
