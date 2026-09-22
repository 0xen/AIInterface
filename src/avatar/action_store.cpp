#include "action_store.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#include "core/app_bus.h"
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

// FNV-1a, 64-bit, over the whole file, as sixteen hex digits. The same
// function `user_paths.cpp` uses for the seed manifest and for the same
// reason: it answers "are these the same bytes?" and keeps this file to the
// standard library, so the store stays testable with no engine and no window.
//
// **Not the seed manifest.** `.seeded` records what this *app shipped* into a
// tree, so that an upgrade can tell a refresh from a hand edit. This records
// what the *user armed*, and the two must not be conflated: a file the seeder
// legitimately refreshed is still a file the user has not consented to in its
// new form, and that is precisely the case finding 11 is about.
std::string file_fingerprint(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return std::string();
  std::uint64_t h = 1469598103934665603ull;
  char buf[4096];
  while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
    const std::streamsize n = f.gcount();
    for (std::streamsize i = 0; i < n; ++i) {
      h ^= static_cast<std::uint8_t>(buf[i]);
      h *= 1099511628211ull;
    }
    if (!f) break;
  }
  char out[17] = {};
  std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(h));
  return std::string(out);
}

// The policy folder: one level above `actions\`, and the same directory
// `ScriptHost::discover()` scans. Stated here rather than derived from
// `actions_root().parent_path()` so that the two readers of this rule are both
// looking at the same sentence.
fs::path scripts_root() { return user_data_root() / "scripts"; }

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

fs::path ActionStore::tmp_root() { return user_data_root() / "scripts" / "tmp"; }

std::string ActionStore::tmp_overview(const std::vector<Action>& temp_rows) {
  std::string out;
  out += "# Temporary scripts\n\n";
  out += "Everything in this folder is temporary and could be cleared at any point. A script "
         "here runs the moment it is named in a `run` line -- there is no approval window and "
         "nothing about it is remembered between launches. This file is written by the app and "
         "regenerated whenever the folder changes; edits to it are lost. To keep one of these "
         "beyond \"could be cleared at any point\", move it into ..\\actions\\, where it goes "
         "through the normal approval.\n\n";
  if (temp_rows.empty()) {
    out += "(empty)\n";
    return out;
  }
  for (const Action& a : temp_rows) {
    out += "- " + a.name + ".py -- ";
    out += a.description.empty() ? std::string("(no description)") : a.description;
    out += "\n";
  }
  return out;
}

std::string ActionStore::policy_key(const std::string& name) { return "policy:" + name; }

