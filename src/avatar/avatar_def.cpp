#include "avatar_def.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

#include "core/config.h"
#include "json.hpp"

namespace aii {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// The directory is polled rather than watched with a Win32 change
// notification: a handle brings a second thread, an overlapped wait and a
// buffer to drain, all to learn something a stat of a handful of files tells
// us just as well. Half a second is far below the time it takes to alt-tab
// back from an editor, and the cost is a few file_size/last_write_time calls.
constexpr float kPollSeconds = 0.5f;

// The one character the format reserves: a palette that redefined it would
// silently make holes opaque.
constexpr char kTransparent = '.';

// The ceiling on `@frame hold=N`. High enough for the slowest thing anyone
// would draw (240 ticks of a 1 fps clip is four minutes on one drawing) and
// low enough that a fat-fingered `hold=1000000` is caught at the save rather
// than showing up as an avatar that appears to have frozen.
constexpr std::uint32_t kMaxHold = 240;

bool hex_nibble(char c, std::uint32_t& out) {
  if (c >= '0' && c <= '9') out = static_cast<std::uint32_t>(c - '0');
  else if (c >= 'a' && c <= 'f') out = static_cast<std::uint32_t>(c - 'a' + 10);
  else if (c >= 'A' && c <= 'F') out = static_cast<std::uint32_t>(c - 'A' + 10);
  else return false;
  return true;
}

// "#rrggbb", "rrggbb", "#rrggbbaa" or "rrggbbaa". Hex because that is what an
// art tool puts on the clipboard; alpha defaults to opaque because a palette
// entry the user bothered to name is one they want to see.
bool parse_rgba(const std::string& text, std::uint32_t& out) {
  std::string s = text;
  if (!s.empty() && s.front() == '#') s.erase(s.begin());
  if (s.size() != 6 && s.size() != 8) return false;
  std::array<std::uint32_t, 8> n{};
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (!hex_nibble(s[i], n[i])) return false;
  }
  const std::uint32_t r = n[0] * 16 + n[1];
  const std::uint32_t g = n[2] * 16 + n[3];
  const std::uint32_t b = n[4] * 16 + n[5];
  const std::uint32_t a = s.size() == 8 ? n[6] * 16 + n[7] : 255u;
  out = avatar_rgba(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                    static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a));
  return true;
}

// nlohmann's const operator[] asserts on a missing key, and a missing key is
// the ordinary case in a file the user is still writing, so every lookup goes
// through this and every check below is a type test on the result.
const json& member(const json& j, const char* key) {
  static const json kNull;
  const auto it = j.find(key);
  return it == j.end() ? kNull : *it;
}

bool read_file(const fs::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  // A UTF-8 BOM is invisible in the editor but would make the first line of a
  // clip file something other than "@frame" — several Windows editors write
  // one, and being told your comment is not a marker is a baffling error for
  // a file you did not appear to change.
  if (out.rfind("\xEF\xBB\xBF", 0) == 0) out.erase(0, 3);
  return true;
}

// Trailing whitespace and a stray CR are invisible in an editor but would
// otherwise fail the width check on every row of a file saved on Windows and
// read back through a text pipeline that did not translate it.
void trim_trailing(std::string& s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
    s.pop_back();
  }
}

// Palette lookup as a flat 128-entry table: characters are ASCII by
// construction (the loader rejects anything else), and a table keeps the
// per-cell cost of a reload to an array index.
// It outlives the parse now (M1c.5) — see AvatarDefinition::palette.
using Palette = AvatarPalette;

// Reads a `{ "#": "#000000", ... }` object into `pal`, leaving keys it does
// not mention alone. Shared by the base palette and by every theme's override
// (M1c.4), so a theme's colour is held to exactly the same rules as the base
// one and there is one place that knows what a palette entry may be. `what`
// names the object in any error, since "palette" and "theme \"jade\" palette"
// are the two things this can be looking at.
bool apply_palette(const nlohmann::json& obj, const std::string& what, Palette& pal,
                   std::string& error) {
  for (const auto& [key, value] : obj.items()) {
    if (key.size() != 1) {
      error = "avatar.json: " + what + " key \"" + key + "\" must be a single character";
      return false;
    }
    const auto idx = static_cast<unsigned char>(key[0]);
    if (key[0] == kTransparent) {
      error = "avatar.json: '.' is reserved for transparent and cannot be in the " + what;
      return false;
    }
    if (idx >= pal.known.size()) {
      error = "avatar.json: " + what + " key \"" + key + "\" must be ASCII";
      return false;
    }
    if (!value.is_string() || !parse_rgba(value.get<std::string>(), pal.rgba[idx])) {
      error = "avatar.json: " + what + " \"" + key + "\" is not a #rrggbb or #rrggbbaa colour";
      return false;
    }
    pal.known[idx] = true;
  }
  return true;
}

}  // namespace

const AvatarClip* AvatarDefinition::find_clip(const std::string& clip_name) const {
  for (const auto& c : clips) {
    if (c.name == clip_name) return &c;
  }
  return nullptr;
}

bool AvatarDefinition::has_theme(const std::string& theme_name) const {
  for (const auto& t : themes) {
    if (t == theme_name) return true;
  }
  return false;
}

const AvatarAnchor* AvatarDefinition::find_anchor(const std::string& anchor_name) const {
  for (const auto& a : anchors) {
    if (a.name == anchor_name) return &a;
  }
  return nullptr;
}

const AvatarSprite* AvatarDefinition::find_sprite(const std::string& sprite_name) const {
  for (const auto& s : sprites) {
    if (s.name == sprite_name) return &s;
  }
  return nullptr;
}

const AvatarClip* AvatarSprite::find_clip(const std::string& clip_name) const {
  for (const auto& c : clips) {
    if (c.name == clip_name) return &c;
  }
  return nullptr;
}

namespace {

// The attributes on a `@frame` line. Only `hold` exists, but the parse is
// written as a key=value loop rather than a special case so the next
// attribute is an `else if` and not a second syntax.
//
// An unknown key is an error rather than a shrug: a typo'd `hodl=6` that
// quietly did nothing would be indistinguishable from a format that ignores
// hold, and the user would be left retiming art that was never being read.
bool parse_frame_attrs(const std::string& attrs, std::uint32_t& hold, std::string& what) {
  std::istringstream in(attrs);
  std::string token;
  while (in >> token) {
    const std::size_t eq = token.find('=');
    if (eq == std::string::npos) {
      what = "@frame attribute \"" + token + "\" must be key=value";
      return false;
    }
    const std::string key = token.substr(0, eq);
    const std::string value = token.substr(eq + 1);
    if (key != "hold") {
      what = "@frame has no attribute \"" + key + "\"";
      return false;
    }
    // Hand-parsed rather than stoul: stoul accepts "6frames" and a leading
    // minus, and both of those are mistakes the author wants named.
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
      what = "hold=\"" + value + "\" is not a whole number";
      return false;
    }
    unsigned long long n = 0;
    for (const char c : value) {
      n = n * 10 + static_cast<unsigned long long>(c - '0');
      if (n > kMaxHold) break;
    }
    if (n < 1 || n > kMaxHold) {
      what = "hold=" + value + " is outside 1.." + std::to_string(kMaxHold);
      return false;
    }
    hold = static_cast<std::uint32_t>(n);
  }
  return true;
}

