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
// A clip entry may name a `trigger` (M7.1), and clips sharing one are
// variants of it: asking to play that name plays one of them, chosen so that
// the same one does not come up twice running, with an optional per-clip
// `weight` to make one of them the usual and another a surprise. A clip is
// always still reachable by its own name, so a script can ask for one
// particular drawing as well as for "any entrance". Neither field is required
// and neither exists in any avatar written before them: a definition that
// mentions no trigger has one single-member set per clip, which is the same
// arrangement as having no sets at all, and plays exactly what it played
// before. Variants of one trigger must share the grid and — being usually
// one-shots — end on the drawing whatever follows them begins on, or the
// handover is a jump.
//
// Definitions are seeded into %APPDATA%\AIInterface\avatars\<name>\ from the
// repo's assets/avatars/<name>\ on first run, and only when the destination
// is absent: a rebuild must never reach in and overwrite art the user has
// been tuning.
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "avatar_renderer.h"

namespace aii {

// The palette as the loader resolved it, as a flat table indexed by the ink's
// own ASCII character. Kept on the loaded definition rather than thrown away
// at the end of the parse (M1c.5): a palette that is still around is what
// lets a colour change be a re-resolve instead of a reload.
struct AvatarPalette {
  std::array<std::uint32_t, 128> rgba{};
  std::array<bool, 128> known{};
};

// One frame, palette already resolved to RGBA. Both layers are packed at the
// definition's own width, which is the layout AvatarGrid uses, so composing a
// frame is a copy rather than a per-cell translation.
//
// `base_ink`/`overlay_ink` are the same two layers as the *ink characters*
// they were drawn with, 0 meaning transparent. They are the M1c.5 colour
// picker's whole trick: the RGBA above stays exactly what the frame loop
// reads, and a new palette is applied by walking these and rewriting the RGBA
// in place — no file is touched, nothing is re-parsed, and above all the clip
// cursor is not disturbed, so the avatar goes on animating through a drag
// instead of restarting on every frame of it. The cost is one byte per cell
// per layer: 71 frames of 16x16 and smaller across this definition, which is
// about 28 KB of extra memory and 28k byte-reads-and-word-writes to apply a
// whole new palette — small enough that a drag does it every frame.
struct AvatarFrame {
  std::vector<std::uint32_t> base;
  std::vector<std::uint32_t> overlay;
  std::vector<std::uint8_t> base_ink;
  std::vector<std::uint8_t> overlay_ink;
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

  // M7.1. The trigger this clip answers to, from avatar.json's optional
  // `trigger` on the clip entry. Empty means the clip's own name, which is
  // what every clip written before this existed gets — so a definition that
  // declares no triggers has one single-member group per clip and behaves
  // exactly as it did before there were groups at all.
  std::string trigger;
  // Relative likelihood inside that group, from the optional `weight`.
  // Uniform unless somebody says otherwise; only consulted when a group has
  // more than one member, so this number cannot change a no-variant
  // definition even if it is written down.
  float weight = 1.0f;
};

// M7.1. One member of a trigger group.
struct AvatarVariant {
  std::size_t clip_index = 0;
  float weight = 1.0f;
};

// M7.1. A name something can ask to play, and the clips that answer to it.
//
// Every clip is reachable by its own name — that is what keeps `--clip`, the
// bus's `play` and a script asking for one particular drawing working — and a
// clip that declares a `trigger` is *also* a member of that group. So
// `wake_slide` plays exactly that clip and `wake` picks between the clips
// that carry the trigger `wake`.
//
// A group of one is the overwhelmingly common case and is deliberately not a
// special case anywhere except in the chooser, which returns its single member
// without touching the random engine. That is the backward-compatibility
// guarantee expressed as code rather than as a promise.
struct AvatarTrigger {
  std::string name;
  std::vector<AvatarVariant> variants;
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

