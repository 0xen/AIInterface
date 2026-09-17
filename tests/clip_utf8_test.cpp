// clip_utf8: a byte cap that never cuts a code point in half.
//
// Both byte caps in this codebase -- the bus's kBusStringMax and the
// scheduler's kScheduleStringMax, both 512 -- used to be a bare resize(), which
// splits whatever multi-byte sequence happens to straddle the cap. Japanese is
// three bytes a character here, so a label at the cap ended in a broken glyph.
// The cap stays a byte cap; what changed is that the cut backs off to the last
// complete code point.
//
// The cases are the four ways a cut can land, plus the one that must not move:
//   1. exactly at the cap                  -> byte-identical, nothing dropped
//   2. a 2-byte sequence straddling it     -> the whole sequence goes
//   3. a 3-byte Japanese char straddling   -> the whole character goes
//   4. a 4-byte emoji straddling, at each  -> the whole emoji goes, wherever
//      of its three possible split points     inside it the cap fell
//   5. ASCII well under the cap            -> byte-identical
//
//   clip_utf8_test          prints every case and exits non-zero on failure
#include <cstdio>
#include <string>

#include "core/app_bus.h"
#include "core/schedule.h"
#include "core/text_util.h"

using aii::clip_utf8;

namespace {

int g_failures = 0;

// True if every byte is part of a complete, well-formed UTF-8 sequence. This is
// the property the whole fix exists to guarantee, so it is checked on every
// result rather than only where a test expects trouble.
bool is_valid_utf8(const std::string& s) {
  for (std::size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t len;
    if (c < 0x80) len = 1;
    else if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    else return false;                 // continuation byte or 0xF8+ as a lead
    if (i + len > s.size()) return false;  // truncated sequence at the end
    for (std::size_t k = 1; k < len; ++k)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    i += len;
  }
  return true;
}

void check(const char* name, const std::string& in, std::size_t cap,
           const std::string& want) {
  const std::string got = clip_utf8(in, cap);
  const bool ok = got == want && got.size() <= cap && is_valid_utf8(got);
  if (!ok) ++g_failures;
  std::printf("  %-46s %s  in=%zu cap=%zu out=%zu%s\n", name,
              ok ? "ok  " : "FAIL", in.size(), cap, got.size(),
              is_valid_utf8(got) ? "" : "  <-- INVALID UTF-8");
  if (!ok) {
    std::printf("      wanted %zu bytes [%s]\n", want.size(), want.c_str());
    std::printf("      got    %zu bytes [%s]\n", got.size(), got.c_str());
  }
}

// `n` copies of a string, for building something that reaches a real cap.
std::string rep(const std::string& unit, std::size_t n) {
  std::string out;
  out.reserve(unit.size() * n);
  for (std::size_t i = 0; i < n; ++i) out += unit;
  return out;
}

}  // namespace

int main() {
  const std::size_t kCap = aii::kBusStringMax;  // 512
  std::printf("clip_utf8: cap under test = %zu bytes\n", kCap);
  std::printf("bus cap = %zu, schedule cap = %zu%s\n\n", aii::kBusStringMax,
              aii::kScheduleStringMax,
              aii::kBusStringMax == aii::kScheduleStringMax
                  ? " (agree)"
                  : "  <-- THE TWO CAPS HAVE DIVERGED");

  // --- 1. exactly at the cap -------------------------------------------------
  // 512 ASCII bytes: the cut is a no-op and nothing may be lost.
  const std::string ascii_exact = rep("a", kCap);
  check("ascii exactly at the cap -> unchanged", ascii_exact, kCap, ascii_exact);

  // ...and the same case built out of Japanese, so "exactly at the cap" is
  // tested where a mistake would actually split something: 170 x 3 bytes = 510,
  // then two ASCII bytes to land the cap on a character boundary at 512.
  const std::string ja_exact = rep("\xE3\x81\x82", 170) + "ab";  // あ x170 + "ab"
  check("japanese exactly at the cap -> unchanged", ja_exact, kCap, ja_exact);

  // --- 2. a 2-byte sequence straddling the cap -------------------------------
  // 511 ASCII + "e-acute": byte 511 is the lead, byte 512 the continuation.
  const std::string two_byte = rep("a", kCap - 1) + "\xC3\xA9" + "tail";
  check("2-byte sequence straddles -> whole char dropped", two_byte, kCap,
        rep("a", kCap - 1));

  // --- 3. a 3-byte Japanese character straddling the cap ---------------------
  // This is the real case: 170 x 3 = 510, then one more character occupies
  // 510..512, so the cap falls on its *last* byte.
  const std::string ja_straddle = rep("\xE3\x81\x82", 171) + "\xE3\x81\x84";
  check("3-byte japanese straddles (cap on byte 3)", ja_straddle, kCap,
        rep("\xE3\x81\x82", 170));

  // And with the cap on its middle byte: 511 = 170 chars + 1 filler byte, so
  // the next character spans 511..513 and 512 is its second byte.
  const std::string ja_straddle_mid = rep("\xE3\x81\x82", 170) + "x" + rep("\xE3\x81\x84", 3);
  check("3-byte japanese straddles (cap on byte 2)", ja_straddle_mid, kCap,
        rep("\xE3\x81\x82", 170) + "x");

  // --- 4. a 4-byte emoji straddling the cap ----------------------------------
  // All three split points, because backing off one byte is not the same job as
  // backing off three and an off-by-one here is exactly the bug being fixed.
  const std::string emoji = "\xF0\x9F\x8E\x89";  // U+1F389
  for (std::size_t lead_at = kCap - 3; lead_at <= kCap - 1; ++lead_at) {
    const std::string head = rep("a", lead_at);
    char name[80];
    std::snprintf(name, sizeof(name), "4-byte emoji straddles (cap on byte %zu)",
                  kCap - lead_at + 1);
    check(name, head + emoji + emoji, kCap, head);
  }
  // The boundary next door: an emoji that ends *exactly* at the cap stays.
  check("4-byte emoji ends exactly at the cap -> kept",
        rep("a", kCap - 4) + emoji + emoji, kCap, rep("a", kCap - 4) + emoji);

  // --- 5. ASCII well under the cap -------------------------------------------
  const std::string small = "schedule.create timer=10min label=tea";
  check("short ascii -> byte-identical", small, kCap, small);
  check("empty string -> empty", "", kCap, "");
  // Japanese well under the cap must also be returned untouched, since that is
  // every real message this app carries.
  const std::string ja_small = "\xE3\x81\x8A\xE8\x8C\xB6\xE3\x81\xAE\xE6\x99\x82\xE9\x96\x93";
  check("short japanese -> byte-identical", ja_small, kCap, ja_small);

  std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
              g_failures, g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
