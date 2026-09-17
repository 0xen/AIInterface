#include "core/app_bus.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/text_util.h"
#include "json.hpp"

namespace aii {
namespace {

using json = nlohmann::json;

std::string clip_string(std::string s) {
  if (s.size() > kBusStringMax) s.resize(kBusStringMax);
  return s;
}

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        // Control characters only; everything else, UTF-8 included, is passed
        // through as the bytes it already is. A \u escape of a multi-byte
        // sequence would have to decode it, and the one thing this app cannot
        // afford to get wrong is Japanese text.
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

}  // namespace

// ------------------------------------------------------------------ values

std::string BusMessage::str(const char* key, const std::string& fallback) const {
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.kind != BusValue::Kind::Str) return fallback;
  return it->second.s;
}

double BusMessage::num(const char* key, double fallback) const {
  const auto it = fields.find(key);
  if (it == fields.end()) return fallback;
  if (it->second.kind == BusValue::Kind::Num) return it->second.n;
  if (it->second.kind == BusValue::Kind::Bool) return it->second.b ? 1.0 : 0.0;
  return fallback;
}

bool BusMessage::flag(const char* key, bool fallback) const {
  const auto it = fields.find(key);
  if (it == fields.end()) return fallback;
  if (it->second.kind == BusValue::Kind::Bool) return it->second.b;
  if (it->second.kind == BusValue::Kind::Num) return it->second.n != 0.0;
  return fallback;
}

const std::vector<double>* BusMessage::nums(const char* key) const {
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.kind != BusValue::Kind::Nums) return nullptr;
  return &it->second.nums;
}

bool BusMessage::has(const char* key) const { return fields.find(key) != fields.end(); }

// -------------------------------------------------------------------- line

BusLine::BusLine(const std::string& topic) {
  out_ = "{\"t\":\"" + json_escape(topic) + "\"";
}

BusLine& BusLine::str(const char* key, const std::string& value) {
  out_ += ",\"";
  out_ += key;
  out_ += "\":\"" + json_escape(clip_string(value)) + "\"";
  return *this;
}

BusLine& BusLine::num(const char* key, double value, int decimals) {
  // Non-finite is not JSON. A level that went NaN is a bug upstream, and
  // emitting `nan` would take the whole line down at the reader instead of
  // there, so it is published as 0 and the line stays parseable.
  if (!std::isfinite(value)) value = 0.0;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", std::clamp(decimals, 0, 9), value);
  out_ += ",\"";
  out_ += key;
  out_ += "\":";
  out_ += buf;
  return *this;
}

BusLine& BusLine::flag(const char* key, bool value) {
  out_ += ",\"";
  out_ += key;
  out_ += "\":";
  out_ += value ? "true" : "false";
  return *this;
}

std::string BusLine::done() const { return out_ + "}"; }

// --------------------------------------------------------------------- bus

AppBus& AppBus::instance() {
  static AppBus bus;
  return bus;
}

void AppBus::note_locked(std::string line) {
  if (status_.size() >= kBusStatusMax) return;  // the log is not a queue either
  status_.push_back(std::move(line));
}

void AppBus::publish(std::string line, const std::string& key) {
  std::lock_guard<std::mutex> l(mutex_);
  if (!key.empty()) {
    // Coalesce in place, keeping the slot's position: a reader that drains
    // once a second sees the newest value of each continuous signal in the
    // order those signals first arrived, rather than a thousand stale ones.
    for (Queued& q : events_) {
      if (q.key == key) {
        q.line = std::move(line);
        return;
      }
    }
  }
  if (events_.size() >= kBusEventsMax) {
    // Keyless events only ever reach here (the keyed ones coalesced above), so
    // this is a flood of turn text with nothing draining it. Oldest first: the
    // newest line is the one still worth having.
    events_.pop_front();
    ++events_dropped_;
  }
  events_.push_back(Queued{key, std::move(line)});
}

std::vector<std::string> AppBus::drain_events() {
  std::lock_guard<std::mutex> l(mutex_);
  std::vector<std::string> out;
  out.reserve(events_.size());
  for (Queued& q : events_) out.push_back(std::move(q.line));
  events_.clear();
  return out;
}

std::size_t AppBus::take_events_dropped() {
  std::lock_guard<std::mutex> l(mutex_);
  const std::size_t n = events_dropped_;
  events_dropped_ = 0;
  return n;
}

bool AppBus::has_listener() const {
  std::lock_guard<std::mutex> l(mutex_);
  return listening_;
}

void AppBus::set_listener(bool listening) {
  std::lock_guard<std::mutex> l(mutex_);
  listening_ = listening;
}

void AppBus::add_family(std::string family, BusHandler handler) {
  std::lock_guard<std::mutex> l(mutex_);
  families_[std::move(family)] = std::move(handler);
}

