#pragma once
// The app's two ends of the bus (M2.5): what this app publishes, and what it
// lets a script do to it.
//
// `AppBus` in aii_core is transport — lines, bounds, routing by family name.
// This file is the only place that knows what a family *means*, and it is
// deliberately thin: every inbound verb lands on a door the app already has.
//
//   avatar.play    -> AvatarController::request_clip   (a lease, see there)
//   avatar.release -> AvatarController::release_clip
//   avatar.sprite  -> the status-sprite channel avatar_apply already takes
//   avatar.cells   -> a bounded cell overlay stamped after compose()
//   avatar.clear   -> drops the cell overlay
//   avatar.load    -> the panel's own avatar-by-name wish (AvatarUiState)
//   theme.set      -> AvatarSource::set_theme, by name (eae9061 built it for
//                     exactly this)
//   theme.colour   -> the colour picker's own wish, so a scripted colour and a
//                     dragged one are the same code path and persist alike
//   toolbar.button -> ButtonRegistry::add_path_button, the existing untrusted
//                     door: validated paths, no command strings, caps on count
//                     and label. `ButtonActionKind::Invoke` is not reachable
//                     from here and there is no verb that could produce one.
//   toolbar.clear  -> ButtonRegistry::clear_registered
//
// **Adding a family is a data change.** `AppBus::add_family("task", handler)`
// and one function; M2b's scheduled tasks are the next one and need nothing
// from the bus, the queues, the bounds or the parser.
//
// Everything with a lease on it — clip, sprite, cells — expires. A script that
// dies mid-performance leaves the avatar back under the C++ policy within
// `AvatarController::kScriptLeaseMax`, and the built-in policy is what runs
// when nothing is scripting, unchanged and authoritative.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "avatar_controller.h"
#include "avatar_def.h"
#include "avatar_ui.h"
#include "core/app_bus.h"
#include "voice_session.h"

namespace aii {

// How the bus is driven with no Python, which is the state of the world until
// M2.6 and the only way M2.5 can be tested at all.
//
// Two files, in the spirit of `--buttons` (which applies a file of ```aii```
// command lines at startup and was built as exactly this kind of escape
// hatch), and for the same reason: the paths that most need exercising are the
// ones a live turn can least be made to produce on demand.
//
//   --bus-in <file>    tailed while the app runs: every line appended to it is
//                      posted to the bus. Tailed rather than read once,
//                      because the half of this that needs watching is what
//                      happens *after* a message — a lease expiring, the
//                      microphone taking the avatar back — and a file read at
//                      startup cannot show either.
//   --bus-out <file>   every published event, appended as it is drained.
//
// The poll is on the frame loop at 10 Hz and reads only what was appended, so
// the frame cost is a stat and a short read; nothing here blocks on anything.
class BusFileHatch {
 public:
  // Either path may be empty. Returns false with `error` if a file could not
  // be opened — a hatch that silently did not open would be indistinguishable
  // from a bus that did not work.
  bool open(const std::string& in_path, const std::string& out_path, std::string* error);
  void poll(float dt);

 private:
  std::string in_path_;
  std::uint64_t in_at_ = 0;
  std::string partial_;
  std::string out_path_;
  bool out_ok_ = false;
  float wait_ = 0.0f;
};

class BusBindings {
 public:
  struct Context {
    AvatarSource* source = nullptr;
    AvatarController* controller = nullptr;
    AvatarUiState* ui = nullptr;
    // --avatar pointed at a directory, or --clip/--sprite pinned the art. Both
    // mean the command line is holding the avatar for a look at it, and a
    // script must not quietly take it back.
    bool avatar_pinned = false;
    bool dir_override = false;
    // Whether turn text is published. Opt-in, as the plan says: the transcript
    // is the most sensitive thing this app holds and it does not leave the
    // process because a script happened to connect.
    bool publish_text = false;
  };

  void install(Context ctx);

  // Frame loop, after AppBus::apply_pending(): ages the leases.
  void tick(float dt);

  // The accessory a script is holding up, or nullptr. Goes into
  // `avatar_apply`'s `status_sprite`, *below* the muted bubble: a condition the
  // user cannot otherwise see outranks a decoration a script asked for.
  const char* script_sprite() const;

  // Stamps the cell overlay into the composed frame. Called after
  // AvatarSource::compose() and before the grid is written to the GPU, which
  // is the only point at which a cell write cannot tear a frame.
  void stamp_cells(AvatarGrid& grid) const;

  // Publishes everything the app has to say about this frame, coalesced and
  // rate-limited. Frame loop.
  void publish(const VoiceSession::Snapshot& snap, float dt);

 private:
  void on_avatar(const BusMessage& m, std::string* error);
  void on_theme(const BusMessage& m, std::string* error);
  void on_toolbar(const BusMessage& m, std::string* error);
  float lease_from(const BusMessage& m) const;

  Context ctx_;

  std::string sprite_;
  float sprite_left_ = 0.0f;

  struct Cell {
    std::uint32_t x = 0, y = 0, rgba = 0;
    bool overlay = true;
  };
  std::vector<Cell> cells_;
  float cells_left_ = 0.0f;

  // What was published last, so only changes go out.
  std::string last_state_;
  float level_wait_ = 0.0f;
  int last_mic_q_ = -1;
  int last_speak_q_ = -1;
  double last_ctx_ = -2.0, last_session_ = -2.0, last_week_ = -2.0;
  std::vector<std::pair<std::string, std::string>> last_workers_;
  std::size_t last_lines_ = 0;
};

}  // namespace aii