// Parses one clip file into frames. Every failure names the file and the line
// the user has to go and look at, because the whole point of the text format
// is that they fix it in the editor they authored it in.
bool parse_clip_file(const fs::path& path, const Palette& pal, std::uint32_t width,
                     std::uint32_t height, bool allow_overlay, std::vector<AvatarFrame>& out,
                     std::string* error) {
  std::string text;
  if (!read_file(path, text)) {
    if (error) *error = path.filename().string() + ": cannot be read";
    return false;
  }
  auto fail = [&](std::size_t line, const std::string& what) {
    if (error) *error = path.filename().string() + ":" + std::to_string(line) + ": " + what;
    return false;
  };

  std::istringstream in(text);
  std::string line;
  std::size_t line_no = 0;
  // Which layer the rows being read belong to, and how many are still owed.
  enum class Target { None, Base, Overlay };
  Target target = Target::None;
  std::uint32_t row = 0;

  while (std::getline(in, line)) {
    ++line_no;
    trim_trailing(line);
    if (target == Target::None) {
      if (line.empty() || line[0] == '#') continue;
      // A marker is its keyword plus optional attributes. Splitting on the
      // first space keeps `@frame` the word the eye scans for and lets the
      // timing ride on the frame it belongs to, rather than in a table
      // somewhere else that has to be kept in step with the art.
      const std::size_t space = line.find(' ');
      const std::string head = line.substr(0, space);
      if (head == "@frame") {
        std::uint32_t hold = 1;
        if (std::string what; space != std::string::npos &&
                              !parse_frame_attrs(line.substr(space + 1), hold, what)) {
          return fail(line_no, what);
        }
        out.push_back(AvatarFrame{});
        out.back().base.assign(std::size_t{width} * height, 0u);
        out.back().overlay.assign(std::size_t{width} * height, 0u);
        // The ink characters behind those two, kept so a palette change is a
        // re-resolve rather than a re-parse (M1c.5). 0 is transparent, which
        // is what an untouched cell already means.
        out.back().base_ink.assign(std::size_t{width} * height, 0u);
        out.back().overlay_ink.assign(std::size_t{width} * height, 0u);
        out.back().hold = hold;
        target = Target::Base;
        row = 0;
        continue;
      }
      if (head == "@overlay") {
        // A sprite is *already* drawn into the overlay layer, so a second
        // overlay inside one would have nowhere different to go. Saying so is
        // kinder than compositing it invisibly onto itself.
        if (!allow_overlay) return fail(line_no, "a sprite has no @overlay; it is the overlay");
        if (out.empty()) return fail(line_no, "@overlay before any @frame");
        // An overlay is the same frame's second layer, so it has no timing of
        // its own; `@overlay hold=2` is someone expecting the layers to run
        // apart, and silently ignoring it would let them believe they do.
        if (space != std::string::npos) {
          return fail(line_no, "@overlay takes no attributes; hold belongs on its @frame");
        }
        target = Target::Overlay;
        row = 0;
        continue;
      }
      return fail(line_no, "expected @frame or @overlay, got \"" + line + "\"");
    }

    // Inside a block every line is art: a comment here would be ambiguous
    // with a row, so comments are only legal between blocks.
    if (line.size() != width) {
      return fail(line_no, "row is " + std::to_string(line.size()) + " characters, expected " +
                               std::to_string(width));
    }
    auto& frame = out.back();
    auto& cells = target == Target::Base ? frame.base : frame.overlay;
    auto& inks = target == Target::Base ? frame.base_ink : frame.overlay_ink;
    for (std::uint32_t x = 0; x < width; ++x) {
      const char c = line[x];
      if (c == kTransparent) continue;
      const auto idx = static_cast<unsigned char>(c);
      if (idx >= pal.known.size() || !pal.known[idx]) {
        return fail(line_no, std::string("character '") + c + "' is not in the palette");
      }
      cells[std::size_t{row} * width + x] = pal.rgba[idx];
      inks[std::size_t{row} * width + x] = idx;
    }
    if (++row == height) target = Target::None;
  }

  if (target != Target::None) {
    return fail(line_no, "block ends after " + std::to_string(row) + " of " +
                             std::to_string(height) + " rows");
  }
  if (out.empty()) {
    if (error) *error = path.filename().string() + ": no frames";
    return false;
  }
  return true;
}

// The clip list, shared by the character and by every sprite: both carry the
// same {name, fps, loop, file} shape, and a sprite whose clips were declared
// differently would be a second format to learn for no gain. `owner` prefixes
// the messages ("sprite \"bubble\" ") and `file_prefix` keeps a sprite's
// default file name from colliding with a character clip of the same name.
bool parse_clips(const json& clips, const fs::path& dir, const Palette& pal, std::uint32_t width,
                 std::uint32_t height, bool allow_overlay, const std::string& owner,
                 const std::string& file_prefix, std::vector<AvatarClip>& out, std::string* error) {
  auto fail = [&](const std::string& what) {
    if (error) *error = "avatar.json: " + owner + what;
    return false;
  };
  if (!clips.is_array() || clips.empty()) return fail("\"clips\" must be a non-empty array");
  for (const auto& entry : clips) {
    if (!entry.is_object() || !member(entry, "name").is_string()) {
      return fail("every clip needs a string \"name\"");
    }
    AvatarClip clip;
    clip.name = member(entry, "name").get<std::string>();
    for (const auto& seen : out) {
      if (seen.name == clip.name) return fail("clip \"" + clip.name + "\" is declared twice");
    }
    const json& fps = member(entry, "fps");
    clip.fps = fps.is_number() ? fps.get<float>() : 8.0f;
    if (!(clip.fps > 0.0f) || clip.fps > 240.0f) {
      return fail("clip \"" + clip.name + "\" has fps outside 0..240");
    }
    const json& loop = member(entry, "loop");
    clip.loop = loop.is_boolean() ? loop.get<bool>() : true;

    const json& file_name = member(entry, "file");
    const std::string file = file_name.is_string() ? file_name.get<std::string>()
                                                   : file_prefix + clip.name + ".txt";
    // The manifest names files inside its own directory; a path that climbs
    // out of it is a typo at best.
    if (file.find('/') != std::string::npos || file.find('\\') != std::string::npos) {
      return fail("clip \"" + clip.name + "\" file must be a plain name");
    }
    if (!parse_clip_file(dir / file, pal, width, height, allow_overlay, clip.frames, error)) {
      return false;
    }
    out.push_back(std::move(clip));
  }
  return true;
}

// How long the composition takes to move from one resting place to the next.
// Long enough to be a movement rather than a jump, short enough that it has
// finished before the accessory that caused it has finished appearing.
constexpr float kSlideSeconds = 0.2f;

// The character's box unioned with a set of placements, in character space —
// so the origin can be negative, which is what a bubble above the head means.
struct Envelope {
  std::int32_t min_x = 0;
  std::int32_t min_y = 0;
  std::uint32_t width = 1;
  std::uint32_t height = 1;
};

