#pragma once
// The avatar definition format (M2.2): what an avatar *is*, as data the user
// can author and diff in a text editor rather than code we recompile.
//
// A definition is a directory:
//
//   avatar.json   grid size, palette (character -> RGBA), anchors, the clip
//                 table and which clip plays by default
//   <clip>.txt    the frames of one clip, as ASCII art blocks
//
// Frames live in their own files rather than inline in the JSON because JSON
// has no multi-line string: inline art would be an array of quoted, comma
// separated rows, and the quotes and commas are exactly the noise that stops
// a 16x16 picture reading as a picture in `git diff`. A `.txt` block *is* the
// picture, and a one-cell change shows up as a one-character diff on one
// line. The cost is one extra file per clip, which git handles better than a
// thousand-line JSON anyway.
//
// A clip file is a sequence of blocks, each introduced by a marker line:
//
//   @frame     the next frame's base layer   (the character)
//   @overlay   that same frame's overlay     (accessories and particles)
//
// `@frame` takes one optional attribute, `hold=N`: the frame occupies N clip
// ticks instead of one. Without it the only way to express an uneven rhythm —
// a blink that stays open for two seconds and shuts in a tenth, a `zZz` that
// rises slowly and fades fast — is to repeat the block, which at 16x16 costs
// sixteen lines per extra tick and buries the one frame that differs. N
// defaults to 1, so every file written before it existed keeps its timing.
//
// followed by exactly `height` rows of exactly `width` characters, each
// indexing the palette, with '.' reserved for transparent. `#` comments and
// blank lines are ignored anywhere. The two layers AvatarRenderer composites
// are therefore expressed per frame, and `@overlay` is optional — a clip that
// never mentions it simply leaves that layer empty, so authoring a body-only
// clip costs nothing.
//
// Definitions are seeded into %APPDATA%\AIInterface\avatars\<name>\ from the
// repo's assets/avatars/<name>\ on first run, and only when the destination
// is absent: a rebuild must never reach in and overwrite art the user has
// been tuning.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "avatar_renderer.h"

namespace aii {

// One frame, palette already resolved to RGBA. Both layers are packed at the
// definition's own width, which is the layout AvatarGrid uses, so composing a
// frame is a copy rather than a per-cell translation.
struct AvatarFrame {
  std::vector<std::uint32_t> base;
  std::vector<std::uint32_t> overlay;
  // How many clip ticks this frame occupies (`@frame hold=N`). The clip's fps
  // stays the unit of time; hold is how many of those units one drawing is
  // worth, so a rhythm is edited by changing a number rather than by
  // duplicating art.
  std::uint32_t hold = 1;
};

struct AvatarClip {
  std::string name;
  float fps = 8.0f;
  bool loop = true;
  std::vector<AvatarFrame> frames;
};

// A cell the overlay system hangs accessories off, named so the art and the
// code that places a thought bubble do not have to agree on numbers. Anchors
// are in *character* space: cell (0,0) is the character's own top-left, not
// the stage's.
struct AvatarAnchor {
  std::string name;
  std::uint32_t x = 0;
  std::uint32_t y = 0;
};

// An accessory: its own little grid with its own clips, pinned to one of the
// character's anchors and drawn *outside* the character's 16x16 (user, 16 Sep
// 2026 — the slime stays a true 16x16 and accessories never overlap the
// body). The offset is signed and in cells, so `head_top` with (+4,-3) is up
// and to the right of the head, and it is measured from the anchor cell to
// the sprite's own top-left.
//
// Sprites share the character's palette rather than carrying one. They are
// part of the same picture: an author who retunes the ink wants the bubble to
// follow, and one character meaning one colour everywhere is the property
// that makes the .txt files readable at a glance. A per-sprite palette is a
// strictly additive change if a sprite ever needs a colour of its own.
struct AvatarSprite {
  std::string name;
  std::uint32_t width = 1;
  std::uint32_t height = 1;
  std::string anchor;
  std::int32_t offset_x = 0;
  std::int32_t offset_y = 0;
  std::vector<AvatarClip> clips;
  std::string default_clip;

  const AvatarClip* find_clip(const std::string& clip_name) const;
};

struct AvatarDefinition {
  std::string name;
  std::uint32_t width = 16;
  std::uint32_t height = 16;
  std::vector<AvatarClip> clips;
  std::vector<AvatarAnchor> anchors;
  std::vector<AvatarSprite> sprites;
  std::string default_clip;

