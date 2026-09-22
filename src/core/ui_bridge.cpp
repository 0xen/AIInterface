// M28: script windows. See ui_bridge.h for the shape and the reasoning; this
// file is just the bookkeeping the header promises.
#include "core/ui_bridge.h"

#include <cctype>

#include "core/text_util.h"

namespace aii {

namespace {

// Bounds one command in place, returning whether anything in it was cut.
bool clamp_command(UiCommand* cmd) {
  bool cut = false;
  if (cmd->label.size() > kUiTextMax) {
    cmd->label = clip_utf8(std::move(cmd->label), kUiTextMax);
    cut = true;
  }
  if (cmd->text.size() > kUiTextMax) {
    cmd->text = clip_utf8(std::move(cmd->text), kUiTextMax);
    cut = true;
  }
  if (cmd->items.size() > kUiItemsMax) {
    cmd->items.resize(kUiItemsMax);
    cut = true;
  }
  for (auto& item : cmd->items) {
    if (item.size() > kUiTextMax) {
      item = clip_utf8(std::move(item), kUiTextMax);
      cut = true;
    }
  }
  if (cmd->values.size() > kUiValuesMax) {
    cmd->values.resize(kUiValuesMax);
    cut = true;
  }
  return cut;
}

}  // namespace

bool UiBridge::open(const UiWindowSpec& spec, std::string* error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ui_valid_key(spec.key)) {
    if (error) {
      *error = "window key is empty, too long, or has characters outside [A-Za-z0-9_.-]";
    }
    return false;
  }
  const bool already_open = entries_.find(spec.key) != entries_.end();
  if (!already_open && entries_.size() >= kUiWindowsMax) {
    if (error) *error = "too many windows are already open";
    return false;
  }
  Entry& entry = entries_[spec.key];
  entry.spec = spec;
  entry.user_closed = false;
  entry.script_closed = false;
  return true;
}

bool UiBridge::submit(const std::string& key, UiFrame frame, std::string* error) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(key);
  if (it == entries_.end()) return false;

  bool cut = false;
  if (frame.cmds.size() > kUiCommandsMax) {
    frame.cmds.resize(kUiCommandsMax);
    cut = true;
  }
  for (auto& cmd : frame.cmds) {
    if (clamp_command(&cmd)) cut = true;
  }

  frame.generation = next_generation_++;
  it->second.frame = std::move(frame);
  if (cut && error) *error = "the frame exceeded a bound and was truncated";
  return true;
}

void UiBridge::close(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(key);
  if (it != entries_.end()) it->second.script_closed = true;
}

bool UiBridge::is_open(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(key);
  return it != entries_.end() && !it->second.user_closed && !it->second.script_closed;
}

std::vector<UiResult> UiBridge::take_results(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<UiResult> out;
  auto it = entries_.find(key);
  if (it == entries_.end()) return out;
  out.reserve(it->second.results.size());
  for (auto& [id, result] : it->second.results) {
    out.push_back(result);
    result.clicked = false;
    result.changed = false;
  }
  return out;
}

std::vector<std::string> UiBridge::keys() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  out.reserve(entries_.size());
  for (const auto& [key, entry] : entries_) out.push_back(key);
  return out;
}

void UiBridge::snapshot(std::vector<UiWindowSpec>* open, std::vector<std::string>* closing) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (open) open->clear();
  if (closing) closing->clear();
  for (const auto& [key, entry] : entries_) {
    if (entry.script_closed) {
      if (closing) closing->push_back(key);
    } else if (!entry.user_closed) {
      if (open) open->push_back(entry.spec);
    }
  }
}

bool UiBridge::frame_for(const std::string& key, UiFrame* out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(key);
  if (it == entries_.end()) return false;
  if (out) *out = it->second.frame;
  return true;
}

std::uint64_t UiBridge::generation_of(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(key);
  return it == entries_.end() ? 0 : it->second.frame.generation;
}

void UiBridge::set_results(const std::string& key, const std::vector<UiResult>& results) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(key);
  if (it == entries_.end()) return;
  for (const auto& result : results) {
    UiResult& stored = it->second.results[result.id];
    stored.id = result.id;
    stored.clicked = stored.clicked || result.clicked;
    stored.changed = stored.changed || result.changed;
    stored.b = result.b;
    for (int i = 0; i < 4; ++i) stored.f[i] = result.f[i];
    stored.i = result.i;
    stored.s = result.s;
  }
}

void UiBridge::mark_closed(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(key);
  if (it != entries_.end()) it->second.user_closed = true;
}

void UiBridge::remove(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.erase(key);
}

std::string ui_compose_id(const std::vector<std::string>& stack, const std::string& label) {
  if (stack.empty()) return label;
  std::string out;
  for (const auto& part : stack) {
    out += part;
    out += '/';
  }
  out += label;
  return out;
}

bool ui_valid_key(const std::string& key) {
  if (key.empty() || key.size() > kUiKeyMax) return false;
  for (unsigned char c : key) {
    const bool ok = std::isalnum(c) || c == '_' || c == '.' || c == '-';
    if (!ok) return false;
  }
  return true;
}

}  // namespace aii
