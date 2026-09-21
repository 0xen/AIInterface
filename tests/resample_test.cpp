// M21.2 (review finding 31). A chunk that arrives at a rate the output device
// was not opened at used to be warned about and played anyway -- at the wrong
// speed and pitch. It is resampled now, and this is the proof, because no
// engine in the app can produce the case: Kokoro and VOICEVOX are both 24 kHz.
//
// The signal is a sine. Getting the length right only shows the arithmetic;
// what matters is that the *tone* survives, so the check that counts is the
// zero-crossing rate of the output, which is the frequency the listener hears.
#include "audio/resample.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

std::vector<float> sine(double hz, double seconds, int rate) {
  std::vector<float> v(size_t(seconds * rate));
  for (size_t i = 0; i < v.size(); ++i)
    v[i] = 0.5f * float(std::sin(2.0 * 3.14159265358979 * hz * double(i) / rate));
  return v;
}

// Upward zero crossings per second: the pitch, measured rather than assumed.
double measured_hz(const std::vector<float>& v, int rate) {
  int crossings = 0;
  for (size_t i = 1; i < v.size(); ++i)
    if (v[i - 1] < 0.0f && v[i] >= 0.0f) ++crossings;
  const double seconds = double(v.size()) / rate;
  return seconds > 0 ? crossings / seconds : 0.0;
}

void test_22050_to_24000_keeps_the_pitch() {
  const int in_rate = 22050, out_rate = 24000;
  const auto in = sine(440.0, 1.0, in_rate);
  std::vector<float> out;
  check(aii::resample_mono(in, in_rate, out_rate, out), "22050 -> 24000 converts");
  const double ratio = double(out.size()) / in.size();
  const double want = double(out_rate) / in_rate;
  check(std::fabs(ratio - want) < 0.01, "the output is as much longer as the rates say");
  const double hz = measured_hz(out, out_rate);
  std::printf("       in %.1f Hz at %d, out %.1f Hz at %d, %zu -> %zu samples\n",
              measured_hz(in, in_rate), in_rate, hz, out_rate, in.size(), out.size());
  check(std::fabs(hz - 440.0) < 5.0, "and it is still a 440 Hz tone, not a 479 Hz one");
}

// What the old code did, stated as a number: pushing a 22050 Hz chunk into a
// 24000 Hz device plays it 8.8% fast, which is 440 Hz heard as 479 Hz.
void test_the_bug_this_replaces_is_audible() {
  const double played_as = 440.0 * 24000.0 / 22050.0;
  check(played_as > 478.0 && played_as < 480.0,
        "unconverted, the same chunk would have sounded at ~479 Hz");
}

void test_24000_to_24000_is_a_copy() {
  const auto in = sine(440.0, 0.1, 24000);
  std::vector<float> out;
  check(aii::resample_mono(in, 24000, 24000, out), "equal rates convert");
  check(out == in, "and change nothing at all -- today's engines take no detour");
}

void test_downward_conversion() {
  const int in_rate = 48000, out_rate = 24000;
  const auto in = sine(1000.0, 0.5, in_rate);
  std::vector<float> out;
  check(aii::resample_mono(in, in_rate, out_rate, out), "48000 -> 24000 converts");
  check(out.size() > in.size() / 2 - 50 && out.size() < in.size() / 2 + 50,
        "halving the rate halves the samples");
  check(std::fabs(measured_hz(out, out_rate) - 1000.0) < 10.0, "the 1 kHz tone survives");
}

void test_bad_input() {
  std::vector<float> out{1.0f, 2.0f};
  check(!aii::resample_mono(sine(440.0, 0.1, 24000), 0, 24000, out), "a zero input rate is refused");
  check(out.empty(), "and leaves nothing behind to play");
  check(!aii::resample_mono(sine(440.0, 0.1, 24000), 24000, -1, out), "a bad output rate is refused");
  std::vector<float> empty_out;
  check(aii::resample_mono({}, 22050, 24000, empty_out), "an empty chunk is not an error");
  check(empty_out.empty(), "and produces nothing");
}

}  // namespace

int main() {
  std::printf("resample_test\n");
  test_22050_to_24000_keeps_the_pitch();
  test_the_bug_this_replaces_is_audible();
  test_24000_to_24000_is_a_copy();
  test_downward_conversion();
  test_bad_input();
  std::printf(failures ? "\n%d failed\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