  const AvatarClip* find_clip(const std::string& clip_name) const;
  const AvatarAnchor* find_anchor(const std::string& anchor_name) const;
  const AvatarSprite* find_sprite(const std::string& sprite_name) const;
};

// Where everything goes, in cells, for a given band. The grid is a stage: it
// covers the band at `scale`, and the character sits at (char_x, char_y) with
// its sprites hung off that.
//
// Size and position are drawn from two different envelopes, and the split is
// the whole design:
//
//   `scale` and `width`/`height` come from the *static* envelope — the
//   character's box unioned with every declared placement, visible or not.
//   Sizing to only the visible ones would make the slime jump a scale the
//   moment a thought bubble appeared, which is exactly when the user is
//   looking at it; sizing to all of them makes the cost of a far-flung
//   accessory a fixed, visible property of the definition.
//
//   `char_x`/`char_y` come from the *visible* envelope. Centring the static
//   one instead left the slime permanently off-centre by however much side
//   room the definition reserved, even with nothing in it — and at rest, with
//   no accessory showing, is the state the user sees almost all of the time
//   (user, 16 Sep 2026). So the composition is centred on what is actually on
//   screen, and the body makes room when an accessory arrives.
//
// The body therefore moves when a sprite appears, but it never changes size
// and the grid is never resized. AvatarSource eases that move and keeps it on
// whole cells; see the slide members there.
struct AvatarStage {
  std::uint32_t scale = 1;
  std::uint32_t width = 1;
  std::uint32_t height = 1;
  std::int32_t char_x = 0;
  std::int32_t char_y = 0;
};

// Fails only if the envelope cannot fit kAvatarMaxGrid, which load-time
// validation has already ruled out.
//
// `visible` is one flag per sprite, in def.sprites order, and decides the
// position only. Passing nullptr treats every declared sprite as visible,
// which is the static layout — useful to ask "where would this sit with
// everything up?" without having to build a vector of trues.
AvatarStage avatar_stage_layout(const AvatarDefinition& def, std::uint32_t band_w,
                                std::uint32_t band_h,
                                const std::vector<bool>* visible = nullptr);

// Loads `dir/avatar.json` and the clip files it names. Returns false with a
// one-line reason in `error` — the caller shows that line and falls back, so
// it names the file and, where there is one, the line number.
bool load_avatar_definition(const std::filesystem::path& dir, AvatarDefinition& out,
                            std::string* error);

// %APPDATA%\AIInterface\avatars — where the user's editable copies live.
std::filesystem::path avatar_user_root();

// Copies `assets/avatars/<name>` to the user root if, and only if, the
// destination does not exist yet. Returns the directory to load from either
// way, so a failed copy still yields a path whose loader failure is the one
// the user sees.
std::filesystem::path seed_avatar_definition(const std::string& name);

// Holds the definition, plays one clip and watches the directory.
//
// Everything that can go wrong here is a file the user is mid-edit, so
// nothing throws and nothing fails hard: compose() always leaves a drawable
// grid, dropping back to avatar_placeholder_blob() when there has never been
// a good definition, and *keeping the last good one* when a reload is what
// broke. Losing tuned art to a stray keystroke would be worse than showing it
// one save stale.
class AvatarSource {
 public:
  // `dir` is the definition directory; `clip` is the clip to start on, empty
  // for the definition's default; `sprites` are accessories to force on (the
  // single name "all" turns on every sprite the definition declares).
  void open(std::filesystem::path dir, std::string clip, std::vector<std::string> sprites);

  // Polls the directory on a fixed cadence and advances the current clip by
  // wall-clock time. `dt` is the frame's own delta, in seconds.
  void update(float dt);

  // Writes the current frame into the grid, or the placeholder if there is no
  // definition to write. The band is passed because the stage is sized to it;
  // pass the same values the renderer's write_slot is given, or the art will
  // be laid out for a band it is not drawn into.
  //
  // Not const: the composition's resting place depends on which accessories
  // are up, so this is where a change of placement is noticed and the slide
  // toward it is started. Call it after update() on the same frame — update()
  // is what advances the slide.
  void compose(AvatarGrid& grid, std::uint32_t band_w, std::uint32_t band_h);