bool AppBus::post(const std::string& line, std::string* error) {
  const auto fail = [&](std::string why) {
    if (error) *error = why;
    std::lock_guard<std::mutex> l(mutex_);
    note_locked("refused: " + why);
    return false;
  };
  // Length first, before the parser sees a byte of it. Everything after this
  // point is bounded by it.
  if (line.size() > kBusLineMax) return fail("line over " + std::to_string(kBusLineMax) + " bytes");

  const json j = json::parse(line, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) return fail("not a JSON object");

  const auto t = j.find("t");
  if (t == j.end() || !t->is_string()) return fail("no \"t\"");
  const std::string topic = clip_string(t->get<std::string>());
  const std::size_t dot = topic.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= topic.size())
    return fail("\"t\" is not family.verb: " + topic);

  BusMessage msg;
  msg.family = topic.substr(0, dot);
  msg.verb = topic.substr(dot + 1);
  for (const auto& [key, value] : j.items()) {
    if (key == "t") continue;
    if (msg.fields.size() >= kBusFieldsMax) break;  // ignored, not refused
    BusValue v;
    if (value.is_string()) {
      v.kind = BusValue::Kind::Str;
      v.s = clip_string(value.get<std::string>());
    } else if (value.is_boolean()) {
      v.kind = BusValue::Kind::Bool;
      v.b = value.get<bool>();
    } else if (value.is_number()) {
      v.kind = BusValue::Kind::Num;
      v.n = value.get<double>();
      if (!std::isfinite(v.n)) continue;
    } else if (value.is_array()) {
      // Flat numbers, or triples flattened into the same list: the plan's
      // sketch wrote cells as [[x,y,c],...] and a hand-written line is easier
      // as [x,y,c,x,y,c]. Both arrive here as one flat list and no consumer
      // has to know which was sent. Anything else in the array is skipped.
      v.kind = BusValue::Kind::Nums;
      for (const auto& e : value) {
        if (v.nums.size() >= kBusNumbersMax) break;
        if (e.is_number()) {
          const double d = e.get<double>();
          if (std::isfinite(d)) v.nums.push_back(d);
        } else if (e.is_array()) {
          for (const auto& e2 : e) {
            if (v.nums.size() >= kBusNumbersMax) break;
            if (!e2.is_number()) continue;
            const double d = e2.get<double>();
            if (std::isfinite(d)) v.nums.push_back(d);
          }
        }
      }
    } else {
      continue;  // null, object: ignored, so a later version's field is safe
    }
    msg.fields.emplace(key, std::move(v));
  }

  {
    std::lock_guard<std::mutex> l(mutex_);
    if (inbox_.size() >= kBusInboxMax) {
      // Newest dropped, not oldest: a command the frame loop has not applied
      // yet is worth more than the one behind it, and dropping from the front
      // would also reorder what does get applied.
      note_locked("inbox full, dropped " + topic);
      if (error) *error = "inbox full";
      return false;
    }
    inbox_.push_back(std::move(msg));
  }
  return true;
}

std::size_t AppBus::post_lines(const std::string& text) {
  std::size_t n = 0;
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t nl = text.find('\n', at);
    if (nl == std::string::npos) nl = text.size();
    const std::string line = trim(text.substr(at, nl - at));
    at = nl + 1;
    if (line.empty() || line[0] == '#') continue;
    if (post(line, nullptr)) ++n;
  }
  return n;
}

std::size_t AppBus::apply_pending() {
  // The queue is swapped out under the lock and the handlers run outside it:
  // a handler is app code that touches the avatar, the registry and the
  // settings, and holding the bus's mutex across that would make the bus part
  // of every lock order in the program.
  std::deque<BusMessage> todo;
  std::map<std::string, BusHandler> families;
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (inbox_.empty()) return 0;  // the ordinary frame: no copy, no allocation
    todo.swap(inbox_);
    families = families_;
  }
  std::vector<std::string> notes;
  std::size_t applied = 0;
  for (const BusMessage& m : todo) {
    const auto it = families.find(m.family);
    if (it == families.end()) {
      notes.push_back("no such family: " + m.family);
      continue;
    }
    std::string err;
    it->second(m, &err);
    if (!err.empty()) notes.push_back(m.family + "." + m.verb + ": " + err);
    else ++applied;
  }
  if (!notes.empty()) {
    std::lock_guard<std::mutex> l(mutex_);
    for (std::string& n : notes) note_locked(std::move(n));
  }
  return applied;
}

std::vector<std::string> AppBus::take_status() {
  std::lock_guard<std::mutex> l(mutex_);
  std::vector<std::string> out;
  out.swap(status_);
  return out;
}

void AppBus::reset() {
  std::lock_guard<std::mutex> l(mutex_);
  events_.clear();
  inbox_.clear();
  families_.clear();
  status_.clear();
  events_dropped_ = 0;
  listening_ = false;
}

}  // namespace aii
