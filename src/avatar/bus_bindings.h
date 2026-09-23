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
//   toolbar.button -> ButtonRegistry::add_path_button or, with `run` instead
//                     of `path` (M33), add_run_button: the existing untrusted
//                     doors, validated paths or action names, no command
//                     strings, caps on count and label. `ButtonActionKind::
//                     Invoke` is not reachable from here and there is no verb
//                     that could produce one.
//   toolbar.clear  -> ButtonRegistry::clear_registered
//   script.status  -> the Scripts line in the settings surface (M2.6)
//   script.log     -> one [py] line in the app's log
//   schedule.create -> build_schedule() + ScheduleBook::create (M2b.2)
//   schedule.cancel -> VoiceSession::cancel_schedule, the AI's own two lookups
//   schedule.list   -> VoiceSession::pending_items, back out as events
//   session.*       -> the transport row (M2.9): toggle_mic, the panel's mute
//                      flag, stop(), reset(), say()
//   settings.*      -> the settings surface's own fields in AvatarUiState
//                      (M2.9), which is where every one of those controls
//                      writes and the only owner of record there is
//   worker.tell     -> VoiceSession::tell_worker -> WorkerPool::tell (M32):
//                      a note to a worker that is already running or has
//                      already finished, without starting a new one
//
// ## The controls a script can work (M2.9)
//
// The user asked that "all UI buttons are triggerable by Python". Two
// families, and a decision about shape that is worth stating: **there is no
// `button.press` verb and there never will be.**
//
//   * Half of these controls are *levels*, not presses. Mute, the model, the
//     tool grants, the language boxes, the listen timeout: the panel owns the
//     value and pushes it down every frame. "Press mute" has no idempotent
//     form — a script that retried it would unmute — whereas `session.mute
//     on=false` means the same thing however many times it arrives.
//   * A press-by-id verb would couple a script to the button *registry*, and
//     `ButtonActionKind::Invoke` is deliberately unreachable from the bus
//     (see on_toolbar). A verb that could press a registered button by name
//     would walk straight through that wall, because a worker and the
//     assistant can both register buttons.
//   * Every verb below lands on the call the control itself makes. `mic` is
//     `toggle_mic()`, the one path into the latch that the button, the SPACE
//     click and `--auto-listen` all share; `mute` and the whole `settings`
//     family write `AvatarUiState`, which is what the checkbox writes and what
//     main.cpp mirrors into `settings.json`. Nothing here is a second
//     implementation of anything, so nothing here can drift — including when
//     a setting starts restarting the child, which it will.
//
//   {"t":"session.mic","on":true}      {"t":"session.mute","on":true}
//   {"t":"session.stop"}               {"t":"session.reset"}
//   {"t":"session.say","text":"hello"} {"t":"session.get"}
//   {"t":"settings.model","name":"haiku"}
//   {"t":"settings.model_worker","name":"sonnet"}
//   {"t":"settings.tools","group":"file_write","on":true}
//   {"t":"settings.tools","group":"browser","on":true}
//   {"t":"settings.language","english":true,"japanese":false}
//   {"t":"settings.listen_timeout","seconds":45}
//   {"t":"settings.auto_listen","on":true}
//   {"t":"settings.chat","on":true}    {"t":"settings.open","on":true}
//   {"t":"settings.avatar_mode","value":"hidden"}
//   {"t":"settings.get"}
//
// and back out:
//
//   {"t":"session.muted","on":true}         {"t":"session.mic","open":true}
//   {"t":"session.said","ok":true,"echo":"x"}
//   {"t":"session.stopped","echo":"x"}
//   {"t":"session.resetting","ok":true,"echo":"x"}
//   {"t":"session.refused","verb":"say","reason":"the microphone is open"}
//   {"t":"session.info","state":"idle","mic":false,"muted":true,...}
//   {"t":"settings.changed","key":"model","value":"haiku","echo":"x"}
//   {"t":"settings.refused","key":"model","reason":"no such model: gpt"}
//   {"t":"settings.info","model":"haiku","tools":"web,file_read",...}
//
// **Levels answer as facts, discrete acts answer with `echo`.** `session.muted`
// and `session.mic` are published from `publish()` like `session.state` — on
// change, coalesced under their own key — because a level is a fact about the
// app and not a reply to anybody: the same event has to arrive whether the
// mute came from this script, from the button, from the S key or from another
// script, and an `echo` on it would be a lie three times out of four. Setting
// one *also* forces the fact out on that frame even when nothing changed, so a
// script that mutes an already-muted app still gets its answer rather than
// waiting forever. Everything else here — say, stop, reset, and every
// `settings` verb — is a request with an outcome, so it carries `echo` back
// exactly as `schedule.created` does.
//
// **`drain_events()` still empties the queue for everybody** and nothing here
// changes that. The two new facts are keyed, so they cost two slots however
// long nothing drains; the acks are keyless and capped like turn text.
//
// **Reset: the explicit call is the confirmation.** The transport row makes
// the user press twice because the button is 24 px wide, sits beside three
// others and cannot be undone — the second press is there to catch a slipped
// mouse. A script does not slip. `aii.reset()` is already a deliberate line
// someone wrote, and a two-message handshake on the bus would only mean every
// script carrying the same boilerplate. What a script *does* inherit is the
// part that is not about confirmation: `VoiceSession::reset()` refuses while
// one is already running, and refuses before the engines are up, so a script
// can no more fire two concurrently than a hand can.
//
// **What is deliberately not here.** Hold-to-dictate
// (`talk_pressed`/`talk_released`) is a gesture whose whole meaning is how
// long it lasted and whether the pointer was still on the button; a script has
// neither, and both of its outcomes are already reachable — a click latches
// the microphone (`session.mic`), a hold puts words in the field to be sent
// (`session.say`). Exposing the halves would mostly buy a script the ability
// to open the microphone and never close it. The sidebar's folder button, the
// prompt inspector and the worker windows open OS windows and an Explorer
// window, which is not app state and cannot be observed on the bus; quitting
// the app is not offered either, since `should_quit()` is the app's word to
// the script and not the other way round.
//
// **Adding a family is a data change.** `AppBus::add_family("schedule",
// handler)` and one function; M2.6's `script` family was exactly that — two
// lines here and nothing at all in `app_bus.*` — and M2b.2's was the same.
//
// ## The schedule family (M2b.2), and why it is called `schedule`
//
// `app_bus.h` sketched this family as `task`. It is `schedule`, because `task=`
// is already a *field* of a schedule — the instruction a deferred worker
// carries — and `{"t":"task.create","task":"..."}` would name two different
// things with one word on one line. Everything else in this feature is called
// schedule: the primitive, the book, the ```aii``` verb, the log prefix.
//
//   {"t":"schedule.create","in":"10m","say":"tea is ready"}
//   {"t":"schedule.create","in":"30m","task":"build main and say if it broke",
//    "cwd":"C:\\github\\AIInterface","name":"build","grade":"phrased"}
//   {"t":"schedule.cancel","id":3}
//   {"t":"schedule.list"}
//
// and back out:
//
//   {"t":"schedule.created","id":3,"kind":"timer","grade":"fixed","in":600.0}
//   {"t":"schedule.refused","reason":"could not read the delay"}
//   {"t":"schedule.cancelled","id":3,"ok":true}
//   {"t":"schedule.pending","id":3,"kind":"timer","label":"tea","in":540.0}
//   {"t":"schedule.list","count":1}
//   {"t":"schedule.fired","id":3,"kind":"timer","grade":"fixed"}
//
// **`grade=` is the point of this family, not an extra.** M2b.3 gives the
// conversational instance two *shapes* — `say=` or `task=`+`cwd=` — and keeps
// `grade=` out of its prompt on purpose, so it picks the report grade by
// answering a question about the request rather than by setting a label. A
// script has no shape to signal with and no register to be judged in, so it
// says the grade outright and `build_schedule()` honours it. A script can
// therefore express everything the ```aii``` verb can and one thing it cannot:
// a `Fixed` worker — do the work, then say exactly this — which
// `deliver_schedule()` was already written to accept.
//
// **The reply rides the one event queue like everything else.** `created`,
// `cancelled` and the `pending` rows are published, not returned, because
// there is no return path on a bus — and `AppBus::drain_events()` empties the
// queue for whoever calls it first. Two *scripts* are fine: `aii_pyhost`
// drains once and fans out to a per-thread cursor. A script racing `--bus-out`
// is not, and that is the bus's known shape, not this family's: nothing here
// makes it worse. An `echo` field is copied from the request onto `created`
// and `refused` so a script that shares the bus can recognise its own replies
// without matching on content.
//
// Everything with a lease on it — clip, sprite, cells — expires. A script that
// dies mid-performance leaves the avatar back under the C++ policy within
// `AvatarController::kScriptLeaseMax`, and the built-in policy is what runs
// when nothing is scripting, unchanged and authoritative.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "action_store.h"
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
    // M2b.2. Null on a run with no voice (`--no-voice`). Creating a schedule
    // still works without it — the book is its own singleton — but cancelling
    // one that has already started a worker, and listing those workers, are
    // the session's to answer, so both degrade to the book alone and say so.
    VoiceSession* session = nullptr;
    // --avatar pointed at a directory, or --clip/--sprite pinned the art. Both
    // mean the command line is holding the avatar for a look at it, and a
    // script must not quietly take it back.
    bool avatar_pinned = false;
    bool dir_override = false;
    // Whether turn text is published. Opt-in, as the plan says: the transcript
    // is the most sensitive thing this app holds and it does not leave the
    // process because a script happened to connect.
    bool publish_text = false;
    // M10.2. The authoritative set of actions, owned by the frame loop. The
    // `script.run` verb resolves a *name* against it and publishes the path;
    // nothing on the bus may name a path, which is what stops the untrusted
    // end introducing a file the app did not already find.
    ActionStore* actions = nullptr;
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

  // M2.6. One line about scripting for the settings surface — a script's own
  // `aii.status()`, the bootstrap's report of a script that raised, or the
  // app's own reason for not starting a host at all. It is deliberately the
  // same field for all three: from the user's side "my script isn't working"
  // has one answer, not three places to look for one.
  //
  // `aii.status()` is a bus message like any other, so it is applied on the
  // frame loop with everything else and a script cannot write into the panel
  // from its own thread.
  void set_script_status(std::string text, bool ok);
  const std::string& script_status() const { return script_status_; }
  bool script_status_ok() const { return script_status_ok_; }

  // Lines a script asked to have logged, since the last call. Frame loop.
  std::vector<std::string> take_script_log();

 private:
  void on_avatar(const BusMessage& m, std::string* error);
  void on_theme(const BusMessage& m, std::string* error);
  void on_toolbar(const BusMessage& m, std::string* error);
  void on_script(const BusMessage& m, std::string* error);
  void on_schedule(const BusMessage& m, std::string* error);
  // M2.9. The transport row and the settings surface.
  void on_session(const BusMessage& m, std::string* error);
  void on_settings(const BusMessage& m, std::string* error);
  // M32. `worker.tell` -- a note to a running or just-finished worker,
  // through VoiceSession::tell_worker() -> WorkerPool::tell().
  void on_worker(const BusMessage& m, std::string* error);
  // The two levels, published on change and whenever a verb touched one.
  void publish_facts();
  float lease_from(const BusMessage& m) const;

  Context ctx_;

  std::string script_status_;
  bool script_status_ok_ = true;
  std::vector<std::string> script_log_;

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
  // M2.9. -1 is "never published", so the first frame states both levels
  // rather than leaving a script to assume a default it cannot see.
  int last_muted_ = -1;
  int last_mic_ = -1;
  // A `session.mute`/`session.mic`/`session.get` landed this frame: say what
  // the level is even if it did not move.
  bool facts_now_ = false;
};

}  // namespace aii
