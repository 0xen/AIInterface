#include "audio/device_pick.h"

namespace aii {
namespace {

// One context per call. Enumeration is a startup cost measured in tens of
// milliseconds and it happens twice per launch at most; keeping a context
// alive for the life of the process to save that would be one more global
// with an audio thread behind it.
struct ScopedContext {
  ma_context ctx{};
  bool ok = false;
  ScopedContext() { ok = ma_context_init(nullptr, 0, nullptr, &ctx) == MA_SUCCESS; }
  ~ScopedContext() {
    if (ok) ma_context_uninit(&ctx);
  }
};

}  // namespace

bool find_audio_device(bool playback, const std::string& needle, ma_device_id* id,
                       std::string* name) {
  ScopedContext c;
  if (!c.ok) return false;
  ma_device_info* pb = nullptr;
  ma_uint32 npb = 0;
  ma_device_info* cp = nullptr;
  ma_uint32 ncp = 0;
  if (ma_context_get_devices(&c.ctx, &pb, &npb, &cp, &ncp) != MA_SUCCESS) return false;
  ma_device_info* list = playback ? pb : cp;
  const ma_uint32 n = playback ? npb : ncp;
  for (ma_uint32 i = 0; i < n; ++i) {
    if (std::string(list[i].name).find(needle) == std::string::npos) continue;
    if (id) *id = list[i].id;
    if (name) *name = list[i].name;
    return true;
  }
  return false;
}

std::vector<AudioDeviceName> list_audio_devices(bool playback) {
  std::vector<AudioDeviceName> out;
  ScopedContext c;
  if (!c.ok) return out;
  ma_device_info* pb = nullptr;
  ma_uint32 npb = 0;
  ma_device_info* cp = nullptr;
  ma_uint32 ncp = 0;
  if (ma_context_get_devices(&c.ctx, &pb, &npb, &cp, &ncp) != MA_SUCCESS) return out;
  ma_device_info* list = playback ? pb : cp;
  const ma_uint32 n = playback ? npb : ncp;
  for (ma_uint32 i = 0; i < n; ++i) out.push_back({list[i].name, list[i].isDefault != 0});
  return out;
}

}  // namespace aii