bool ActionStore::policy_allowed(const fs::path& file) {
  // Read straight off the record, with no store and no scan: this is called
  // from `ScriptHost::discover()` at startup, before anything else has looked
  // at the directory. The failure direction is deliberate — an unreadable or
  // absent record allows nothing, so a policy runs only when a file this app
  // wrote says the user said yes to these exact bytes.
  std::ifstream f(ActionStore::actions_root() / kArmedFile, std::ios::binary);
  if (!f) return false;
  json j = json::parse(f, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return false;
  const auto armed = j.find("armed");
  if (armed == j.end() || !armed->is_object()) return false;
  const std::string key = policy_key(file.stem().string());
  const auto it = armed->find(key);
  if (it == armed->end() || !it->is_string()) return false;
  const std::string recorded = it->get<std::string>();
  if (recorded.empty()) return false;
  return recorded == file_fingerprint(file);
}

void ActionStore::load() {
  const fs::path root = actions_root();
  std::error_code ec;
  fs::create_directories(root, ec);
  // M29. The temporary folder, created here too: `rescan()` needs it to exist
  // before its first `directory_iterator`, and the CLAUDE.md it writes needs
  // somewhere to land even when nothing has ever been dropped in it.
  fs::create_directories(tmp_root(), ec);
  // The same seed rule the rest of the app uses: refresh a file when the
  // shipped bytes changed and nobody has touched the installed copy, so a
  // better example reaches a machine that has already run the app without
  // walking over an action the user rewrote. The shipped example is an *action*
  // and lands armed only if the user says so, exactly like one the AI wrote.
  std::string seed_err;
  std::vector<std::string> seed_notes;
  if (!seed_tree(fs::path(AII_ASSETS_DIR) / "scripts" / "actions", root, &seed_err, &seed_notes))
    std::fprintf(stderr, "[actions] %s\n", seed_err.c_str());
  for (const std::string& note : seed_notes) std::fprintf(stderr, "[actions] %s\n", note.c_str());
  read_armed();
  rescan();
  news_.clear();
}

void ActionStore::read_armed() {
  armed_.clear();
  seen_names_.clear();
  std::ifstream f(actions_root() / kArmedFile, std::ios::binary);
  if (!f) return;
  json j = json::parse(f, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return;
  const auto armed = j.find("armed");
  if (armed != j.end()) {
    // M19.1 writes an object of key -> fingerprint. Everything before it wrote
    // a bare array of names, and that file is still read rather than
    // discarded: throwing away a consent record on upgrade would re-ask about
    // every action the user has ever armed, which is a pop-up that teaches
    // them to click it away.
    if (armed->is_object()) {
      for (const auto& [key, value] : armed->items())
        armed_[key] = value.is_string() ? value.get<std::string>() : std::string();
    } else if (armed->is_array()) {
      for (const json& n : *armed)
        if (n.is_string()) armed_[n.get<std::string>()] = std::string();
    }
  }
  const auto seen = j.find("seen");
  if (seen != j.end() && seen->is_array()) {
    for (const json& n : *seen)
      if (n.is_string()) seen_names_.push_back(n.get<std::string>());
  }
  // A file written by an older build has `armed` and no `seen`. Everything
  // armed has plainly been answered, so it counts as seen; anything else in
  // the directory is asked about once, which is the right side to err on.
  for (const auto& [key, fingerprint] : armed_) {
    (void)fingerprint;
    if (std::find(seen_names_.begin(), seen_names_.end(), key) == seen_names_.end())
      seen_names_.push_back(key);
  }
}

void ActionStore::write_armed() const {
  json j = json::object();
  j["armed"] = json::object();
  for (const auto& [key, fingerprint] : armed_) j["armed"][key] = fingerprint;
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
    a.fingerprint = file_fingerprint(e.path());
    found.push_back(std::move(a));
  }
  // Sorted, so "which action is listed first" is a property of the name rather
  // than of the order the filesystem happened to hand them back — the same
  // reason `ScriptHost::discover()` sorts.
  std::sort(found.begin(), found.end(),
            [](const Action& a, const Action& b) { return a.name < b.name; });

  // M29. Temp rows are appended *after* every action, so `in_digest` (the
  // `kActionDigestMax` cap below) favours the ones the user actually consented
  // to over a throwaway the assistant wrote a minute ago. A name already taken
  // by an action wins outright: `run name=x` has to resolve to exactly one
  // file, and it is the one the user clicked Confirm on.
  std::set<std::string> names_taken;
  for (const Action& a : found) names_taken.insert(a.name);
  rescan_tmp(found, names_taken);

  for (std::size_t i = 0; i < found.size(); ++i) {
    // A temp row is armed by location, not by a record in `armed_` — there is
    // no consent to compare bytes against, so it never goes through
    // `resolve_armed()`.
    found[i].armed = found[i].temporary ? true : resolve_armed(&found[i]);
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
    // M29. A temp row is never seen, never queued and never written to
    // `_armed.json` — its news was already pushed inside `rescan_tmp()`, the
    // one place that knows whether this launch has announced it before.
    if (a.temporary) continue;
    if (std::find(seen_names_.begin(), seen_names_.end(), a.name) != seen_names_.end()) continue;
    // Already in the queue means already asked about, and the two reasons it
    // can be there are both reasons not to arm it from here: the user is
    // looking at the question right now, or M19.1 has just revoked it because
    // the bytes moved. `set_auto_allow`'s own comment has always said the
    // switch does not reach into the queue; before M19.1 the next scan did it
    // anyway, one second later.
    if (std::find(awaiting_.begin(), awaiting_.end(), a.name) != awaiting_.end()) continue;
    if (auto_allow_) {
      // Armed on sight, because the user has said every new one may be. Still
      // recorded as seen, so turning the switch back off does not make the
      // whole set ask again.
      armed_[a.name] = a.fingerprint;
      seen_names_.push_back(a.name);
      write_armed();
      news_.push_back(a.name);
    } else {
      awaiting_.push_back(a.name);
    }
  }
  actions_ = std::move(found);
  // Re-resolve armed after any auto-arm above, so the list handed out is never
  // one frame behind the file that was just written. A temp row is left
  // alone: it has no entry in `armed_` by design and is always armed anyway.
  for (Action& a : actions_) {
    if (a.temporary) continue;
    const auto it = armed_.find(a.name);
    a.armed = it != armed_.end();
  }
  rescan_policies();
  // A file deleted outside the app leaves nothing behind here either. Policies
  // are in this queue too, so the check is against everything `find()` can
  // resolve rather than against the action list alone.
  awaiting_.erase(std::remove_if(awaiting_.begin(), awaiting_.end(),
                                 [&](const std::string& n) { return find(n) == nullptr; }),
                  awaiting_.end());
}

// M29. The temp-folder scan, appended to `found` (the action rows already
// gathered by `rescan()`) rather than kept apart the way policies are: a temp
// row is callable by name exactly like an action, and `kActionDigestMax`
// needs to see both lists as one to make the actions-first ordering mean
// anything.
void ActionStore::rescan_tmp(std::vector<Action>& found, const std::set<std::string>& names_taken) {
  std::vector<Action> temp_found;
  std::error_code ec;
  const fs::path root = tmp_root();
  for (const fs::directory_entry& e : fs::directory_iterator(root, ec)) {
    if (ec) break;
    if (!e.is_regular_file(ec) || ec) continue;
    if (!is_python_file(e.path())) continue;
    const std::string file = e.path().filename().string();
    if (!file.empty() && (file[0] == '_' || file[0] == '.')) continue;
    if (found.size() + temp_found.size() >= kActionsMax) break;
    Action a;
    a.name = e.path().stem().string();
    if (names_taken.count(a.name)) {
      // Named once per launch, not once per second: `warned_collisions_`
      // makes this a one-time note rather than a line every scan for as long
      // as the stray file sits there.
      if (warned_collisions_.insert(a.name).second)
        std::fprintf(stderr,
                     "[action] temp script '%s' shares a name with an action; the action wins "
                     "and the temp file is ignored\n",
                     a.name.c_str());
      continue;
    }
    a.path = e.path().string();
    a.description = truncated(first_docstring_line(e.path()), kActionDescMax);
    // Cheap, and kept for symmetry with an action row even though `CLAUDE.md`
    // shows none of it: there is no consent record to key it against here,
    // but a fingerprint that always exists is one fewer special case for
    // anything that later reads an `Action` without checking `temporary`.
    a.fingerprint = file_fingerprint(e.path());
    a.temporary = true;
    a.armed = true;
    temp_found.push_back(std::move(a));
  }
  std::sort(temp_found.begin(), temp_found.end(),
            [](const Action& a, const Action& b) { return a.name < b.name; });

  // News: a temp row seen for the first time this launch is news to the
  // running conversation, exactly like an auto-armed action, because the
  // assistant just wrote it and needs to know mid-conversation that it is
  // callable. `announced_temp_` is per-launch and unpersisted on purpose —
  // there is nothing to remember between launches about a folder that "could
  // be cleared at any point".
  for (const Action& a : temp_found)
    if (announced_temp_.insert(a.name).second) news_.push_back(a.name);

  // `CLAUDE.md`, written only when its content changed. On the very first
  // scan of a launch, compare against whatever is already on disk too, so an
  // unchanged folder costs no write across a restart.
  const std::string overview = tmp_overview(temp_found);
  if (!tmp_overview_checked_) {
    tmp_overview_checked_ = true;
    std::ifstream cur(root / "CLAUDE.md", std::ios::binary);
    if (cur) {
      std::ostringstream ss;
      ss << cur.rdbuf();
      last_tmp_overview_ = ss.str();
    }
  }
  if (overview != last_tmp_overview_) {
    std::ofstream f(root / "CLAUDE.md", std::ios::binary | std::ios::trunc);
    if (f) {
      f << overview;
      last_tmp_overview_ = overview;
    } else {
      std::fprintf(stderr, "[action] could not write %s\n",
                   (root / "CLAUDE.md").string().c_str());
    }
  }

  found.insert(found.end(), std::make_move_iterator(temp_found.begin()),
              std::make_move_iterator(temp_found.end()));
}

// M19.2. The policy scan. Deliberately the same shape as the action scan above
// — flat, non-recursive, `_` and `.` skipped — because `ScriptHost::discover()`
// picks exactly that set up, and a gate that disagrees with the thing it gates
// is worse than no gate.
void ActionStore::rescan_policies() {
  std::vector<Action> found;
  std::error_code ec;
  for (const fs::directory_entry& e : fs::directory_iterator(scripts_root(), ec)) {
    if (ec) break;
    if (!e.is_regular_file(ec) || ec) continue;
    if (!is_python_file(e.path())) continue;
    const std::string file = e.path().filename().string();
    if (!file.empty() && (file[0] == '_' || file[0] == '.')) continue;
    if (found.size() >= kActionsMax) break;
    Action a;
    a.name = e.path().stem().string();
    a.path = e.path().string();
    a.policy = true;
    a.in_digest = false;  // never in the prompt: the model cannot call one
    const std::string doc = first_docstring_line(e.path());
    // Said on the row rather than assumed known. The approval window shows a
    // name and a line, and "this one runs every time you start the app" is the
    // whole difference between the two things the window now asks about.
    a.description = truncated(
        doc.empty() ? std::string("runs at every launch") : "runs at every launch - " + doc,
        kActionDescMax);
    a.fingerprint = file_fingerprint(e.path());
    found.push_back(std::move(a));
  }
  std::sort(found.begin(), found.end(),
            [](const Action& a, const Action& b) { return a.name < b.name; });

  for (Action& a : found) {
    a.armed = resolve_armed(&a);
    const std::string key = policy_key(a.name);
    if (a.armed) continue;
    if (std::find(seen_names_.begin(), seen_names_.end(), key) != seen_names_.end()) continue;
    if (std::find(awaiting_.begin(), awaiting_.end(), key) != awaiting_.end()) continue;
    // **`auto_allow` is not consulted here, and that is the point.** That
    // switch is the user saying the scripts *the assistant writes for them to
    // call* may run without a click. A file that runs unattended for the whole
    // life of the app from the next launch is the case they asked for a pop-up
    // about, so it always gets one.
    awaiting_.push_back(key);
    std::fprintf(stderr, "[scripts] new policy script '%s' is held until you allow it: %s\n",
                 a.name.c_str(), a.path.c_str());
    AppBus::instance().post(BusLine("script.status")
                                .str("text", "the policy script '" + a.name +
                                                 "' is waiting for you to allow it")
                                .flag("ok", false)
                                .done());
  }
  policies_ = std::move(found);
}

bool ActionStore::resolve_armed(Action* a) {
  const std::string key = a->policy ? policy_key(a->name) : a->name;
  const auto it = armed_.find(key);
  if (it == armed_.end()) return false;
  if (it->second.empty()) {
    // A record from before M19.1. There is no arm-time to compare against, so
    // the current bytes are adopted as the armed ones and the adoption is
    // logged: an upgrade that re-asked about every action instead would put a
    // window of names in front of the user with nothing new to tell them.
    it->second = a->fingerprint;
    write_armed();
    std::fprintf(stderr,
                 "[actions] '%s' was armed before its bytes were recorded; recording them now\n",
                 key.c_str());
    return true;
  }
  if (!a->fingerprint.empty() && it->second == a->fingerprint) return true;
  // The bytes moved under a yes the user gave to different code, or the file
  // cannot be read at all. Consent is withdrawn here and now — written back,
  // so it survives the app being killed — and the question goes back into the
  // queue the approval window draws.
  armed_.erase(it);
  seen_names_.erase(std::remove(seen_names_.begin(), seen_names_.end(), key), seen_names_.end());
  write_armed();
  note_changed(*a);
  return false;
}

void ActionStore::note_changed(const Action& a) {
  const std::string key = a.policy ? policy_key(a.name) : a.name;
  if (std::find(awaiting_.begin(), awaiting_.end(), key) == awaiting_.end())
    awaiting_.push_back(key);
  const char* kind = a.policy ? "policy script" : "action";
  std::fprintf(stderr, "[actions] the %s '%s' changed since you armed it; it will not run "
                       "until you confirm it again\n",
               kind, a.name.c_str());
  // The existing status path: the same `script.status` a script itself posts,
  // so this lands in the Scripts section of the settings surface and in the
  // bus log, with no new route to keep working. Posted rather than published:
  // inbound is what `BusBindings` applies on the frame loop.
  AppBus::instance().post(BusLine("script.status")
                              .str("text", std::string("the ") + kind + " '" + a.name +
                                               "' changed since you armed it; confirm it again")
                              .flag("ok", false)
                              .done());
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

const Action* ActionStore::find_action(const std::string& name) const {
  for (const Action& a : actions_)
    if (a.name == name) return &a;
  return nullptr;
}

const Action* ActionStore::find(const std::string& name) const {
  if (const Action* a = find_action(name)) return a;
  // A policy answers only to its prefixed key, which is what the queue and the
  // approval window carry. A bare name never reaches one, so the window can
  // draw a policy row without the name in an ```aii``` block ever resolving.
  for (const Action& p : policies_)
    if (policy_key(p.name) == name) return &p;
  return nullptr;
}

ActionRefusal ActionStore::check(const std::string& name) const {
  const Action* a = find_action(name);
  // The name is resolved against the discovered set and nothing else. This is
  // the line that makes "a name, never a path and never a body" true: there is
  // no branch here that reads a file the scan did not already find.
  if (!a) return ActionRefusal::NoSuchAction;
  if (!authoring_) return ActionRefusal::AuthoringOff;
  if (!a->in_digest) return ActionRefusal::PastCap;
  // M29. A temp row has no consent record to compare against — the folder it
  // sits in is the trust boundary, not a fingerprint in `_armed.json` — so
  // `NotArmed` and `Changed` do not apply to it. `AuthoringOff` and `PastCap`
  // still do: the folder does not override the author switch or the digest
  // cap.
  if (a->temporary) return ActionRefusal::None;
  if (!a->armed) return ActionRefusal::NotArmed;
  // M19.1. The last word on consent is the file on disk at the moment of the
  // call, not the scan up to a second ago and not the name. The body is read
  // again by `runpy` a few milliseconds from here, so this is the only
  // comparison that is about the bytes that will actually run.
  const auto it = armed_.find(a->name);
  const std::string now = file_fingerprint(a->path);
  if (it == armed_.end() || now.empty() || (!it->second.empty() && it->second != now))
    return ActionRefusal::Changed;
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
  // **The bytes are read here, at the moment of the yes.** That is the whole
  // of M19.1: the record the later calls are checked against is taken from the
  // file the user was just shown, so "armed" can never mean more than the code
  // that was on disk when they clicked.
  const Action* a = find(name);
  armed_[name] = a ? file_fingerprint(a->path) : std::string();
  if (std::find(seen_names_.begin(), seen_names_.end(), name) == seen_names_.end())
    seen_names_.push_back(name);
  write_armed();
  awaiting_.erase(std::remove(awaiting_.begin(), awaiting_.end(), name), awaiting_.end());
  for (Action& x : actions_)
    if (x.name == name) x.armed = true;
  bool is_policy = false;
  for (Action& p : policies_)
    if (policy_key(p.name) == name) {
      p.armed = true;
      is_policy = true;
    }
  // `news_` is what the running conversation is told, and the model has no
  // verb that reaches a policy, so arming one is not news to it. It is news to
  // the user, and they were told by the window they clicked.
  if (!is_policy) news_.push_back(name);
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
  // `find_action`, not `find`: the Scripts row deletes actions, and a policy
  // file is the user's own and is not this button's to unlink.
  const Action* a = find_action(name);
  if (!a) return false;
  std::error_code ec;
  const bool gone = fs::remove(a->path, ec) && !ec;
  if (!gone) return false;
  armed_.erase(name);
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

// §9's digest: `name [not armed] -- one line`, plus M29's `[temp]` mark for a
// row found in `tmp\` rather than `actions\` — always armed, never asked
// about, and worth flagging so the assistant can tell the user which ones
// would need moving to `actions\` to survive the folder being cleared.
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
    if (a.temporary) out += "  [temp]";
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
  std::size_t action_n = 0, armed_n = 0, temp_n = 0;
  for (const Action& a : actions_) {
    if (a.temporary) {
      ++temp_n;
      continue;
    }
    ++action_n;
    if (a.armed) ++armed_n;
  }
  std::string out = action_n == 0
                         ? std::string("No actions")
                         : std::to_string(action_n) + (action_n == 1 ? " action, " : " actions, ") +
                               std::to_string(armed_n) + " armed";
  // Only mentioned when there is at least one, so the common case — nothing in
  // `tmp\` — reads exactly as it always has.
  if (temp_n > 0) out += "; " + std::to_string(temp_n) + " temporary";
  out += ".";
  return out;
}

}  // namespace aii
