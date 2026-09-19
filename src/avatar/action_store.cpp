#include "action_store.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include "core/user_paths.h"
#include "json.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace aii {
namespace {

// How often the directory is looked at. A `directory_iterator` over a flat
// folder of a few files is microseconds, and one a second is fast enough that
// "write me a script" → "now use it" feels immediate while costing nothing
// measurable on a 60 Hz loop.
constexpr float kScanEvery = 1.0f;

// Where the armed set is remembered.
//
// **Deliberately not `settings.json`.** That file is a mirror, not a master:
// `panel`, `language`, `timing`, `startup`, `tools`, `model` and `avatar` are
// written out of `AvatarUiState` *every frame*, so a value put there by
// anything else is overwritten within about sixteen milliseconds, silently,
// and the write looks like it worked (see `settings.h`). A list that grows a
// row per authored action is also not a settings key — it is this class's own
// state — so it gets its own file with exactly one writer, which is this file.
//
// The leading `_` is the same rule the script scan already uses, so the
// bookkeeping can never be mistaken for an action.
const char* kArmedFile = "_armed.json";

bool is_python_file(const fs::path& p) {
  std::string ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == ".py";
}

std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return std::string();
  const size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// The first line of the module docstring, and nothing else.
//
// A docstring may be as long as its author likes; **only its first line is
// ever paid for**, because the digest is on every turn forever. This reads the
// head of the file rather than the whole of it for the same reason: a
// thousand-line action costs the same to describe as a ten-line one.
//
// Anything that is not a docstring — no docstring, a file that opens with
// code, a file that cannot be read — yields an empty description, which the
// digest renders as the honest "(no description)" rather than inventing one.
std::string first_docstring_line(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return std::string();
  std::string line;
  int guard = 0;
  bool first = true;
  while (std::getline(f, line) && guard++ < 64) {
    // A UTF-8 BOM is invisible in an editor and is not part of the docstring.
    // **Measured, not anticipated**: the first capture of this feature showed a
    // script with a perfectly good docstring listed as "(no description)",
    // because the tool that wrote it added one. `PromptStore::read_file`
    // already strips a BOM for exactly this reason, and anything that writes a
    // file here — an editor, a shell redirect, the model's own Write — may add
    // one without saying so.
    if (first && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
      line.erase(0, 3);
    first = false;
    const std::string t = trim(line);
    if (t.empty()) continue;
    if (t[0] == '#') continue;  // a shebang or a comment header, not the doc
    const bool tri3 = t.rfind("\"\"\"", 0) == 0 || t.rfind("'''", 0) == 0;
    if (!tri3) return std::string();  // the file opens with code: no docstring
    const std::string quote = t.substr(0, 3);
    std::string rest = t.substr(3);
    // `"""Say hello."""` and `"""Say hello.` are the same fact; so is a
    // `"""` on its own with the sentence on the next line.
    const size_t close = rest.find(quote);
    if (close != std::string::npos) rest = rest.substr(0, close);
    rest = trim(rest);
    if (!rest.empty()) return rest;
    if (std::getline(f, line)) return trim(line);
    return std::string();
  }
  return std::string();
}

std::string truncated(const std::string& s, std::size_t max) {
  if (s.size() <= max) return s;
  // Cut on a UTF-8 boundary, so a Japanese description does not end in half a
  // character — the prompt is read by a model, but the Scripts row is read by
  // the user and a mojibake tail there is a bug report.
  std::size_t cut = max;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  return s.substr(0, cut) + "...";
}

}  // namespace

fs::path ActionStore::actions_root() { return user_data_root() / "scripts" / "actions"; }

void ActionStore::load() {
  const fs::path root = actions_root();
  std::error_code ec;
  fs::create_directories(root, ec);
  // The same seed rule the rest of the app uses: refresh a file only when the
  // shipped one is newer, so a better example reaches a machine that has
  // already run the app. The shipped example is an *action* and lands armed
  // only if the user says so, exactly like one the AI wrote.
  seed_tree(fs::path(AII_ASSETS_DIR) / "scripts" / "actions", root, nullptr);
  read_armed();
  rescan();
  news_.clear();
}

void ActionStore::read_armed() {
  armed_names_.clear();
  seen_names_.clear();
  std::ifstream f(actions_root() / kArmedFile, std::ios::binary);
  if (!f) return;
  json j = json::parse(f, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return;
  const auto read = [&](const char* key, std::vector<std::string>* into) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return;
    for (const json& n : *it)
      if (n.is_string()) into->push_back(n.get<std::string>());
  };
  read("armed", &armed_names_);
  read("seen", &seen_names_);
  // A file written by an older build has `armed` and no `seen`. Everything
  // armed has plainly been answered, so it counts as seen; anything else in
  // the directory is asked about once, which is the right side to err on.
  for (const std::string& n : armed_names_)
    if (std::find(seen_names_.begin(), seen_names_.end(), n) == seen_names_.end())
      seen_names_.push_back(n);
}

void ActionStore::write_armed() const {
  json j = json::object();
  j["armed"] = armed_names_;
  j["seen"] = seen_names_;
  std::error_code ec;
  fs::create_directories(actions_root(), ec);
  std::ofstream f(actions_root() / kArmedFile, std::ios::binary | std::ios::trunc);
  if (!f) return;
  f << j.dump(2) << "\n";
}

void ActionStore::rescan() {
  std::vector<Action> found;
  std::error_code ec;
  for (const fs::directory_entry& e : fs::directory_iterator(actions_root(), ec)) {
    if (ec) break;
    if (!e.is_regular_file(ec) || ec) continue;
    if (!is_python_file(e.path())) continue;
    const std::string file = e.path().filename().string();
    if (!file.empty() && (file[0] == '_' || file[0] == '.')) continue;
    if (found.size() >= kActionsMax) break;
    Action a;
    a.name = e.path().stem().string();
    a.path = e.path().string();
    a.description = truncated(first_docstring_line(e.path()), kActionDescMax);
    found.push_back(std::move(a));
  }
  // Sorted, so "which action is listed first" is a property of the name rather
  // than of the order the filesystem happened to hand them back — the same
  // reason `ScriptHost::discover()` sorts.
  std::sort(found.begin(), found.end(),
            [](const Action& a, const Action& b) { return a.name < b.name; });

  for (std::size_t i = 0; i < found.size(); ++i) {
    found[i].armed = std::find(armed_names_.begin(), armed_names_.end(), found[i].name) !=
                     armed_names_.end();
    found[i].in_digest = i < kActionDigestMax;
  }

  // **"New" is "never answered", not "appeared since this launch".**
  //
  // The first version of this marked everything present at startup as already
  // known, which read as the obvious way to avoid nagging about actions armed
  // months ago. It was wrong, and in the one direction that matters: a file
  // the model wrote while the app was *closed* would then never be asked
  // about, and would sit in the list unarmed with nothing having said so. The
  // seen set is persisted instead, so the question is asked exactly once per
  // action, whenever the app happens to notice it, and a dismissal is durable
  // rather than lasting until the next launch.
  for (const Action& a : found) {
    if (std::find(seen_names_.begin(), seen_names_.end(), a.name) != seen_names_.end()) continue;
    if (auto_allow_) {
      // Armed on sight, because the user has said every new one may be. Still
      // recorded as seen, so turning the switch back off does not make the
      // whole set ask again.
      armed_names_.push_back(a.name);
      seen_names_.push_back(a.name);
      write_armed();
      news_.push_back(a.name);
    } else if (std::find(awaiting_.begin(), awaiting_.end(), a.name) == awaiting_.end()) {
      awaiting_.push_back(a.name);
    }
  }
  // A file deleted outside the app leaves nothing behind here either.
  awaiting_.erase(std::remove_if(awaiting_.begin(), awaiting_.end(),
                                 [&](const std::string& n) {
                                   return !std::any_of(
                                       found.begin(), found.end(),
                                       [&](const Action& a) { return a.name == n; });
                                 }),
                  awaiting_.end());
  actions_ = std::move(found);
  // Re-resolve armed after any auto-arm above, so the list handed out is never
  // one frame behind the file that was just written.
  for (Action& a : actions_)
    a.armed =
        std::find(armed_names_.begin(), armed_names_.end(), a.name) != armed_names_.end();
}

bool ActionStore::tick(float dt) {
  since_scan_ += dt;
  if (since_scan_ < kScanEvery) return false;
  since_scan_ = 0.0f;
  const std::size_t before_n = actions_.size();
  std::vector<std::string> before;
  for (const Action& a : actions_) before.push_back(a.name + (a.armed ? "+" : "-"));
  rescan();
  std::vector<std::string> after;
  for (const Action& a : actions_) after.push_back(a.name + (a.armed ? "+" : "-"));
  return before_n != actions_.size() || before != after;
}

const Action* ActionStore::find(const std::string& name) const {
  for (const Action& a : actions_)
    if (a.name == name) return &a;
  return nullptr;
}

ActionRefusal ActionStore::check(const std::string& name) const {
  const Action* a = find(name);
  // The name is resolved against the discovered set and nothing else. This is
  // the line that makes "a name, never a path and never a body" true: there is
  // no branch here that reads a file the scan did not already find.
  if (!a) return ActionRefusal::NoSuchAction;
  if (!authoring_) return ActionRefusal::AuthoringOff;
  if (!a->in_digest) return ActionRefusal::PastCap;
  if (!a->armed) return ActionRefusal::NotArmed;
  return ActionRefusal::None;
}

void ActionStore::set_auto_allow(bool on) {
  const bool was = auto_allow_;
  auto_allow_ = on;
  // Turning it on does not retroactively arm what is already waiting. The
  // user's sentence is about *new* scripts, and a queue the user has been
  // looking at is not new — silently arming three things they had not answered
  // would be the switch doing more than it says.
  (void)was;
}

void ActionStore::arm(const std::string& name) {
  if (std::find(armed_names_.begin(), armed_names_.end(), name) == armed_names_.end())
    armed_names_.push_back(name);
  if (std::find(seen_names_.begin(), seen_names_.end(), name) == seen_names_.end())
    seen_names_.push_back(name);
  write_armed();
  awaiting_.erase(std::remove(awaiting_.begin(), awaiting_.end(), name), awaiting_.end());
  for (Action& a : actions_)
    if (a.name == name) a.armed = true;
  news_.push_back(name);
}

void ActionStore::dismiss(const std::string& name) {
  // **Nothing is deleted.** The action keeps existing, unarmed, and the Scripts
  // row is where it can be armed or removed later.
  //
  // What *is* written is that it has been answered, so it does not ask again
  // at the next launch. "Not now" that turns into "every time you start the
  // app" is the shape of notification the user learns to click through
  // without reading, which would cost the Confirm on the next one its meaning.
  if (std::find(seen_names_.begin(), seen_names_.end(), name) == seen_names_.end()) {
    seen_names_.push_back(name);
    write_armed();
  }
  awaiting_.erase(std::remove(awaiting_.begin(), awaiting_.end(), name), awaiting_.end());
}

void ActionStore::arm_all() {
  const std::vector<std::string> queue = awaiting_;
  for (const std::string& n : queue) arm(n);
}

void ActionStore::dismiss_all() {
  const std::vector<std::string> queue = awaiting_;
  for (const std::string& n : queue) dismiss(n);
}

bool ActionStore::remove(const std::string& name) {
  const Action* a = find(name);
  if (!a) return false;
  std::error_code ec;
  const bool gone = fs::remove(a->path, ec) && !ec;
  if (!gone) return false;
  armed_names_.erase(std::remove(armed_names_.begin(), armed_names_.end(), name),
                     armed_names_.end());
  // Forgotten entirely, not just disarmed: a later file of the same name is a
  // different script and must be asked about on its own account.
  seen_names_.erase(std::remove(seen_names_.begin(), seen_names_.end(), name),
                    seen_names_.end());
  write_armed();
  awaiting_.erase(std::remove(awaiting_.begin(), awaiting_.end(), name), awaiting_.end());
  actions_.erase(std::remove_if(actions_.begin(), actions_.end(),
                                [&](const Action& x) { return x.name == name; }),
                 actions_.end());
  return true;
}

std::string ActionStore::digest() const {
  if (actions_.empty()) return std::string("(none yet)");
  std::string out;
  std::size_t hidden = 0;
  for (const Action& a : actions_) {
    if (!a.in_digest) {
      ++hidden;
      continue;
    }
    out += a.name;
    // **The armed state is a mark on the row, not a paragraph.** One bracket
    // per row is the whole cost of the user's "the AI should know it cannot
    // use an unarmed script and should tell them to arm it" — and M3.13's
    // finding was that the row and the syntax line are what move this model,
    // where surrounding prose bought hallucinated readings.
    if (!a.armed) out += "  [NOT ARMED - ask the user to arm it]";
    out += " -- ";
    out += a.description.empty() ? std::string("(no description)") : a.description;
    out += "\n";
  }
  if (hidden > 0) {
    // Named and refused rather than silently omitted: growing past the cap has
    // to fail where somebody can see it.
    out += "(" + std::to_string(hidden) +
           " more exist but are past the limit this list carries, and calling one is "
           "refused; the user can delete some under Scripts.)\n";
  }
  return out;
}

std::vector<std::string> ActionStore::take_news() {
  std::vector<std::string> out;
  out.swap(news_);
  return out;
}

std::string ActionStore::summary() const {
  if (actions_.empty()) return std::string("No actions.");
  std::size_t armed = 0;
  for (const Action& a : actions_)
    if (a.armed) ++armed;
  return std::to_string(actions_.size()) +
         (actions_.size() == 1 ? " action, " : " actions, ") + std::to_string(armed) + " armed.";
}

}  // namespace aii