// `visible` null means "every declared sprite counts", which is the static
// envelope the scale is taken from; a vector counts only the ones that are up,
// which is the envelope the position is taken from.
Envelope envelope_of(const AvatarDefinition& def, const std::vector<bool>* visible) {
  std::int32_t min_x = 0, min_y = 0;
  auto max_x = static_cast<std::int32_t>(def.width);
  auto max_y = static_cast<std::int32_t>(def.height);
  for (std::size_t i = 0; i < def.sprites.size(); ++i) {
    if (visible && (i >= visible->size() || !(*visible)[i])) continue;
    const AvatarSprite& sprite = def.sprites[i];
    const AvatarAnchor* a = def.find_anchor(sprite.anchor);
    if (!a) continue;  // load-time validation has already rejected this
    const std::int32_t x = static_cast<std::int32_t>(a->x) + sprite.offset_x;
    const std::int32_t y = static_cast<std::int32_t>(a->y) + sprite.offset_y;
    min_x = std::min(min_x, x);
    min_y = std::min(min_y, y);
    max_x = std::max(max_x, x + static_cast<std::int32_t>(sprite.width));
    max_y = std::max(max_y, y + static_cast<std::int32_t>(sprite.height));
  }
  Envelope env;
  env.min_x = min_x;
  env.min_y = min_y;
  env.width = static_cast<std::uint32_t>(max_x - min_x);
  env.height = static_cast<std::uint32_t>(max_y - min_y);
  return env;
}

}  // namespace