  // M7.1. The trigger table, derived from `clips` at load time: one entry per
  // distinct name anything may ask for, in declaration order. Derived rather
  // than authored so there is no second list in avatar.json to keep in step
  // with the first.
  std::vector<AvatarTrigger> triggers;

  // Load-time complaints that were survivable — today, a variant whose art
  // would not parse while a sibling of the same trigger did. The definition
  // still loads and the trigger still plays; the note is what stops that
  // being silent. A complaint that is *not* survivable is still an error and
  // still costs the whole load, exactly as before.
  std::vector<std::string> warnings;

  // M1c.4: the named palette variants this definition declares, in the order
  // they are declared — which is the order the settings picker shows them in,
  // so the author decides what comes first. Always non-empty: a definition
  // with no `themes` block reports the one implicit theme, its bare palette.
  //
  // The colours themselves are *not* here. A theme is resolved into the
  // frames at load time (parse_clip_file writes RGBA, not palette indices),
  // so what a loaded definition carries is the theme it was loaded *with*,
  // and changing theme means loading again. That is a deliberate trade: the
  // alternative is storing every frame as indices and resolving per cell on
  // every compose, which would put a palette lookup in the frame loop to save
  // a reload the user asks for by hand a handful of times a session. The
  // reload path is already the hot-reload path, which runs on every save.
  std::vector<std::string> themes;
  std::string theme;  // the one that was applied to the frames above

  // M1c.5. The palette that was applied, kept so a colour can be changed
  // without a reload, and the body colour each declared theme would have
  // given — which is what lets picking `ember` seed the colour picker with
  // ember's own body instead of resetting it. One entry per `themes` entry.
  AvatarPalette palette;
  // The bare `palette` block, before any theme was merged over it. The derived
  // theme merges over *this*, exactly as a named theme does, so what it gives
  // depends only on the picked colour and never on which theme happened to be
  // loaded when the user reached for the picker.
  AvatarPalette base_palette;
  std::vector<std::uint32_t> theme_body;

  // Which inks the derived-colour theme drives, from avatar.json's optional
  // `custom_inks`. Defaults are this avatar's, and they are defaults rather
  // than constants so a second definition drawn with different letters is a
  // data change and not a code change. `translucent` may name several inks or
  // none; each keeps its own alpha and takes the feature's hue.
  char body_ink = '#';
  char feature_ink = 'o';
  std::string translucent_inks = "*";

  bool has_theme(const std::string& theme_name) const;

  const AvatarClip* find_clip(const std::string& clip_name) const;
  // M7.1. The group `name` asks for, or nullptr. Every clip name is a group,
  // so this answers for everything find_clip() answers for and for trigger
  // names besides.
  const AvatarTrigger* find_trigger(const std::string& trigger_name) const;
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
//
// `theme` names one of the definition's palette variants (M1c.4). Empty means
// the definition's own `default_theme`. A name the definition does not have is
// **not** an error: it falls back to the default and says so in `out.theme`,
// because the name comes from a settings file or, later, from the bus, and a
// theme deleted from the art must not leave the user with no avatar at all.
bool load_avatar_definition(const std::filesystem::path& dir, const std::string& theme,
                            AvatarDefinition& out, std::string* error);

// ---- M1c.5: a picked body colour and the palette derived from it ----------
//
// The user picks one colour — the body — and the app derives the rest. Their
// decision, taken with the consequence stated: the feature ink is the **true
// colour-theory complement**, the opposite hue on the wheel, and the hue is
// never moved off it. Some picks therefore land on a pair that is hard to
// read at 16x16; that was chosen deliberately over a legibility-first rule
// that would have quietly returned a different hue than the wheel says.
//
// "Adjusted only as far as it must be" is the other half of it, and it lands
// entirely on *lightness*:
//
//   `#` is the picked colour verbatim, forced opaque. Translucency is a
//        property of the named `jelly` preset, not something a hue implies.
//   `o`  takes hue = body hue + 180 exactly, saturation = the body's (with a
//        floor, see `achromatic`), and the **smallest lightness at or above
//        the body's** that reaches a contrast ratio of kAvatarMinContrast
//        against it. Smallest, because every step up the lightness ramp is a
//        step toward white and away from the complement's colour: stopping at
//        the first value that works is what makes the adjustment minimal.
//        `o` is never darker than `#` — all sixteen clip files use it for the
//        gloss on the dome as well as the eyes, and a dark `o` turns that
//        highlight into a scuff mark.
//   `*`  is `o`'s colour at `o`'s alpha in the base palette, so the one glint
//        in think.txt stays consistent with the features by construction.
//
// Where that is not enough the honest answer is to say so rather than to
// override the hue, so everything the UI needs to say it is reported back:
// the ratio actually reached, whether white itself was not enough, and
// whether the body had a hue worth complementing at all.
constexpr float kAvatarMinContrast = 3.0f;

struct AvatarDerivedPalette {
  std::uint32_t body = 0;
  std::uint32_t feature = 0;
  std::uint32_t translucent = 0;

