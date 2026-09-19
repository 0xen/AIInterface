#include "bus_bindings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "core/button_registry.h"
#include "core/language.h"
#include "core/model_choice.h"
#include "core/schedule.h"
#include "core/tool_policy.h"
#include "core/worker_pool.h"

namespace aii {
namespace {

// The most cells a script may hold at once. The grid's own maximum, so a
// script can in principle drive every cell of the stage, and not one more.
constexpr std::size_t kCellsMax = kAvatarMaxCells;

// How long a sprite or a cell overlay is held when the message does not say.
// A sprite has no authored length to fall back on the way a clip does, so it
// gets the ceiling: long enough to be useful, short enough that a dead script
// cannot leave a thought bubble over the avatar for the rest of the session.
constexpr float kDecorLeaseDefault = AvatarController::kScriptLeaseMax;

bool colour_from_hex(const std::string& text, std::uint32_t& out) {
  std::string s = text;
  if (!s.empty() && s.front() == '#') s.erase(s.begin());
  if (s.size() != 6) return false;
  std::uint32_t v[6]{};
  for (std::size_t i = 0; i < 6; ++i) {
    const char c = s[i];
    if (c >= '0' && c <= '9') v[i] = static_cast<std::uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f') v[i] = static_cast<std::uint32_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v[i] = static_cast<std::uint32_t>(c - 'A' + 10);
    else return false;
  }
  out = avatar_rgba(static_cast<std::uint8_t>(v[0] * 16 + v[1]),
                    static_cast<std::uint8_t>(v[2] * 16 + v[3]),
                    static_cast<std::uint8_t>(v[4] * 16 + v[5]), 255);
  return true;
}

// An avatar name is a directory name under %APPDATA%\AIInterface\avatars, and
// a script does not get to say where that is. Letters, digits, dash and
// underscore: no separators, no dots, so there is no `..` to reason about.
bool safe_name(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (const unsigned char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

}  // namespace

void BusBindings::install(Context ctx) {
  ctx_ = ctx;
  AppBus& bus = AppBus::instance();
  bus.add_family("avatar", [this](const BusMessage& m, std::string* e) { on_avatar(m, e); });
  bus.add_family("theme", [this](const BusMessage& m, std::string* e) { on_theme(m, e); });
  bus.add_family("toolbar", [this](const BusMessage& m, std::string* e) { on_toolbar(m, e); });
  // M2.6. The one family that points back at the user rather than at the app:
  // it is how a script says something, and how a script that fell over is
  // something the user can see rather than a line in a console they are not
  // watching.
  bus.add_family("script", [this](const BusMessage& m, std::string* e) { on_script(m, e); });
  // M2b.2. Scheduling without the conversational instance: the family the
  // ```aii``` verb's `grade=` was parsed and then withheld for.
  bus.add_family("schedule", [this](const BusMessage& m, std::string* e) { on_schedule(m, e); });
  // M2.9. The transport row and the settings surface — every control the user
  // can work with the pointer, worked by the same calls the controls make.
  bus.add_family("session", [this](const BusMessage& m, std::string* e) { on_session(m, e); });
  bus.add_family("settings", [this](const BusMessage& m, std::string* e) { on_settings(m, e); });
}

float BusBindings::lease_from(const BusMessage& m) const {
  const double hold = m.num("hold", 0.0);
  if (!(hold > 0.0)) return 0.0f;  // 0 means "the default for this verb"
  return static_cast<float>(std::min(hold, static_cast<double>(AvatarController::kScriptLeaseMax)));
}

// ------------------------------------------------------------------ avatar

void BusBindings::on_avatar(const BusMessage& m, std::string* error) {
  if (!ctx_.controller || !ctx_.source) {
    if (error) *error = "no avatar in this run";
    return;
  }
  if (m.verb == "play") {
    if (ctx_.avatar_pinned) {
      if (error) *error = "the command line pinned the avatar";
      return;
    }
    ctx_.controller->request_clip(m.str("clip"), lease_from(m), error);
    return;
  }
  if (m.verb == "release") {
    ctx_.controller->release_clip();
    sprite_.clear();
    sprite_left_ = 0.0f;
    return;
  }
  if (m.verb == "sprite") {
    const std::string name = m.str("name");
    const bool on = m.flag("on", true);
    if (!on) {
      if (name.empty() || name == sprite_) {
        sprite_.clear();
        sprite_left_ = 0.0f;
      }
      return;
    }
    // Checked against the definition rather than taken on trust, so a typo is
    // one log line now instead of an accessory that never appears and no
    // explanation of why.
    bool known = false;
    for (const AvatarSprite& s : ctx_.source->definition().sprites) known = known || s.name == name;
    if (!known) {
      if (error) *error = "no such sprite: " + name;
      return;
    }
    sprite_ = name;
    const float lease = lease_from(m);
    sprite_left_ = lease > 0.0f ? lease : kDecorLeaseDefault;
    return;
  }
  if (m.verb == "cells") {
    const std::vector<double>* v = m.nums("cells");
    if (!v || v->size() < 3) {
      if (error) *error = "cells: expected [x,y,rgba,...]";
      return;
    }
    const bool overlay = m.str("layer", "overlay") != "base";
    if (!m.flag("add", false)) cells_.clear();
    std::size_t clamped = 0;
    for (std::size_t i = 0; i + 2 < v->size(); i += 3) {
      if (cells_.size() >= kCellsMax) {
        ++clamped;
        continue;
      }
      const double x = (*v)[i], y = (*v)[i + 1], c = (*v)[i + 2];
      // Bounds are checked again at stamp time against the live grid, because
      // the stage is sized from the band and the band changes with the window.
      // This one only keeps the store finite and the casts defined.
      if (x < 0 || y < 0 || x >= kAvatarMaxGrid || y >= kAvatarMaxGrid) {
        ++clamped;
        continue;
      }
      Cell cell;
      cell.x = static_cast<std::uint32_t>(x);
      cell.y = static_cast<std::uint32_t>(y);
      cell.rgba = static_cast<std::uint32_t>(
          std::clamp(c, 0.0, static_cast<double>(0xFFFFFFFFu)));
      cell.overlay = overlay;
      cells_.push_back(cell);
    }
    const float lease = lease_from(m);
    cells_left_ = lease > 0.0f ? lease : kDecorLeaseDefault;
    if (clamped && error) *error = std::to_string(clamped) + " cell(s) out of range, dropped";
    return;
  }
  if (m.verb == "clear") {
    cells_.clear();
    cells_left_ = 0.0f;
    return;
  }
  if (m.verb == "load") {
    if (ctx_.dir_override || !ctx_.ui) {
      if (error) *error = "the command line pinned the avatar directory";
      return;
    }
    const std::string name = m.str("name");
    if (!safe_name(name)) {
      if (error) *error = "bad avatar name";
      return;
    }
    // Written as the panel's own wish rather than opened here. main.cpp's
    // existing block is what turns a wish into a load — it seeds %APPDATA%,
    // keeps --clip/--sprite, re-measures the controller's clip lengths and
    // persists the name — and a second opener would be a second copy of all
    // of that, drifting.
    ctx_.ui->avatar_name = name;
    return;
  }
  if (error) *error = "unknown verb";
}

// ------------------------------------------------------------------- theme

void BusBindings::on_theme(const BusMessage& m, std::string* error) {
  if (!ctx_.source || !ctx_.ui) {
    if (error) *error = "no avatar in this run";
    return;
  }
  if (m.verb == "set") {
    const std::string name = m.str("name");
    // Validated against what the art declares, by name — the narrow door
    // set_theme() was built to be. Refusing here rather than letting the wish
    // through means a bad name never reaches the settings file.
    const std::vector<std::string>& have = ctx_.source->themes();
    if (std::find(have.begin(), have.end(), name) == have.end()) {
      if (error) *error = "no such theme: " + name;
      return;
    }
    ctx_.ui->theme = name;
    return;
  }
  if (m.verb == "colour" || m.verb == "color") {
    std::uint32_t rgba = 0;
    if (!colour_from_hex(m.str("value"), rgba)) {
      if (error) *error = "colour: expected #rrggbb";
      return;
    }
    ctx_.ui->custom_colour[0] = static_cast<float>(avatar_r(rgba)) / 255.0f;
    ctx_.ui->custom_colour[1] = static_cast<float>(avatar_g(rgba)) / 255.0f;
    ctx_.ui->custom_colour[2] = static_cast<float>(avatar_b(rgba)) / 255.0f;
    ctx_.ui->custom_colour_changed = true;
    // The picked colour is only *visible* under the derived theme, so a script
    // that sets one and sees nothing change would be right to call that a bug.
    ctx_.ui->theme = AvatarSource::custom_theme();
    return;
  }
  if (error) *error = "unknown verb";
}

// ----------------------------------------------------------------- toolbar

void BusBindings::on_toolbar(const BusMessage& m, std::string* error) {
  if (m.verb == "button") {
    // Straight at the existing untrusted door. Every bound that matters — the
    // path must exist and be a directory, the label length, the count, the
    // closed set of action kinds — is enforced there, once, for this and for
    // the assistant's ```aii``` block alike. There is deliberately no verb here
    // that could produce a ButtonActionKind::Invoke.
    ButtonRegistry::instance().add_path_button(m.str("id"), m.str("label"), m.str("tip"),
                                               m.str("path"), error);
    return;
  }
  if (m.verb == "clear") {
    ButtonRegistry::instance().clear_registered();
    return;
  }
  if (error) *error = "unknown verb";
}

// ------------------------------------------------------------------ script

void BusBindings::on_script(const BusMessage& m, std::string* error) {
  if (m.verb == "status") {
    // A failure replaces a success but not another failure: when two scripts
    // are running, the one that broke is the news, and a healthy one
    // reporting afterwards must not paper over it.
    const bool ok = m.flag("ok", true);
    if (ok && !script_status_ok_) return;
    set_script_status(m.str("text"), ok);
    // Also into the log. The settings surface is where the user finds it; the
    // log is where a scripted run finds it, and a status line that only
    // existed behind a cog would be untestable without a screenshot.
    if (script_log_.size() < kBusStatusMax)
      script_log_.push_back((ok ? "status: " : "status (failed): ") + script_status_);
    return;
  }
  if (m.verb == "log") {
    // Bounded like everything else here. A script logging in a tight loop
    // fills a frame's worth and no more, because this is drained every frame.
    if (script_log_.size() < kBusStatusMax) script_log_.push_back(m.str("text"));
    return;
  }
  if (error) *error = "unknown verb";
}

// ---------------------------------------------------------------- schedule

// M2b.2. Scheduling from a script, without the conversational instance.
//
// Every verb here runs inside `AppBus::apply_pending()`, which is frame loop
// only and, in main.cpp, runs *earlier in the same frame* than
// `ScheduleBook::tick()` and `VoiceSession::apply_cancels()`. That ordering is
// what makes a cancel from here safe without a second queue: a schedule is
// either still in the book when this looks, or it fired on an earlier frame
// and is already recorded as a running worker. It is never in neither place,
// which is M2b.5's argument and this call simply stands inside it.
//
// Nothing here invents a privilege. `create` goes through `build_schedule()`,
// the same mapping — and the same absolute-`cwd` refusal — the ```aii``` verb
// uses; `cancel` goes through the session's own two lookups. The one thing a
// script may do that the model may not is state `grade=`, which is exactly what
// M2b.3 held back for it.
void BusBindings::on_schedule(const BusMessage& m, std::string* error) {
  AppBus& bus = AppBus::instance();
  // Copied onto the replies so a script sharing the bus with another can pick
  // its own out. Never interpreted, never stored on the schedule.
  const std::string echo = m.str("echo");

  if (m.verb == "create") {
    ScheduleRequest req;
    req.in = m.str("in");
    req.say = m.str("say");
    req.task = m.str("task");
    req.cwd = m.str("cwd");
    req.name = m.str("name");
    req.label = m.str("label");
    req.grade = m.str("grade");
    ScheduleAction action;
    ReportGrade grade = ReportGrade::Fixed;
    double seconds = 0.0;
    std::string detail;
    const ScheduleRefusal why = build_schedule(req, &action, &grade, &seconds, &detail);
    std::string reason = why == ScheduleRefusal::None ? std::string() : to_string(why);
    std::uint64_t id = 0;
    if (reason.empty()) {
      std::string err;
      id = ScheduleBook::instance().create(std::chrono::duration<double>(seconds), action, grade,
                                           &err);
      // The book's own refusal: full at kSchedulesMax. A script in a loop is
      // the realistic way to reach it, so this is the one a script most needs
      // to hear about, and it hears about it as data rather than as silence.
      if (id == 0) reason = err.empty() ? "the schedule book refused it" : err;
    }
    if (!reason.empty()) {
      // A refused *script* schedule is logged and published, never spoken.
      // M2b.3's refusals are spoken because the model has already promised the
      // user a timer out loud by the time the block runs; a script has made no
      // promise to anyone, and the app announcing another program's mistake is
      // the toolbar family's rule too.
      // No verb prefix: AppBus's own status line already carries `family.verb`.
      if (error) *error = reason + (detail.empty() ? "" : " (" + detail + ")");
      bus.publish(BusLine("schedule.refused")
                      .str("reason", reason)
                      .str("detail", detail)
                      .str("echo", echo)
                      .done());
      return;
    }
    if (script_log_.size() < kBusStatusMax) {
      char when[32];
      std::snprintf(when, sizeof when, "%.1fs", seconds);
      script_log_.push_back("scheduled id=" + std::to_string(id) + " kind=" + action.kind +
                            " grade=" + to_string(grade) + " in " + when);
    }
    bus.publish(BusLine("schedule.created")
                    .num("id", static_cast<double>(id), 0)
                    .str("kind", action.kind)
                    .str("grade", to_string(grade))
                    .num("in", seconds)
                    .str("label", action.label)
                    .str("echo", echo)
                    .done());
    return;
  }

  if (m.verb == "cancel") {
    const std::uint64_t id = static_cast<std::uint64_t>(m.num("id", 0.0));
    bool ok = false;
    if (id == 0) {
      if (error) *error = "no id";
    } else if (ctx_.session) {
      ok = ctx_.session->cancel_schedule(id);
    } else {
      // No session in this run, so there are no schedule-started workers to
      // look in either. The book is the whole of the truth here.
      ok = ScheduleBook::instance().cancel(id);
    }
    bus.publish(BusLine("schedule.cancelled")
                    .num("id", static_cast<double>(id), 0)
                    .flag("ok", ok)
                    .str("echo", echo)
                    .done());
    return;
  }

  if (m.verb == "list") {
    // One event per item and then a count, so a script knows when it has the
    // whole answer without counting on the order of an empty list. `count`
    // last rather than first for exactly that: a list of zero is one event,
    // not a promise of rows that never arrive.
    std::size_t n = 0;
    if (ctx_.session) {
      for (const VoiceSession::PendingItem& it : ctx_.session->pending_items()) {
        bus.publish(BusLine("schedule.pending")
                        .num("id", static_cast<double>(it.id), 0)
                        .str("kind", it.kind)
                        .str("label", it.label)
                        .str("grade", it.phrased ? "phrased" : "fixed")
                        .num("in", it.seconds, 1)
                        .str("echo", echo)
                        .done());
        ++n;
      }
    } else {
      const auto now = std::chrono::steady_clock::now();
      for (const Schedule& s : ScheduleBook::instance().list()) {
        bus.publish(BusLine("schedule.pending")
                        .num("id", static_cast<double>(s.id), 0)
                        .str("kind", s.action.kind)
                        .str("label", s.action.label)
                        .str("grade", to_string(s.grade))
                        .num("in", s.seconds_until(now), 1)
                        .str("echo", echo)
                        .done());
        ++n;
      }
    }
    bus.publish(
        BusLine("schedule.list").num("count", static_cast<double>(n), 0).str("echo", echo).done());
    return;
  }

  if (error) *error = "unknown verb";
}

// ----------------------------------------------------------------- session

// M2.9. The transport row from a script.
//
// Every verb here is the call the control itself makes, and that is the whole
// design. `mic` is `toggle_mic()`, which the header calls the one path into
// the latch — the microphone button's short click, the SPACE click and
// `--auto-listen` all go through it. `mute` writes `AvatarUiState::muted`,
// which is what the button and the S key write; main.cpp pushes it into the
// session later in this same frame and mirrors it into `settings.json`, so a
// scripted mute is remembered exactly as a clicked one is. `stop` and `reset`
// are the session's own, and `say` is the message field's.
//
// The refusals are the panel's refusals, not new ones. `send_refusal()` was
// file-static in avatar_ui.cpp until this family existed; it is in the header
// now so the field and this verb answer "why did nothing happen" with the same
// sentence. Reset's two guards are the session's — `resetting` and
// `resettable` — so a script can no more start two resets at once, or reset an
// empty conversation, than the button can.
void BusBindings::on_session(const BusMessage& m, std::string* error) {
  AppBus& bus = AppBus::instance();
  const std::string echo = m.str("echo");
  const auto refuse = [&](const std::string& reason) {
    // A refused control is logged and published, never spoken. The toolbar
    // family's rule and the schedule family's: the app does not announce
    // another program's mistake in the user's ear.
    if (error) *error = reason;
    bus.publish(BusLine("session.refused")
                    .str("verb", m.verb)
                    .str("reason", reason)
                    .str("echo", echo)
                    .done());
  };

  if (m.verb == "mute") {
    if (!ctx_.ui) return refuse("no panel in this run");
    // `on` omitted means flip it, which is what the button does. Given, it is
    // a level and idempotent — the form a script should be using.
    ctx_.ui->muted = m.has("on") ? m.flag("on", true) : !ctx_.ui->muted;
    facts_now_ = true;
    return;
  }

  if (m.verb == "mic") {
    if (!ctx_.session) return refuse("no voice in this run");
    const bool want = m.has("on") ? m.flag("on", true) : !ctx_.session->mic_open();
    if (want != ctx_.session->mic_open()) ctx_.session->toggle_mic();
    facts_now_ = true;
    // `set_mic_open()` declines in silence while a reset is running and before
    // the engines are up. Silence is right for a button that is drawn disabled
    // at the same moment; a script cannot see the button, so it is told.
    if (want && !ctx_.session->mic_open()) return refuse("the microphone cannot open yet");
    return;
  }

  if (m.verb == "stop") {
    if (!ctx_.session) return refuse("no voice in this run");
    ctx_.session->stop();
    facts_now_ = true;  // stop drops the latch, and that is a fact worth saying
    bus.publish(BusLine("session.stopped").str("echo", echo).done());
    return;
  }

  if (m.verb == "reset") {
    if (!ctx_.session) return refuse("no voice in this run");
    const VoiceSession::Snapshot snap = ctx_.session->snapshot();
    if (snap.resetting) return refuse("a reset is already running");
    if (!snap.resettable) return refuse("there is nothing to reset");
    ctx_.session->reset();
    facts_now_ = true;
    // `ok` is "accepted", not "finished": reset() returns straight away and
    // does the teardown on its own thread. A script waits for `session.state`
    // to come back, the same way the window does.
    bus.publish(BusLine("session.resetting").flag("ok", true).str("echo", echo).done());
    return;
  }

  // M3.15. The script half of the handover, and the whole of it that could be
  // built honestly today.
  //
  // **What a script already has.** `session.usage` publishes `ctx` — the CLI's
  // own context fraction — on every change, and has since M2.9. So the policy
  // half of "hand over at 40%" has been scriptable all along; what was missing
  // was the *act*, and this is it. A Python script that wants a different rule
  // (hand over at 30% after 9pm, never mid-task, only when the user has been
  // quiet for a minute) writes eight lines against two messages it can already
  // see, and does not need a line of C++.
  //
  // **Why `session` and not `settings`.** Handing over is not a setting: it is
  // a thing the session does, like `reset` above it, and it is refused under
  // the same two conditions by the same snapshot. The *threshold* is a setting
  // and is deliberately not exposed here — it lives in `settings.json` under
  // `handoff.threshold`, and a verb that wrote it would be a second owner of
  // record for a value the panel does not yet draw a control for.
  //
  // `ok` is "accepted", not "finished", and it means less here than it does
  // for `reset`: this arms a sequence that waits for a settled moment, speaks,
  // spends a turn and only then restarts. A script watches `session.state` the
  // way the window does.
  if (m.verb == "handoff") {
    if (!ctx_.session) return refuse("no voice in this run");
    const VoiceSession::Snapshot snap = ctx_.session->snapshot();
    if (snap.resetting) return refuse("a restart is already running");
    if (!ctx_.session->handoff_now()) return refuse("a handover is already under way");
    facts_now_ = true;
    bus.publish(BusLine("session.handoff").flag("ok", true).str("echo", echo).done());
    return;
  }

  if (m.verb == "say") {
    if (!ctx_.session) return refuse("no voice this run (--no-voice)");
    const std::string text = m.str("text");
    const VoiceSession::Snapshot snap = ctx_.session->snapshot();
    if (const char* why = send_refusal(snap, true, text.c_str())) return refuse(why);
    ctx_.session->say(text);
    bus.publish(BusLine("session.said").flag("ok", true).str("text", text).str("echo", echo).done());
    return;
  }

  if (m.verb == "get") {
    facts_now_ = true;  // and the two levels, so `get` answers in full
    BusLine line("session.info");
    if (ctx_.session) {
      const VoiceSession::Snapshot snap = ctx_.session->snapshot();
      line.str("state", VoiceSession::state_name(snap.state))
          .flag("mic", ctx_.session->mic_open())
          .flag("resetting", snap.resetting)
          .flag("resettable", snap.resettable)
          .str("status", snap.status);
    } else {
      line.str("state", "none").flag("mic", false);
    }
    line.flag("muted", ctx_.ui ? ctx_.ui->muted : (ctx_.session && ctx_.session->muted()));
    bus.publish(line.str("echo", echo).done());
    return;
  }

  if (error) *error = "unknown verb";
}

// ---------------------------------------------------------------- settings

// M2.9. The settings surface from a script.
//
// Every one of these writes the field the control writes, in `AvatarUiState`,
// and then stops. That is deliberately all: `AvatarUiState` is the owner of
// record for these values, and main.cpp is the one place that mirrors them
// into `settings.json` and pushes the live ones into the session. Writing the
// file here, or calling a setter here, would make a scripted change and a
// clicked change two different things — and two settings that a script could
// hold at odds with what the panel is drawing.
//
// It also means a script inherits whatever a control's field comes to mean.
// `model` and the tool grants are written to disk and reach the `claude` child
// when it next starts, which is what the surface says in amber under them
// today; on the day a change restarts the child instead, it will restart it
// for a script too, because there is nothing here that would have to be
// told.
void BusBindings::on_settings(const BusMessage& m, std::string* error) {
  AppBus& bus = AppBus::instance();
  const std::string echo = m.str("echo");
  const auto refuse = [&](const std::string& reason) {
    if (error) *error = reason;
    bus.publish(BusLine("settings.refused")
                    .str("key", m.verb)
                    .str("reason", reason)
                    .str("echo", echo)
                    .done());
  };
  // One event for the whole family rather than one per verb: these are all the
  // same act — set this field to this value — and `key`/`value` says which,
  // where `schedule.created` and `schedule.cancelled` are genuinely different
  // outcomes and earn their own names.
  const auto changed = [&](const std::string& value) {
    bus.publish(BusLine("settings.changed")
                    .str("key", m.verb)
                    .str("value", value)
                    .str("echo", echo)
                    .done());
  };
  const auto on_off = [](bool b) { return std::string(b ? "on" : "off"); };

  if (!ctx_.ui) return refuse("no panel in this run");
  AvatarUiState& ui = *ctx_.ui;

  if (m.verb == "model") {
    const std::string name = m.str("name");
    // By the `settings.json` key, which is the stable on-disk spelling and the
    // one a person hand-editing the file already knows. A name this build has
    // never heard of is refused rather than written: an unknown `--model`
    // starts a child in which every turn fails, which is the failure the
    // picker exists to make unreachable.
    const int idx = model_choice_for_key(name);
    if (idx < 0) return refuse("no such model: " + name);
    ui.model = idx;
    return changed(model_choice(idx).key);
  }

  if (m.verb == "tools") {
    const std::string group = m.str("group");
    int id = -1;
    for (int i = 0; i < kToolGroupCount; ++i)
      if (group == tool_group(i).key) id = i;
    if (id < 0) return refuse("no such tool group: " + group);
    // The surface draws an unoffered group disabled; a script gets the same
    // answer as a word, so a hand-written line cannot grant what the panel
    // refuses to offer.
    if (!tool_group(id).offered) return refuse("that group is not offered yet");
    ui.tools.on[id] = m.flag("on", true);
    return changed(group + "=" + on_off(ui.tools.on[id]));
  }

  if (m.verb == "language") {
    LanguageSelection sel{m.flag("english", ui.lang_english), m.flag("japanese", ui.lang_japanese)};
    // The checkboxes lock the last one on rather than letting it be cleared.
    // The same invariant, said rather than drawn.
    if (!sel.english && !sel.japanese) return refuse("at least one language has to stay on");
    ui.lang_english = sel.english;
    ui.lang_japanese = sel.japanese;
    return changed(language_spec(sel));
  }

  if (m.verb == "listen_timeout") {
    const double s = m.num("seconds", 0.0);
    // 0 is never, which is the spelling the mechanism, the config and the file
    // all share. The rest is the DragInt's own clamp.
    if (s < 0.0) return refuse("seconds cannot be negative");
    if (s > 0.0 && (s < 15.0 || s > 600.0)) return refuse("15 to 600 seconds, or 0 for never");
    ui.listen_timeout_on = s > 0.0;
    if (s > 0.0) ui.listen_timeout_sec = static_cast<int>(s);
    return changed(std::to_string(static_cast<int>(s)));
  }

  if (m.verb == "auto_listen") {
    ui.auto_listen = m.flag("on", true);
    return changed(on_off(ui.auto_listen));
  }

  if (m.verb == "chat") {
    ui.chat_open = m.flag("on", true);
    return changed(on_off(ui.chat_open));
  }

  if (m.verb == "open") {
    ui.settings_open = m.flag("on", true);
    return changed(on_off(ui.settings_open));
  }

  if (m.verb == "avatar_mode") {
    const std::string v = m.str("value");
    for (int i = 0; i < kAvatarVisibilityCount; ++i) {
      if (v == kAvatarVisibilityNames[i]) {
        ui.avatar_mode = static_cast<AvatarVisibility>(i);
        return changed(v);
      }
    }
    return refuse("no such avatar mode: " + v);
  }

  if (m.verb == "get") {
    // Read off the panel, not off the file: the panel is the owner of record
    // and the file is written from it, so this is the value that is in force
    // even on the frame before the debounced write has happened.
    bus.publish(
        BusLine("settings.info")
            .str("model", model_choice(ui.model).key)
            .str("tools", tool_list(ui.tools))
            .str("language", language_spec({ui.lang_english, ui.lang_japanese}))
            .flag("auto_listen", ui.auto_listen)
            .num("listen_timeout", listen_timeout_seconds(ui), 0)
            .str("avatar", ui.avatar_name)
            .str("theme", ui.theme)
            .str("avatar_mode", kAvatarVisibilityNames[static_cast<int>(ui.avatar_mode)])
            .flag("chat", ui.chat_open)
            .flag("settings_open", ui.settings_open)
            .str("echo", echo)
            .done());
    return;
  }

  if (error) *error = "unknown verb";
}

void BusBindings::set_script_status(std::string text, bool ok) {
  script_status_ = std::move(text);
  script_status_ok_ = ok;
}

std::vector<std::string> BusBindings::take_script_log() {
  std::vector<std::string> out;
  out.swap(script_log_);
  return out;
}

// -------------------------------------------------------------- the frame

void BusBindings::tick(float dt) {
  if (sprite_left_ > 0.0f && (sprite_left_ -= dt) <= 0.0f) {
    sprite_.clear();
    sprite_left_ = 0.0f;
  }
  if (cells_left_ > 0.0f && (cells_left_ -= dt) <= 0.0f) {
    cells_.clear();
    cells_left_ = 0.0f;
  }
}

const char* BusBindings::script_sprite() const {
  return sprite_left_ > 0.0f && !sprite_.empty() ? sprite_.c_str() : nullptr;
}

void BusBindings::stamp_cells(AvatarGrid& grid) const {
  for (const Cell& c : cells_) {
    if (c.x >= grid.width || c.y >= grid.height) continue;
    grid.set(c.overlay ? AvatarLayer::Overlay : AvatarLayer::Base, c.x, c.y, c.rgba);
  }
}

// ----------------------------------------------------------------- publish

// M2.9. The two levels of the transport row, as facts rather than as replies.
//
// They are published from here — on change, coalesced under their own key,
// exactly like `session.state` — and not from the verbs that set them, because
// a level is a fact about the app and not an answer to anybody. The mute may
// have come from this script, from the button, from the S key or from a second
// script, and the same event has to arrive in all four cases; an `echo` on it
// would be a lie three times out of four.
//
// `facts_now_` is what a verb leaves behind, and it is the whole of why a
// script does not have to guess: muting an app that was already muted moves
// nothing, so on-change alone would leave the caller waiting for an event that
// is never coming. One frame later it gets the level anyway.
//
// Two keyed events, so an unread queue costs two slots however long nothing
// drains it. `drain_events()` still empties the queue for everybody and
// nothing here changes that.
void BusBindings::publish_facts() {
  AppBus& bus = AppBus::instance();
  // The panel is the owner of record for mute, so that is where it is read —
  // `set_muted()` is not called until later in this frame, and reading the
  // session would report the value from before the verb landed.
  const int muted = ctx_.ui ? (ctx_.ui->muted ? 1 : 0)
                            : (ctx_.session && ctx_.session->muted() ? 1 : 0);
  const int mic = (ctx_.session && ctx_.session->mic_open()) ? 1 : 0;
  if (facts_now_ || muted != last_muted_) {
    last_muted_ = muted;
    bus.publish(BusLine("session.muted").flag("on", muted != 0).done(), "session.muted");
  }
  if (facts_now_ || mic != last_mic_) {
    last_mic_ = mic;
    bus.publish(BusLine("session.mic").flag("open", mic != 0).done(), "session.mic");
  }
  facts_now_ = false;
}

void BusBindings::publish(const VoiceSession::Snapshot& snap, float dt) {
  AppBus& bus = AppBus::instance();
  publish_facts();

  // Everything here is published from the frame loop, off the Snapshot the
  // frame loop already took, rather than from inside VoiceSession. That keeps
  // the rule announce() sets — the turn thread and the worker poll queue, the
  // frame loop acts — without adding a second publisher to either of them, and
  // it means M2.5 changes no line of voice_session.cpp.
  const char* state = VoiceSession::state_name(snap.state);
  if (last_state_ != state) {
    last_state_ = state;
    bus.publish(BusLine("session.state").str("value", state).done(), "session.state");
  }

  // Levels: quantised and rate-limited. At frame rate this is the one signal
  // that could fill a queue on its own, and a mic level is not meaningful to
  // 1/1000th — 1/64 at 10 Hz is more than a reaction needs and two orders of
  // magnitude less traffic.
  level_wait_ -= dt;
  if (level_wait_ <= 0.0f) {
    level_wait_ = 0.1f;
    const int mic = static_cast<int>(std::clamp(snap.mic_level, 0.0f, 1.0f) * 64.0f);
    const int spk = static_cast<int>(std::clamp(snap.speak_level, 0.0f, 1.0f) * 64.0f);
    if (mic != last_mic_q_ || spk != last_speak_q_) {
      last_mic_q_ = mic;
      last_speak_q_ = spk;
      bus.publish(BusLine("session.level")
                      .num("mic", mic / 64.0, 3)
                      .num("speaker", spk / 64.0, 3)
                      .done(),
                  "session.level");
    }
  }

  const auto moved = [](double a, double b) { return std::fabs(a - b) >= 0.005; };
  const UsageStats& u = snap.usage_stats;
  if (moved(u.ctx, last_ctx_) || moved(u.session, last_session_) || moved(u.week, last_week_)) {
    last_ctx_ = u.ctx;
    last_session_ = u.session;
    last_week_ = u.week;
    bus.publish(BusLine("session.usage")
                    .num("ctx", u.ctx)
                    .num("session", u.session)
                    .num("week", u.week)
                    .done(),
                "session.usage");
  }

  // One event per worker per state change, keyed by name so an unread queue
  // holds the current state of each worker rather than its history.
  for (const WorkerPool::Snapshot& w : snap.workers) {
    const char* ws = worker_state_name(w.state);
    auto it = std::find_if(last_workers_.begin(), last_workers_.end(),
                           [&](const auto& p) { return p.first == w.name; });
    if (it != last_workers_.end() && it->second == ws) continue;
    if (it == last_workers_.end()) last_workers_.emplace_back(w.name, ws);
    else it->second = ws;
    bus.publish(
        BusLine("worker.state").str("name", w.name).str("state", ws).str("activity", w.activity).done(),
        "worker." + w.name);
  }

  // Turn text, opt-in and keyless: it is the one thing here that is a record
  // rather than a reading, so it is neither coalesced nor published unless the
  // run asked for it.
  if (ctx_.publish_text) {
    if (snap.lines.size() < last_lines_) last_lines_ = 0;  // the chat was cleared
    // **A line is published when it is finished, not when it appears.** The
    // assistant's line is created empty and then grown token by token as the
    // reply streams in, so publishing on arrival — which is what this did in
    // its first run, and the capture caught it — puts `"text":""` on the bus
    // and never corrects it. A line is finished when there is another one
    // after it, or when the session has gone quiet.
    std::size_t done = snap.lines.size();
    if (done > 0 && snap.state != VoiceSession::State::Idle &&
        snap.state != VoiceSession::State::Failed) {
      --done;  // the last line is still being written
    }
    for (std::size_t i = last_lines_; i < done; ++i) {
      bus.publish(BusLine("turn.text")
                      .str("role", snap.lines[i].user ? "user" : "assistant")
                      .str("text", snap.lines[i].text)
                      .done());
    }
    last_lines_ = std::max(last_lines_, done);
  }
}

// ------------------------------------------------------------- file hatch

bool BusFileHatch::open(const std::string& in_path, const std::string& out_path,
                        std::string* error) {
  bool ok = true;
  if (!in_path.empty()) {
    in_path_ = in_path;
    // Existing content is skipped, not replayed: the file is a channel, and a
    // run that started by applying yesterday's commands would be a surprise
    // every time the same file was reused. `--buttons` is the other shape and
    // it is still there for a file meant to be applied at startup.
    std::error_code ec;
    const auto size = std::filesystem::file_size(in_path_, ec);
    in_at_ = ec ? 0 : static_cast<std::uint64_t>(size);
  }
  if (!out_path.empty()) {
    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) {
      if (error) *error = "cannot write " + out_path;
      ok = false;
    } else {
      out_path_ = out_path;
      out_ok_ = true;
      AppBus::instance().set_listener(true);
    }
  }
  return ok;
}

void BusFileHatch::poll(float dt) {
  wait_ -= dt;
  if (wait_ > 0.0f) return;
  wait_ = 0.1f;

  if (!in_path_.empty()) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(in_path_, ec);
    if (!ec) {
      const auto now = static_cast<std::uint64_t>(size);
      // Truncated or replaced: start again rather than reading from a stale
      // offset, which would cut lines in half.
      if (now < in_at_) {
        in_at_ = 0;
        partial_.clear();
      }
      if (now > in_at_) {
        std::ifstream f(in_path_, std::ios::binary);
        if (f) {
          f.seekg(static_cast<std::streamoff>(in_at_));
          std::string chunk;
          chunk.resize(static_cast<std::size_t>(now - in_at_));
          f.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
          chunk.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, f.gcount())));
          in_at_ += chunk.size();
          partial_ += chunk;
          // Whole lines only. A half-written line stays in the buffer until
          // its newline arrives; a writer flushing mid-line is the normal case
          // for a file being appended to by hand.
          std::size_t nl = partial_.find('\n');
          while (nl != std::string::npos) {
            AppBus::instance().post_lines(partial_.substr(0, nl));
            partial_.erase(0, nl + 1);
            nl = partial_.find('\n');
          }
          // A line longer than the bus will ever accept is dropped here rather
          // than growing this buffer for the rest of the run.
          if (partial_.size() > kBusLineMax) partial_.clear();
        }
      }
    }
  }

  if (out_ok_) {
    const std::vector<std::string> events = AppBus::instance().drain_events();
    const std::size_t dropped = AppBus::instance().take_events_dropped();
    if (events.empty() && dropped == 0) return;
    std::ofstream f(out_path_, std::ios::binary | std::ios::app);
    if (!f) return;
    for (const std::string& e : events) f << e << "\n";
    if (dropped) f << BusLine("bus.dropped").num("events", static_cast<double>(dropped), 0).done() << "\n";
  }
}

}  // namespace aii