bool load_avatar_definition(const fs::path& dir, const std::string& theme,
                            AvatarDefinition& out, std::string* error) {
  auto fail = [&](std::string what) {
    if (error) *error = std::move(what);
    return false;
  };

  const fs::path manifest = dir / "avatar.json";
  std::string text;
  if (!read_file(manifest, text)) return fail("avatar.json not found in " + dir.string());

  // allow_exceptions=false: a half-saved file is the normal case under hot
  // reload, not an exceptional one.
  const json root = json::parse(text, nullptr, false);
  if (root.is_discarded() || !root.is_object()) return fail("avatar.json: not valid JSON");

  AvatarDefinition def;
  const json& name = member(root, "name");
  def.name = name.is_string() ? name.get<std::string>() : dir.filename().string();

  const json& grid = member(root, "grid");
  if (!grid.is_object()) return fail("avatar.json: \"grid\" must be an object with width and height");
  const json& gw = member(grid, "width");
  const json& gh = member(grid, "height");
  const auto w = gw.is_number_unsigned() ? gw.get<std::uint32_t>() : 0u;
  const auto h = gh.is_number_unsigned() ? gh.get<std::uint32_t>() : 0u;
  // The renderer's buffer is sized once for kAvatarMaxGrid, so anything
  // larger has nowhere to go; anything smaller than a cell is not art.
  if (w == 0 || h == 0 || w > kAvatarMaxGrid || h > kAvatarMaxGrid) {
    return fail("avatar.json: grid " + std::to_string(w) + "x" + std::to_string(h) +
                " is outside 1x1.." + std::to_string(kAvatarMaxGrid) + "x" +
                std::to_string(kAvatarMaxGrid));
  }
  def.width = w;
  def.height = h;

  const json& palette = member(root, "palette");
  if (!palette.is_object() || palette.empty()) {
    return fail("avatar.json: \"palette\" must be a non-empty object");
  }
  Palette pal{};
  if (std::string why; !apply_palette(palette, "palette", pal, why)) return fail(why);

  // M1c.5: which of those inks the derived-colour theme drives. Optional, and
  // the defaults are this avatar's own letters, so nothing existing has to
  // change — including the copy already seeded under %APPDATA%, which a
  // rebuild deliberately never overwrites. Named in the file rather than
  // hard-coded so a second avatar drawn with different letters is a data
  // change; validated against the palette, because an ink the art does not
  // have would make the picker drive nothing at all and say nothing about it.
  if (const json& inks = member(root, "custom_inks"); !inks.is_null()) {
    if (!inks.is_object()) return fail("avatar.json: \"custom_inks\" must be an object");
    auto one = [&](const char* key, char& slot) {
      const json& v = member(inks, key);
      if (!v.is_string()) return true;
      const std::string s = v.get<std::string>();
      if (s.size() != 1) return false;
      slot = s[0];
      return true;
    };
    if (!one("body", def.body_ink)) return fail("avatar.json: custom_inks.body must be one character");
    if (!one("feature", def.feature_ink))
      return fail("avatar.json: custom_inks.feature must be one character");
    if (const json& t = member(inks, "translucent"); t.is_string())
      def.translucent_inks = t.get<std::string>();
    for (const char c : std::string{def.body_ink, def.feature_ink} + def.translucent_inks) {
      const auto i = static_cast<unsigned char>(c);
      if (i >= pal.known.size() || !pal.known[i]) {
        return fail(std::string("avatar.json: custom_inks names '") + c +
                    "', which is not in the palette");
      }
    }
  }

  // ---- M1c.4: named themes -------------------------------------------------
  //
  // `themes` is an array, not an object, because the order is the picker's
  // order and JSON objects have none worth relying on (nlohmann sorts keys, so
  // an object would list them alphabetically and the author would lose the say
  // in which palette the user meets first).
  //
  // Each theme's palette is *merged over* the base rather than replacing it,
  // so a theme states only the inks it changes. That is not brevity for its
  // own sake: the base is the one place a new ink character is declared, and a
  // theme that had to be a complete palette would silently omit any ink added
  // to the art after it was written — the new cells would then fail to load
  // with "character 'x' is not in the palette" under that theme alone, which
  // is about the worst shape of bug this format could have. Merging makes a
  // new ink appear in every theme at its base colour until someone tints it.
  def.base_palette = pal;

  const json& themes = member(root, "themes");
  std::vector<const json*> theme_palettes;
  if (!themes.is_null()) {
    if (!themes.is_array() || themes.empty()) {
      return fail("avatar.json: \"themes\" must be a non-empty array");
    }
    for (const auto& entry : themes) {
      if (!entry.is_object() || !member(entry, "name").is_string()) {
        return fail("avatar.json: every theme needs a string \"name\"");
      }
      const auto theme_name = member(entry, "name").get<std::string>();
      if (theme_name.empty()) return fail("avatar.json: a theme name may not be empty");
      if (def.has_theme(theme_name)) {
        return fail("avatar.json: theme \"" + theme_name + "\" is declared twice");
      }
      const json& tp = member(entry, "palette");
      // An absent or empty palette is legal and means "the base as it is" —
      // which is how the definition's original colours keep a name of their
      // own (`mono` here) without being written out twice.
      if (!tp.is_null() && !tp.is_object()) {
        return fail("avatar.json: theme \"" + theme_name + "\" has a \"palette\" that is not an object");
      }
      // Validated now, against a throwaway copy of the base, so a bad colour
      // in a theme nobody has selected is still reported at the save that
      // introduced it rather than months later when someone picks it.
      Palette probe = pal;
      if (std::string why;
          tp.is_object() && !apply_palette(tp, "theme \"" + theme_name + "\" palette", probe, why)) {
        return fail(why);
      }
      def.themes.push_back(theme_name);
      theme_palettes.push_back(tp.is_object() ? &tp : nullptr);
      // The body colour this theme would give, captured off the same probe
      // the validation just built. It is what seeds the colour picker when the
      // user selects a preset (M1c.5), so starting from `ember` and nudging it
      // is one click rather than matching a hex by eye.
      def.theme_body.push_back(probe.rgba[static_cast<unsigned char>(def.body_ink)]);
    }
  } else {
    // No themes block at all: the bare palette is the one theme there is. It
    // gets a name so every caller above can speak in names, and so the picker
    // has something honest to show for a definition that has never heard of
    // this feature.
    def.themes.push_back("default");
    theme_palettes.push_back(nullptr);
    def.theme_body.push_back(pal.rgba[static_cast<unsigned char>(def.body_ink)]);
  }

  const json& dflt_theme = member(root, "default_theme");
  const std::string default_theme =
      dflt_theme.is_string() ? dflt_theme.get<std::string>() : def.themes.front();
  if (!def.has_theme(default_theme)) {
    return fail("avatar.json: default_theme \"" + default_theme + "\" is not a declared theme");
  }
  // A requested theme that is gone is not an error — see the header. It falls
  // back to the default, and `out.theme` is what the caller reads back to find
  // out what it actually got.
  def.theme = def.has_theme(theme) ? theme : default_theme;
  for (std::size_t i = 0; i < def.themes.size(); ++i) {
    if (def.themes[i] != def.theme || !theme_palettes[i]) continue;
    if (std::string why; !apply_palette(*theme_palettes[i], "theme palette", pal, why)) {
      return fail(why);
    }
  }
  // Kept rather than dropped at the end of the parse: this is the palette a
  // derived colour is merged over, and keeping it is what makes a recolour a
  // pass over memory instead of a second trip through the filesystem.
  def.palette = pal;

  if (const json& anchors = member(root, "anchors"); !anchors.is_null()) {
    if (!anchors.is_object()) return fail("avatar.json: \"anchors\" must be an object");
    for (const auto& [key, value] : anchors.items()) {
      if (!value.is_array() || value.size() != 2 || !value[0].is_number_unsigned() ||
          !value[1].is_number_unsigned()) {
        return fail("avatar.json: anchor \"" + key + "\" must be [x, y]");
      }
      AvatarAnchor a;
      a.name = key;
      a.x = value[0].get<std::uint32_t>();
      a.y = value[1].get<std::uint32_t>();
      if (a.x >= def.width || a.y >= def.height) {
        return fail("avatar.json: anchor \"" + key + "\" is outside the grid");
      }
      def.anchors.push_back(std::move(a));
    }
  }

  if (!parse_clips(member(root, "clips"), dir, pal, def.width, def.height, true, "", "", def.clips,
                   error)) {
    return false;
  }

  const json& dflt = member(root, "default_clip");
  def.default_clip = dflt.is_string() ? dflt.get<std::string>() : def.clips.front().name;
  if (!def.find_clip(def.default_clip)) {
    return fail("avatar.json: default_clip \"" + def.default_clip + "\" is not a declared clip");
  }

  // Sprites: accessories with their own grid, their own clips and a placement
  // against one of the anchors above. This is where anchors stop being
  // decoration and start having to be right, so a placement that names one
  // that is not declared is an error rather than a silent (0,0).
  if (const json& sprites = member(root, "sprites"); !sprites.is_null()) {
    if (!sprites.is_array()) return fail("avatar.json: \"sprites\" must be an array");
    for (const auto& entry : sprites) {
      if (!entry.is_object() || !member(entry, "name").is_string()) {
        return fail("avatar.json: every sprite needs a string \"name\"");
      }
      AvatarSprite sprite;
      sprite.name = member(entry, "name").get<std::string>();
      const std::string owner = "sprite \"" + sprite.name + "\" ";
      if (def.find_sprite(sprite.name)) {
        return fail("avatar.json: " + owner + "is declared twice");
      }

      const json& sg = member(entry, "grid");
      const json& sw = member(sg, "width");
      const json& sh = member(sg, "height");
      const auto spw = sw.is_number_unsigned() ? sw.get<std::uint32_t>() : 0u;
      const auto sph = sh.is_number_unsigned() ? sh.get<std::uint32_t>() : 0u;
      if (spw == 0 || sph == 0 || spw > kAvatarMaxGrid || sph > kAvatarMaxGrid) {
        return fail("avatar.json: " + owner + "grid " + std::to_string(spw) + "x" +
                    std::to_string(sph) + " is outside 1x1.." + std::to_string(kAvatarMaxGrid) +
                    "x" + std::to_string(kAvatarMaxGrid));
      }
      sprite.width = spw;
      sprite.height = sph;

      const json& anchor = member(entry, "anchor");
      if (!anchor.is_string()) return fail("avatar.json: " + owner + "needs a string \"anchor\"");
      sprite.anchor = anchor.get<std::string>();
      if (!def.find_anchor(sprite.anchor)) {
        return fail("avatar.json: " + owner + "anchor \"" + sprite.anchor +
                    "\" is not a declared anchor");
      }

      // Signed, because the whole point is to sit above and beside the body.
      if (const json& off = member(entry, "offset"); !off.is_null()) {
        if (!off.is_array() || off.size() != 2 || !off[0].is_number_integer() ||
            !off[1].is_number_integer()) {
          return fail("avatar.json: " + owner + "offset must be [x, y] whole numbers");
        }
        sprite.offset_x = off[0].get<std::int32_t>();
        sprite.offset_y = off[1].get<std::int32_t>();
      }

      if (!parse_clips(member(entry, "clips"), dir, pal, sprite.width, sprite.height, false, owner,
                       sprite.name + "_", sprite.clips, error)) {
        return false;
      }
      const json& sdflt = member(entry, "default_clip");
      sprite.default_clip =
          sdflt.is_string() ? sdflt.get<std::string>() : sprite.clips.front().name;
      if (!sprite.find_clip(sprite.default_clip)) {
        return fail("avatar.json: " + owner + "default_clip \"" + sprite.default_clip +
                    "\" is not one of its clips");
      }
      def.sprites.push_back(std::move(sprite));
    }
  }

  // The envelope fixes the scale, so it has to fit the one buffer there is.
  // Checked here rather than at compose time because it depends only on the
  // definition: the author should be told when they save it, not when the
  // window happens to be a particular size.
  const Envelope env = envelope_of(def, nullptr);
  if (env.width > kAvatarMaxGrid || env.height > kAvatarMaxGrid) {
    return fail("avatar.json: the character plus its placed sprites span " +
                std::to_string(env.width) + "x" + std::to_string(env.height) + " cells, past the " +
                std::to_string(kAvatarMaxGrid) + "x" + std::to_string(kAvatarMaxGrid) +
                " maximum");
  }

  out = std::move(def);
  return true;
}