  float hue = 0.0f;          // the body's hue, degrees
  float feature_hue = 0.0f;  // always (hue + 180) mod 360 — never adjusted
  float body_l = 0.0f;       // HSL lightness of each, 0..1
  float feature_l = 0.0f;
  float contrast = 1.0f;  // WCAG ratio actually reached, feature against body

  // The body is so pale that even a white `o` does not reach
  // kAvatarMinContrast. The features still carry the complement's hue as far
  // as they can, but the pair is faint and the UI says so.
  bool contrast_short = false;
  // The complement came out *perceptually darker* than the body even though
  // its HSL lightness is not below the body's — which happens for a luminous
  // mid-tone hue, a green especially, whose opposite is a magenta that carries
  // far less luminance at the same lightness. Every clip uses `o` for the
  // gloss on the top-left of the dome as well as for the eyes, so a darker
  // `o` turns that highlight into a scuff mark.
  //
  // It is reported and not corrected. Correcting it means lifting the feature
  // to near-white, which throws away the complement's colour entirely — the
  // one thing the user asked not to have done quietly.
  bool gloss_inverted = false;
  // The body has essentially no hue (a grey, a black, a white), so "the
  // opposite hue" does not mean anything. The features are given the minimum
  // tint so they are still a colour rather than a second grey, and the UI
  // says the complement is nominal.
  bool achromatic = false;
  // The body is dark enough (or pale enough) that the three body-ink-only
  // accessories — zzz, question, steam — have nothing to rescue them. They
  // read exactly as well as the body does against the desktop, and no better.
  bool body_faint_dark = false;
  bool body_faint_light = false;
};

AvatarDerivedPalette avatar_derive_palette(std::uint32_t body_rgba);

// Rewrites every frame's RGBA from its ink characters and `pal`, in place.
// Nothing is re-read and no cursor is touched: about 28k cells for the default
// avatar, which is why a colour picker can be dragged.
void avatar_recolour(AvatarDefinition& def, const AvatarPalette& pal);

// %APPDATA%\AIInterface\avatars — where the user's editable copies live.
std::filesystem::path avatar_user_root();

// Copies `assets/avatars/<name>` to the user root if, and only if, the
// destination does not exist yet. Returns the directory to load from either
// way, so a failed copy still yields a path whose loader failure is the one
// the user sees.
std::filesystem::path seed_avatar_definition(const std::string& name);

// Every avatar definition that could be opened, by name: the directories under
// assets/avatars that ship with the app, unioned with any the user has added
// to %APPDATA%\AIInterface\avatars, sorted, with "default" first. This is the
// avatar picker's list (M1c.3); it is a directory listing rather than a
// manifest because a definition *is* a directory and a manifest would be a
// second thing to keep in step with it.
std::vector<std::string> avatar_definition_names();

// Holds the definition, plays one clip and watches the directory.
//
// Everything that can go wrong here is a file the user is mid-edit, so
// nothing throws and nothing fails hard: compose() always leaves a drawable
// grid, *keeping the last good definition* when a reload is what broke.
// Losing tuned art to a stray keystroke would be worse than showing it one
// save stale.
//
// What it will not do is draw a substitute. There used to be a 16x16 blob
// compiled into the binary for the never-loaded case, and it cost this
// project a day: the user reported the avatar "looks static", and it was not
// a frozen clip, it was that blob, reached because the definition directory
// was wrong. A shape that only appears when the real art fails makes a broken
// install look like a working app with a different bug. So the only art this
// program draws is art it loaded from a file: a definition that will not load
// falls back to `default` by *loading* it, and a `default` that will not load
// leaves the band empty and says so.
class AvatarSource {
 public:
  // `dir` is the definition directory; `clip` is the clip to start on, empty
  // for the definition's default; `sprites` are accessories to force on (the
  // single name "all" turns on every sprite the definition declares).
  //
  // `fallback_name` is the definition to load instead if `dir` will not load
  // — "default" from main.cpp, empty for a caller that means *exactly this
  // directory* (AII_AVATAR_DIR, whose whole purpose is to reach the failure
  // paths). It is a name rather than a path because it is resolved through
  // seed_avatar_definition() and only when it is needed: a fallback that
  // seeded on every open would touch the user's %APPDATA% copy of `default`
  // on every launch, which is a file write for a case that almost never
  // happens. The fallback is taken at most once per open(), so a `default`
  // that is itself broken cannot re-enter it.
  void open(std::filesystem::path dir, std::string clip, std::vector<std::string> sprites,
            std::string fallback_name = {});

