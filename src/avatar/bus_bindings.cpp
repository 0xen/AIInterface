#include "bus_bindings.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "core/button_registry.h"
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

void BusBindings::publish(const VoiceSession::Snapshot& snap, float dt) {
  AppBus& bus = AppBus::instance();

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