AvatarStage avatar_stage_layout(const AvatarDefinition& def, std::uint32_t band_w,
                                std::uint32_t band_h, const std::vector<bool>* visible) {
  // Size from the static envelope. The same integer rule M2.1 used for the
  // character, applied to the whole composition instead: the largest whole
  // number of pixels per cell that still fits. Every declared placement
  // counts whether it is on screen or not, so the scale is a property of the
  // definition and turning an accessory on can never resize the body.
  const Envelope all = envelope_of(def, nullptr);
  AvatarStage stage;
  stage.scale = std::max(1u, std::min(band_w / std::max(1u, all.width),
                                      band_h / std::max(1u, all.height)));
  // The stage then covers as much of the band as whole cells reach. Cells
  // beyond the envelope are the room an accessory has to clip into.
  stage.width = std::clamp(band_w / stage.scale, all.width, kAvatarMaxGrid);
  stage.height = std::clamp(band_h / stage.scale, all.height, kAvatarMaxGrid);

  // Position from the visible envelope, so what is actually on screen is what
  // is centred. With nothing up that is the character alone and the slime sits
  // dead centre; with a bubble out to the right the pair is centred, which
  // pushes the body left to make room. `shown` is a subset of `all` and
  // `stage.width` is at least `all.width`, so the difference cannot go
  // negative.
  const Envelope shown = envelope_of(def, visible);
  const auto ox = static_cast<std::int32_t>((stage.width - shown.width) / 2);
  const auto oy = static_cast<std::int32_t>((stage.height - shown.height) / 2);
  stage.char_x = ox - shown.min_x;
  stage.char_y = oy - shown.min_y;
  return stage;
}

// ---- M1c.5: the derivation -------------------------------------------------