  // M1c.4: the whole of the theme mechanism's outside, deliberately narrow.
  //
  // A theme is chosen by *name*, never by index and never by colour, so every
  // caller that will ever set one — the settings picker today, M2.5's app bus
  // tomorrow, a Python driver after that — is the same one-line call against
  // the same vocabulary the art declares. Wiring the bus to it is a call site,
  // not a redesign.
  //
  // Returns false only when the name is not one this definition declares; the
  // theme is unchanged then and the caller keeps whatever it had, which is
  // what makes a stale settings file harmless. Asking for the theme that is
  // already on is a no-op, so a frame loop may mirror a stored value into this
  // every frame the same way it mirrors `muted`.
  bool set_theme(const std::string& theme_name);
  const std::string& theme() const { return theme_; }
  // The art's own themes with the derived one appended, which is the picker's
  // list. Appended rather than declared in avatar.json because it has no
  // colours of its own to declare: it *is* whatever the user picked, and a
  // definition that listed it would be claiming to own a value stored in
  // settings.json.
  const std::vector<std::string>& themes() const { return theme_names_; }

  // M1c.5. The name of the derived theme, the one the colour picker drives.
  static const char* custom_theme();

  // The picked body colour. Setting it while the derived theme is on is the
  // live-drag path: the palette is re-resolved into the frames and nothing
  // else happens — no file is read, no clip restarts, the slide is untouched.
  // Setting it while a named theme is on only stores it, so that switching to
  // the derived theme later brings the colour back.
  //
  // Returns true if the colour changed anything (false when it was already
  // this colour), so the caller can decide to persist on the edge.
  bool set_custom_colour(std::uint32_t body_rgba);
  std::uint32_t custom_colour() const { return custom_body_; }
  // What the current picked colour derives to, for the UI to show and explain.
  const AvatarDerivedPalette& derived() const { return derived_; }
  // The body colour a named theme would give, so selecting a preset can seed
  // the picker. 0 if the name is not a declared theme.
  std::uint32_t theme_body_colour(const std::string& theme_name) const;

  // Polls the directory on a fixed cadence and advances the current clip by
  // wall-clock time. `dt` is the frame's own delta, in seconds.
  void update(float dt);