  // `start_frame` is where the clip is entered, and exists for exactly one
  // case: blink.txt spends its first frame holding the open eye for two
  // seconds, which *is* the interval between blinks. M2.4 owns that interval
  // (randomised, so it does not look mechanical), so it enters the clip at
  // the lid instead. Out of range is clamped to 0, which is also what every
  // other caller wants.
  bool play(const std::string& clip, std::size_t start_frame = 0);

  // Multiplies the character clip's clock. The frames are fixed art, so the
  // only continuous lever a policy has over a clip is how fast it runs
  // through it: this is how mic RMS reaches the listen lean and the playback
  // level reaches the talk bounce. Sprites are deliberately left at 1 — a
  // thought bubble's dots are the bubble's own rhythm, not the body's.
  void set_speed(float speed) { speed_ = speed; }

  // Accessories are off until something turns them on. Deciding *when* is
  // M2.4's policy; this is the switch it will throw, and what --sprite throws
  // now so the placement can be seen at all.
  bool show_sprite(const std::string& sprite, bool on);

  bool loaded() const { return loaded_; }
  const AvatarDefinition& definition() const { return def_; }
  // One line: what was loaded, or why it was not. Set on every load attempt.
  const std::string& status() const { return status_; }
  // Whether that line is good news. A rejected reload is `loaded()` — the
  // previous art is still on screen — but it is not ok, and the user needs to
  // see it at the volume of a problem.
  bool status_ok() const { return status_ok_; }
  // True once per new status, so the frame loop can log a change without
  // repeating the same line sixty times a second.
  bool take_status_change();

 private:
  void reload(bool initial);
  std::filesystem::file_time_type directory_stamp() const;

  // The slide's position right now, rounded to a whole cell. Everything that
  // places art goes through these, so there is exactly one line in the program
  // where the eased value stops being fractional.
  std::int32_t slide_cell_x() const;
  std::int32_t slide_cell_y() const;

  std::filesystem::path dir_;
  AvatarDefinition def_;
  bool loaded_ = false;
  std::string status_;
  bool status_ok_ = false;
  bool status_new_ = false;

  std::string wanted_clip_;  // the --clip request, kept across reloads
  std::size_t clip_index_ = 0;
  std::size_t frame_index_ = 0;
  float frame_time_ = 0.0f;
  float speed_ = 1.0f;

  // A sprite runs its own clip on its own clock: a bubble's dots have nothing
  // to do with the body's breathe, and tying them to one cursor would force
  // one fps on both.
  struct SpriteState {
    bool on = false;
    std::size_t clip_index = 0;
    std::size_t frame_index = 0;
    float frame_time = 0.0f;
  };
  std::vector<SpriteState> sprite_state_;
  // Requested by name, so a --sprite that names something the definition does
  // not have yet survives the edit that adds it.
  std::vector<std::string> wanted_sprites_;

  // Which sprites are up, as avatar_stage_layout wants it. A member rather
  // than a local so composing a frame does not allocate; it is rebuilt from
  // sprite_state_ every compose, which is the one place that cannot go stale.
  std::vector<bool> visible_;

  // The slide between two resting places. A body that teleports sideways the
  // instant a bubble appears reads as a glitch; the same move over a fifth of
  // a second reads as making room for it.
  //
  // Both ends are whole cells and the interpolated value is rounded back to a
  // whole cell, so the slide is a short series of cell steps rather than a
  // smooth sub-cell glide. That is deliberate: the entire avatar is
  // pixel-perfect because every origin is an integer number of cells, and an
  // eased offset is fractional by nature — landing on a half cell mid-slide
  // would put the whole grid on a half pixel at scale 16 and lose the one
  // property this design is built on. At 16 px a cell the steps are big
  // enough to see, which is why they are eased rather than linear: slow in,
  // slow out, so the three-cell shift reads as one movement.
  std::int32_t slide_from_x_ = 0;
  std::int32_t slide_from_y_ = 0;
  std::int32_t slide_to_x_ = 0;
  std::int32_t slide_to_y_ = 0;
  float slide_t_ = 1.0f;     // 0 at the start of a move, 1 when it has landed
  bool slide_valid_ = false;  // false until the first compose places it

  float poll_time_ = 0.0f;
  std::filesystem::file_time_type stamp_{};
};

}  // namespace aii