namespace {

struct Hsl {
  float h = 0.0f;  // degrees, 0..360
  float s = 0.0f;
  float l = 0.0f;
};

Hsl rgb_to_hsl(float r, float g, float b) {
  const float mx = std::max(r, std::max(g, b));
  const float mn = std::min(r, std::min(g, b));
  Hsl out;
  out.l = (mx + mn) * 0.5f;
  const float d = mx - mn;
  if (d <= 1e-6f) return out;  // achromatic: hue and saturation stay 0
  out.s = out.l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
  if (mx == r) out.h = 60.0f * std::fmod((g - b) / d + 6.0f, 6.0f);
  else if (mx == g) out.h = 60.0f * ((b - r) / d + 2.0f);
  else out.h = 60.0f * ((r - g) / d + 4.0f);
  return out;
}

float hue_channel(float p, float q, float t) {
  if (t < 0.0f) t += 1.0f;
  if (t > 1.0f) t -= 1.0f;
  if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
  if (t < 0.5f) return q;
  if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
  return p;
}

void hsl_to_rgb(const Hsl& c, float& r, float& g, float& b) {
  if (c.s <= 1e-6f) {
    r = g = b = c.l;
    return;
  }
  const float q = c.l < 0.5f ? c.l * (1.0f + c.s) : c.l + c.s - c.l * c.s;
  const float p = 2.0f * c.l - q;
  const float h = c.h / 360.0f;
  r = hue_channel(p, q, h + 1.0f / 3.0f);
  g = hue_channel(p, q, h);
  b = hue_channel(p, q, h - 1.0f / 3.0f);
}

// WCAG relative luminance. Used rather than HSL lightness because lightness
// says a saturated yellow and a saturated blue at L=0.5 are equally light,
// and on screen they are nothing of the sort — which is exactly the case
// where a complement pair fails.
float srgb_linear(float c) {
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float luminance(float r, float g, float b) {
  return 0.2126f * srgb_linear(r) + 0.7152f * srgb_linear(g) + 0.0722f * srgb_linear(b);
}

float contrast_ratio(float l1, float l2) {
  const float hi = std::max(l1, l2) + 0.05f;
  const float lo = std::min(l1, l2) + 0.05f;
  return hi / lo;
}

// Below this the body is a grey and "the opposite hue" means nothing; the
// features get this much saturation anyway so they are still a colour.
constexpr float kAchromatic = 0.06f;
constexpr float kMinFeatureSaturation = 0.25f;

}  // namespace

AvatarDerivedPalette avatar_derive_palette(std::uint32_t body_rgba) {
  AvatarDerivedPalette out;
  const float br = static_cast<float>(avatar_r(body_rgba)) / 255.0f;
  const float bg = static_cast<float>(avatar_g(body_rgba)) / 255.0f;
  const float bb = static_cast<float>(avatar_b(body_rgba)) / 255.0f;
  // Opaque by construction: translucency is `jelly`'s idea, not a hue's, and
  // a picker that could make the body fade would be a second control wearing
  // the first one's clothes.
  out.body = avatar_rgba(avatar_r(body_rgba), avatar_g(body_rgba), avatar_b(body_rgba), 255);

  const Hsl body = rgb_to_hsl(br, bg, bb);
  const float body_lum = luminance(br, bg, bb);
  out.hue = body.h;
  out.body_l = body.l;
  out.achromatic = body.s < kAchromatic;
  // The two accessories drawn in body ink alone — zzz, question and steam have
  // no second ink to rescue them — read exactly as well as the body does and
  // no better, so the extremes are worth naming to the user rather than
  // leaving them to discover a thought that never appears.
  out.body_faint_dark = body_lum < 0.03f;
  out.body_faint_light = body_lum > 0.75f;

  // The hue is the user's decision and is never moved: exactly opposite.
  out.feature_hue = std::fmod(body.h + 180.0f, 360.0f);

  Hsl feature;
  feature.h = out.feature_hue;
  feature.s = std::max(body.s, kMinFeatureSaturation);

  // The minimum adjustment, as a search rather than a formula: walk the
  // lightness ramp upward from the body's own and stop at the first step that
  // clears kAvatarMinContrast. Every step costs colour — an HSL lightness
  // above the hue's natural level is literally white being mixed in — so the
  // first value that works is the most saturated complement this body allows.
  //
  // 256 steps because that is the resolution the 8-bit output has anyway;
  // a bisection would be fewer iterations and is not worth the asymmetry
  // (contrast is not quite monotonic in L for every hue near the top of the
  // ramp, and a linear scan is honest about that where a bisection is not).
  float fr = 0.0f, fg = 0.0f, fb = 0.0f;
  float best_ratio = 0.0f;
  Hsl best = feature;
  bool reached = false;
  for (int i = 0; i <= 256; ++i) {
    feature.l = body.l + (1.0f - body.l) * (static_cast<float>(i) / 256.0f);
    hsl_to_rgb(feature, fr, fg, fb);
    const float ratio = contrast_ratio(luminance(fr, fg, fb), body_lum);
    if (ratio > best_ratio) {
      best_ratio = ratio;
      best = feature;
    }
    if (ratio >= kAvatarMinContrast) {
      best = feature;
      best_ratio = ratio;
      reached = true;
      break;
    }
  }
  out.contrast_short = !reached;
  out.contrast = best_ratio;
  out.feature_l = best.l;
  hsl_to_rgb(best, fr, fg, fb);
  out.gloss_inverted = luminance(fr, fg, fb) < body_lum;

  auto q = [](float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  out.feature = avatar_rgba(q(fr), q(fg), q(fb), 255);
  // The glint is the features' colour at the base palette's own translucency,
  // so `*` follows `o` by construction and there is no third thing to tune.
  out.translucent = avatar_rgba(q(fr), q(fg), q(fb), 0xCC);
  return out;
}

void avatar_recolour(AvatarDefinition& def, const AvatarPalette& pal) {
  def.palette = pal;
  auto redo = [&pal](std::vector<AvatarClip>& clips) {
    for (AvatarClip& clip : clips) {
      for (AvatarFrame& frame : clip.frames) {
        const std::size_t n = frame.base.size();
        for (std::size_t i = 0; i < n && i < frame.base_ink.size(); ++i) {
          const std::uint8_t ink = frame.base_ink[i];
          if (ink) frame.base[i] = pal.rgba[ink];
        }
        const std::size_t m = frame.overlay.size();
        for (std::size_t i = 0; i < m && i < frame.overlay_ink.size(); ++i) {
          const std::uint8_t ink = frame.overlay_ink[i];
          if (ink) frame.overlay[i] = pal.rgba[ink];
        }
      }
    }
  };
  redo(def.clips);
  for (AvatarSprite& sprite : def.sprites) redo(sprite.clips);
}

fs::path avatar_user_root() {
  // Per-user roaming data, which is where a definition the user edits belongs
  // — it has to survive a rebuild, and it is theirs, not the install's.
  const std::string appdata = env_or("APPDATA", "");
  if (appdata.empty()) return fs::path("avatars");
  return fs::path(appdata) / "AIInterface" / "avatars";
}

fs::path seed_avatar_definition(const std::string& name) {
  const fs::path dest = avatar_user_root() / name;
  std::error_code ec;
  const fs::path source = fs::path(AII_ASSETS_DIR) / "avatars" / name;
  if (!fs::exists(source, ec)) return dest;  // the loader reports the miss
  fs::create_directories(dest.parent_path(), ec);
  // `update_existing`, and it used to be a bare `exists()` check that returned
  // early. That was a silent one-way door: the copy ran exactly once, so any
  // art added to assets/ afterwards never reached a machine that had already
  // started the app once. It cost a round on 16 Sep 2026 — a new sprite was
  // authored, declared, wired up, and simply never appeared, because the
  // definition being read was the one seeded days earlier and it had never
  // heard of it. Every clip and sprite the milestone plan still has to add
  // would have hit the same wall.
  //
  // The rule this replaces it with is the ordinary installer one: a file is
  // refreshed only when the shipped copy is *newer* than the user's. Art the
  // user has edited since the last release keeps their edit — which is the
  // property the old early return was reaching for — and art they have never
  // touched follows the app. `recursive` also adds files that are simply not
  // there yet, which is most of what this has to do.
  fs::copy(source, dest,
           fs::copy_options::recursive | fs::copy_options::update_existing, ec);
  return dest;
}

std::vector<std::string> avatar_definition_names() {
  std::vector<std::string> names;
  auto scan = [&names](const fs::path& root) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
      if (!entry.is_directory(ec)) continue;
      // A directory is an avatar only if it has the one file that makes it
      // one. Otherwise a stray folder — a backup, a half-finished sketch, the
      // editor's own scratch directory — would show up in the picker and pick
      // as an avatar that cannot load.
      if (!fs::exists(entry.path() / "avatar.json", ec)) continue;
      const std::string name = entry.path().filename().string();
      if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
  };
  scan(fs::path(AII_ASSETS_DIR) / "avatars");
  scan(avatar_user_root());
  std::sort(names.begin(), names.end());
  // "default" first whatever it sorts as: it is the one every install has and
  // the one an avatar that failed to load falls back to.
  const auto it = std::find(names.begin(), names.end(), std::string("default"));
  if (it != names.end()) std::rotate(names.begin(), it, it + 1);
  return names;
}

void AvatarSource::open(fs::path dir, std::string clip, std::vector<std::string> sprites) {
  dir_ = std::move(dir);
  wanted_clip_ = std::move(clip);
  wanted_sprites_ = std::move(sprites);
  reload(true);
}

const char* AvatarSource::custom_theme() { return "custom"; }

std::uint32_t AvatarSource::theme_body_colour(const std::string& theme_name) const {
  for (std::size_t i = 0; i < def_.themes.size() && i < def_.theme_body.size(); ++i) {
    if (def_.themes[i] == theme_name) return def_.theme_body[i];
  }
  return 0;
}

void AvatarSource::apply_custom() {
  // The derived theme is the definition's *default* palette with three inks
  // replaced, not a palette of its own: every ink the art has that the picker
  // does not drive keeps the colour avatar.json gives it, so adding a fourth
  // ink to the art shows up under the derived theme too, at its own colour.
  // Same reasoning as a named theme merging over the base.
  derived_ = avatar_derive_palette(custom_body_);
  AvatarPalette pal = def_.base_palette;
  auto put = [&pal](char ink, std::uint32_t rgba) {
    const auto i = static_cast<unsigned char>(ink);
    if (i < pal.known.size() && pal.known[i]) pal.rgba[i] = rgba;
  };
  put(def_.body_ink, derived_.body);
  put(def_.feature_ink, derived_.feature);
  for (const char c : def_.translucent_inks) {
    // Each translucent ink keeps the alpha the art gave it and takes the
    // features' colour, so a definition with two glints at two opacities does
    // not have both flattened to one.
    const auto i = static_cast<unsigned char>(c);
    if (i >= pal.known.size() || !pal.known[i]) continue;
    pal.rgba[i] = avatar_rgba(avatar_r(derived_.feature), avatar_g(derived_.feature),
                              avatar_b(derived_.feature), avatar_a(def_.base_palette.rgba[i]));
  }
  avatar_recolour(def_, pal);
  theme_ = custom_theme();
}

bool AvatarSource::set_custom_colour(std::uint32_t body_rgba) {
  // Opaque, and compared opaque: the picker has no alpha channel, and an
  // incoming 0 alpha from a settings file parsed as #rrggbb must not read as
  // a different colour from the same one picked in the UI.
  const std::uint32_t rgb =
      avatar_rgba(avatar_r(body_rgba), avatar_g(body_rgba), avatar_b(body_rgba), 255);
  if (custom_set_ && rgb == custom_body_) return false;
  custom_body_ = rgb;
  custom_set_ = true;
  // This is the drag path, and it is deliberately everything that happens on
  // it: a pass over the frames' ink bytes. No file is opened, no clip index or
  // frame timer is touched, and the slide is left where it is — so the avatar
  // animates *through* a drag rather than restarting on every frame of one.
  if (loaded_ && theme_ == custom_theme()) apply_custom();
  return true;
}

bool AvatarSource::set_theme(const std::string& theme_name) {
  if (theme_name == wanted_theme_) return true;
  // Before the first load there is nothing to validate against, so the name is
  // simply remembered and the load that follows decides. That is what lets
  // main.cpp push the stored theme in *before* open(), so the avatar's first
  // frame is already in the user's colours rather than flashing the default.
  if (loaded_ && theme_name != custom_theme() && !def_.has_theme(theme_name)) return false;
  // Switching *to* the derived theme is not a reload either: the frames on
  // screen were resolved with whatever palette the last load used, and the
  // ink bytes are still beside them, so the derived palette goes straight over
  // the top. Switching *away* from it is, because the named theme's colours
  // have to come back out of the file.
  if (loaded_ && theme_name == custom_theme()) {
    wanted_theme_ = theme_name;
    if (!custom_set_) {
      // Reaching the derived theme with no colour yet starts it from the one
      // that was on screen a moment ago, so the first thing the picker shows
      // is the avatar the user was already looking at.
      custom_body_ = theme_body_colour(def_.theme);
      custom_set_ = true;
    }
    apply_custom();
    return true;
  }
  wanted_theme_ = theme_name;
  // A reload, because the palette is resolved into the frames (see the note on
  // AvatarDefinition::themes). It is the same path a hot-reload takes and has
  // the same contract: a definition that will not load leaves the art that is
  // on screen exactly where it is.
  if (loaded_) reload(false);
  return true;
}

bool AvatarSource::show_sprite(const std::string& sprite, bool on) {
  for (std::size_t i = 0; i < def_.sprites.size() && i < sprite_state_.size(); ++i) {
    if (def_.sprites[i].name != sprite) continue;
    if (sprite_state_[i].on != on) {
      sprite_state_[i].on = on;
      // Restart rather than resume: an accessory that appears mid-cycle would
      // show a thought bubble already half drawn.
      sprite_state_[i].frame_index = 0;
      sprite_state_[i].frame_time = 0.0f;
    }
    return true;
  }
  return false;
}

bool AvatarSource::take_status_change() {
  const bool was = status_new_;
  status_new_ = false;
  return was;
}

fs::file_time_type AvatarSource::directory_stamp() const {
  // The newest write anywhere in the directory. A rename or a deletion moves
  // this too, because the entry that carried the old time is simply gone from
  // the walk on the next poll.
  fs::file_time_type newest{};
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir_, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const auto t = entry.last_write_time(ec);
    if (!ec && t > newest) newest = t;
  }
  return newest;
}