  // Writes the current frame into the grid, or clears it if there is no
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
  //
  // M7.1: `clip` may name a trigger with several clips behind it, in which
  // case one of them is chosen here. Asking again for a trigger whose chosen
  // variant is already running is the same no-op it has always been for a
  // plain clip name — the chooser runs when the trigger is *entered*, not on
  // every frame the policy restates its wish, or the clip would restart sixty
  // times a second.
  bool play(const std::string& clip, std::size_t start_frame = 0);

  // M7.1, for the log line that makes the chooser visible and for anything
  // that wants to know which variant it got: the clip actually on screen.
  // Empty when there is no definition loaded.
  const std::string& playing() const;

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

  // M7.1. Which clip of a trigger group plays next.
  //
  // The rule is a *weighted draw that cannot repeat the last pick*, not a
  // shuffle bag. The bag was the tempting answer and it is the wrong one
  // here: with weights it has to quantise them into copies, which turns "this
  // one is rare" into "this one is guaranteed once every N", and a surprise
  // on a schedule is not a surprise. Rejection-free — the last pick is
  // excluded by leaving it out of the sum, not by re-rolling — so there is no
  // loop to spin when a group has one member.
  //
  // A group of one returns its member and never touches rng_. That is what
  // makes a definition declaring no variants bit-for-bit what it was.
  std::size_t choose_variant(const AvatarTrigger& trigger);
  std::filesystem::file_time_type directory_stamp() const;

  // The slide's position right now, rounded to a whole cell. Everything that
  // places art goes through these, so there is exactly one line in the program
  // where the eased value stops being fractional.
  std::int32_t slide_cell_x() const;
  std::int32_t slide_cell_y() const;

  std::filesystem::path dir_;
  // The definition to fall back to, cleared the moment it is taken so the
  // fallback cannot recurse. Set by open(), never by a reload: a hot edit
  // that breaks the art keeps the art, it does not fall back.
  std::string fallback_name_;
  // What the failed name was and why, carried into the status line the
  // successful fallback writes. Cleared by that line, so the complaint is
  // said once — loudly — and a later hot reload of the fallback is clean.
  std::string fallback_note_;
  AvatarDefinition def_;
  bool loaded_ = false;
  std::string status_;
  bool status_ok_ = false;
  bool status_new_ = false;

  std::string wanted_clip_;  // the --clip request, kept across reloads
  // The theme asked for, kept by name across reloads for the same reason the
  // clip is: a save that renames or removes it must not silently strand the
  // avatar on a palette the user did not choose, and a save that adds it back
  // must pick it up again.
  std::string wanted_theme_;
  // What is actually on the frames right now. Usually def_.theme; it is the
  // derived theme's name instead when the picked colour has been applied over
  // the top of the definition's default palette.
  std::string theme_;
  std::vector<std::string> theme_names_;
  // The picked colour and what it derives to. Kept across reloads for the
  // same reason the theme name is: a hot edit of avatar.json must not lose the
  // colour the user chose in the picker, and the two never fight because they
  // live in different files — the art in avatar.json, the pick in
  // settings.json — and the derived palette is applied *after* every load.
  std::uint32_t custom_body_ = 0;
  bool custom_set_ = false;
  AvatarDerivedPalette derived_;

  // Applies derived_ over def_.palette and rewrites the frames. The one place
  // the derived theme becomes visible; called from set_custom_colour, from
  // set_theme and from the tail of every reload.
  void apply_custom();

  // M7.1. What each multi-member trigger played last, by trigger name, so the
  // no-immediate-repeat rule survives a clip being left and come back to.
  // Keyed by name rather than by index because a hot reload renumbers the
  // clips but not what the user just watched; cleared on reload all the same
  // when the group it refers to is gone.
  std::map<std::string, std::size_t> last_variant_;
  // Seeded from random_device, and *only ever consumed by a group of two or
  // more*: a definition with no variants draws no numbers from it.
  std::mt19937 rng_{std::random_device{}()};

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