void AvatarSource::reload(bool initial) {
  AvatarDefinition fresh;
  std::string error;
  stamp_ = directory_stamp();
  if (load_avatar_definition(dir_, wanted_theme_, fresh, &error)) {
    def_ = std::move(fresh);
    loaded_ = true;
    // The clip the user asked for on the command line outlives a reload; if
    // the edit they just made removed it, fall back rather than freeze.
    const std::string want = !wanted_clip_.empty() ? wanted_clip_ : def_.default_clip;
    clip_index_ = 0;
    bool found = false;
    for (std::size_t i = 0; i < def_.clips.size(); ++i) {
      if (def_.clips[i].name != want) continue;
      clip_index_ = i;
      found = true;
    }
    std::string missing = found ? std::string() : " (no clip \"" + want + "\")";
    frame_index_ = 0;
    frame_time_ = 0.0f;

    // A reload is not a movement: the definition may have different sprites
    // in different places, and sliding from the old layout to the new one
    // would animate an edit rather than a state change.
    slide_valid_ = false;

    // Sprite state is rebuilt, not carried: a reload may have renamed or
    // reordered sprites, and an index into the old list would point at the
    // wrong art. Which ones are *wanted* is kept by name instead.
    sprite_state_.assign(def_.sprites.size(), SpriteState{});
    for (std::size_t i = 0; i < def_.sprites.size(); ++i) {
      const AvatarSprite& sprite = def_.sprites[i];
      for (std::size_t c = 0; c < sprite.clips.size(); ++c) {
        if (sprite.clips[c].name == sprite.default_clip) sprite_state_[i].clip_index = c;
      }
    }
    std::size_t shown = 0;
    for (const auto& wanted : wanted_sprites_) {
      if (wanted == "all") {
        for (auto& st : sprite_state_) st.on = true;
        shown = sprite_state_.size();
        continue;
      }
      if (show_sprite(wanted, true)) ++shown;
      else missing += " (no sprite \"" + wanted + "\")";
    }

    // A theme that is no longer declared is reported at the volume of a
    // problem, exactly as a missing clip is: the avatar is on screen in the
    // wrong colours, which is quiet enough to go unnoticed otherwise.
    // M1c.5: the derived theme is re-applied at the tail of every load, which
    // is what makes a hot edit of avatar.json and the colour picker unable to
    // fight. They write different files — the art is avatar.json, the pick is
    // settings.json — and a save of the art reloads it and then puts the pick
    // back on top, so a hand-tuned ink the picker does not drive survives the
    // picker and a picked colour survives the hand edit.
    theme_ = def_.theme;
    if (wanted_theme_ == custom_theme()) {
      // A stored "custom" with no stored colour beside it — a hand-edited
      // settings file, or one written before the colour was ever picked —
      // starts from the theme that would otherwise have loaded rather than
      // from black. Falling back to the named theme instead would silently
      // discard a setting the user did make.
      if (!custom_set_) {
        custom_body_ = theme_body_colour(def_.theme);
        custom_set_ = true;
      }
      apply_custom();
    }
    // The picker's list: what the art declares, plus the derived one, which is
    // only offerable once there is a definition loaded to derive against.
    theme_names_ = def_.themes;
    theme_names_.push_back(custom_theme());

    if (!wanted_theme_.empty() && wanted_theme_ != theme_)
      missing += " (no theme \"" + wanted_theme_ + "\")";
    status_ = (initial ? "avatar " : "avatar reloaded ") + def_.name + " [" + theme_ + "]: " +
              std::to_string(def_.clips.size()) + " clips, playing \"" +
              def_.clips[clip_index_].name + "\", " + std::to_string(def_.sprites.size()) +
              " sprites (" + std::to_string(shown) + " shown)" + missing;
    status_ok_ = missing.empty();
  } else if (loaded_) {
    // Requirement of the format, not an accident: a bad save must cost the
    // user the save, never the definition they already had.
    status_ = "avatar reload rejected, keeping the last good art - " + error;
    status_ok_ = false;
  } else {
    loaded_ = false;
    status_ = "avatar definition unusable, drawing the built-in placeholder - " + error;
    status_ok_ = false;
  }
  status_new_ = true;
}

bool AvatarSource::play(const std::string& clip, std::size_t start_frame) {
  if (!loaded_) return false;
  for (std::size_t i = 0; i < def_.clips.size(); ++i) {
    if (def_.clips[i].name != clip) continue;
    // Asking for the clip that is already running is a no-op, not a restart:
    // M2.4's policy states its wish every frame, and restarting on each of
    // them would freeze every clip on its first drawing.
    if (i != clip_index_) {
      clip_index_ = i;
      frame_index_ = start_frame < def_.clips[i].frames.size() ? start_frame : 0;
      frame_time_ = 0.0f;
    }
    return true;
  }
  return false;
}

namespace {

// One clip cursor, stepped by wall clock. Shared by the character and by
// every sprite so a sprite cannot quietly acquire different timing rules.
void advance_clip(const AvatarClip& clip, float dt, std::size_t& frame_index, float& frame_time) {
  if (clip.frames.size() <= 1) return;
  frame_time += dt;
  // A while loop rather than one step per frame: a stall (a reload, a resize)
  // must not turn into a clip that slowly falls behind wall clock. The step is
  // read inside the loop because each frame states its own hold, so the
  // duration changes as the cursor walks.
  for (;;) {
    if (frame_index >= clip.frames.size()) frame_index = 0;
    const float step = static_cast<float>(clip.frames[frame_index].hold) / clip.fps;
    if (frame_time < step) break;
    frame_time -= step;
    if (frame_index + 1 < clip.frames.size()) {
      ++frame_index;
    } else if (clip.loop) {
      frame_index = 0;
    } else {
      // A one-shot holds its last frame; M2.4 decides what follows it.
      frame_time = 0.0f;
      break;
    }
  }
}

}  // namespace

void AvatarSource::update(float dt) {
  poll_time_ += dt;
  if (poll_time_ >= kPollSeconds) {
    poll_time_ = 0.0f;
    if (const auto now = directory_stamp(); now != stamp_) reload(false);
  }
  if (!loaded_) return;

  // The slide runs on the same wall clock as the clips. compose() is what
  // notices that the resting place has changed and restarts it; this only
  // moves it along, so a frame where nothing changed costs an add.
  if (slide_t_ < 1.0f) slide_t_ = std::min(1.0f, slide_t_ + dt / kSlideSeconds);

  // Only the body takes the speed multiplier; the slide above and the sprites
  // below keep wall clock, so a loud talker does not also hurry the move that
  // makes room for an accessory.
  advance_clip(def_.clips[clip_index_], dt * std::max(0.0f, speed_), frame_index_, frame_time_);
  // Each accessory runs on its own cursor, so a bubble's dots keep their own
  // fps no matter what the body is doing.
  for (std::size_t i = 0; i < def_.sprites.size() && i < sprite_state_.size(); ++i) {
    SpriteState& st = sprite_state_[i];
    if (!st.on) continue;
    advance_clip(def_.sprites[i].clips[st.clip_index], dt, st.frame_index, st.frame_time);
  }
}

namespace {

// Stamps one cell block into a layer at a signed cell offset. Every cell is
// bounds-checked against the stage rather than the row being clipped once,
// because that is also what stops a partly off-stage sprite wrapping onto the
// opposite edge — the failure a packed grid invites. Transparent cells are
// skipped so a sprite composites over what is already there instead of
// punching a hole in it.
void blit(AvatarGrid& grid, AvatarLayer layer, const std::vector<std::uint32_t>& cells,
          std::uint32_t w, std::uint32_t h, std::int32_t dx, std::int32_t dy) {
  auto& dest = layer == AvatarLayer::Base ? grid.base : grid.overlay;
  for (std::uint32_t y = 0; y < h; ++y) {
    const std::int32_t ty = dy + static_cast<std::int32_t>(y);
    if (ty < 0 || ty >= static_cast<std::int32_t>(grid.height)) continue;
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::uint32_t v = cells[std::size_t{y} * w + x];
      if (v == 0) continue;
      const std::int32_t tx = dx + static_cast<std::int32_t>(x);
      if (tx < 0 || tx >= static_cast<std::int32_t>(grid.width)) continue;
      dest[static_cast<std::size_t>(ty) * grid.width + static_cast<std::size_t>(tx)] = v;
    }
  }
}

}  // namespace

namespace {

// Slow in, slow out — the same smoothstep the window's fades use, for the
// same reason: a move that starts and stops abruptly reads as a jump however
// long you give it.
float ease(float t) { return t * t * (3.0f - 2.0f * t); }

// The eased position, rounded back to a whole cell. Rounding here rather than
// letting the renderer take a fractional origin is what keeps the avatar on
// the pixel grid while it is moving: the slide is a short series of cell
// steps, eased in time rather than in space.
std::int32_t slide_cell(std::int32_t from, std::int32_t to, float t) {
  const float e = ease(std::clamp(t, 0.0f, 1.0f));
  return from + static_cast<std::int32_t>(std::lround(static_cast<double>(to - from) * e));
}

}  // namespace

std::int32_t AvatarSource::slide_cell_x() const {
  return slide_cell(slide_from_x_, slide_to_x_, slide_t_);
}

std::int32_t AvatarSource::slide_cell_y() const {
  return slide_cell(slide_from_y_, slide_to_y_, slide_t_);
}

void AvatarSource::compose(AvatarGrid& grid, std::uint32_t band_w, std::uint32_t band_h) {
  if (!loaded_) {
    avatar_placeholder_blob(grid);
    return;
  }
  // Rebuilt from sprite_state_ rather than tracked alongside it: one source of
  // truth for what is up, and assign() on a vector that is already the right
  // size does not allocate.
  visible_.assign(def_.sprites.size(), false);
  for (std::size_t i = 0; i < def_.sprites.size() && i < sprite_state_.size(); ++i) {
    visible_[i] = sprite_state_[i].on;
  }

  const AvatarStage stage = avatar_stage_layout(def_, band_w, band_h, &visible_);

  // Where the composition wants to sit. `stage.width` and `stage.scale` come
  // from the static envelope and so never change here; only this does.
  if (!slide_valid_) {
    // First frame after a load: be where you belong, do not slide in from
    // wherever the last definition happened to sit.
    slide_from_x_ = slide_to_x_ = stage.char_x;
    slide_from_y_ = slide_to_y_ = stage.char_y;
    slide_t_ = 1.0f;
    slide_valid_ = true;
  } else if (stage.char_x != slide_to_x_ || stage.char_y != slide_to_y_) {
    // An accessory went up or came down. Start from wherever the last slide
    // had got to, so interrupting one mid-move does not snap.
    slide_from_x_ = slide_cell_x();
    slide_from_y_ = slide_cell_y();
    slide_to_x_ = stage.char_x;
    slide_to_y_ = stage.char_y;
    slide_t_ = 0.0f;
  }
  const std::int32_t char_x = slide_cell_x();
  const std::int32_t char_y = slide_cell_y();

  if (grid.width != stage.width || grid.height != stage.height) {
    grid.resize(stage.width, stage.height);
  }
  grid.scale = stage.scale;
  // The stage is composited from several pieces now, so it is cleared rather
  // than overwritten wholesale: resize() only clears when the size changed,
  // and a sprite that moved or switched off would otherwise leave its last
  // cells behind.
  const std::size_t used = std::size_t{stage.width} * stage.height;
  std::fill_n(grid.base.begin(), used, 0u);
  std::fill_n(grid.overlay.begin(), used, 0u);

  const AvatarFrame& frame = def_.clips[clip_index_].frames[frame_index_];
  blit(grid, AvatarLayer::Base, frame.base, def_.width, def_.height, char_x, char_y);
  blit(grid, AvatarLayer::Overlay, frame.overlay, def_.width, def_.height, char_x, char_y);

  // Accessories land in the overlay layer: that is what it is for, and it
  // means a bubble never destroys body art it happens to pass over.
  for (std::size_t i = 0; i < def_.sprites.size() && i < sprite_state_.size(); ++i) {
    const SpriteState& st = sprite_state_[i];
    if (!st.on) continue;
    const AvatarSprite& sprite = def_.sprites[i];
    const AvatarAnchor* a = def_.find_anchor(sprite.anchor);
    if (!a) continue;
    const AvatarFrame& sf = sprite.clips[st.clip_index].frames[st.frame_index];
    blit(grid, AvatarLayer::Overlay, sf.base, sprite.width, sprite.height,
         char_x + static_cast<std::int32_t>(a->x) + sprite.offset_x,
         char_y + static_cast<std::int32_t>(a->y) + sprite.offset_y);
  }
}

}  // namespace aii
