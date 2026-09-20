#include "core/prompt_store.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <system_error>

#include "core/config.h"
#include "core/text_util.h"
#include "core/user_paths.h"
#include "json.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace aii {
namespace {

const json& member(const json& j, const char* key) {
  static const json kNull;
  if (!j.is_object()) return kNull;
  const auto it = j.find(key);
  return it == j.end() ? kNull : *it;
}

PromptKind kind_from(const std::string& s) {
  if (s == "project") return PromptKind::Project;
  if (s == "skill") return PromptKind::Skill;
  return PromptKind::Global;
}

const char* kind_name(PromptKind k) {
  switch (k) {
    case PromptKind::Project: return "project";
    case PromptKind::Skill: return "skill";
    default: return "global";
  }
}

// Read a whole file as bytes and normalise away the two things a Windows text
// editor adds invisibly: a UTF-8 BOM and CRLF line endings. Opened in binary
// so that normalisation is this function's decision and not the CRT's.
bool read_file(const fs::path& p, std::string* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  // A UTF-8 BOM is invisible in an editor and is not part of the prompt. The
  // user is invited to edit these files by hand and Notepad adds one.
  if (out->size() >= 3 && static_cast<unsigned char>((*out)[0]) == 0xEF &&
      static_cast<unsigned char>((*out)[1]) == 0xBB &&
      static_cast<unsigned char>((*out)[2]) == 0xBF)
    out->erase(0, 3);
  // CRLF to LF, for the same reason as the BOM and with more at stake. Saving
  // one of these bodies in Notepad rewrites every line ending in the file; the
  // prose is unchanged and the user has no way to see that anything happened,
  // but the composed prompt would be a different string and the CLI's prompt
  // cache would be thrown away on the next launch. Line endings are not part
  // of a prompt, so they are not allowed to be part of its bytes.
  out->erase(std::remove(out->begin(), out->end(), '\r'), out->end());
  return true;
}

// Trailing whitespace off the end of a body, nothing else touched.
//
// This is the whole of what makes composition byte-stable across launches, and
// it exists because of one specific hazard: every text editor in the world has
// an opinion about the final newline, and some of them change it on save. A
// body that sometimes ends "\n" and sometimes "\n\n" would compose to a
// different string on different days, and the only visible symptom would be
// that prompt caching had quietly stopped working. So the file's trailing
// newlines carry no meaning; the separator between nodes is composition's, not
// the body's.
std::string trim_end(std::string s) {
  const auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

// What goes between two node bodies. One blank line — the separator the
// milestone asks for, and the one already between the two paragraphs of the
// prompt this replaces.
constexpr const char* kSeparator = "\n\n";

}  // namespace

const PromptNode* PromptGraph::find(const std::string& id) const {
  for (const PromptNode& n : nodes)
    if (n.id == id) return &n;
  return nullptr;
}

fs::path PromptStore::root() {
  if (const std::string over = env_or("AII_PROMPTS_DIR", ""); !over.empty()) return fs::path(over);
  return user_data_root() / "prompts";
}

bool PromptStore::load(std::string* error) {
  graphs_.clear();
  problems_.clear();
  const fs::path dir = root();
  // Seeded every start, not only when the directory is absent — see
  // `seed_tree`, and commit 636f24e for what the other rule cost.
  std::string seed_err;
  if (!seed_tree(fs::path(AII_ASSETS_DIR) / "prompts", dir, &seed_err)) {
    problems_.push_back(seed_err);  // not fatal: an existing store still loads
    if (error) *error = seed_err;
  }

  std::string text;
  if (!read_file(dir / "graph.json", &text)) {
    if (error) *error = "no prompt graph at " + (dir / "graph.json").string();
    return false;
  }
  json root_j = json::parse(text, nullptr, false);
  if (root_j.is_discarded() || !root_j.is_object()) {
    if (error) *error = (dir / "graph.json").string() + " is not valid JSON";
    return false;
  }

  const json& graphs_j = member(root_j, "graphs");
  if (!graphs_j.is_object()) {
    if (error) *error = "graph.json has no `graphs` object";
    return false;
  }
  for (auto it = graphs_j.begin(); it != graphs_j.end(); ++it) {
    PromptGraph g;
    g.name = it.key();
    const json& gj = it.value();
    const json& start = member(gj, "start");
    if (const json& sid = member(start, "id"); sid.is_string()) g.start_id = sid.get<std::string>();
    if (const json& sp = member(start, "pos"); sp.is_array() && sp.size() == 2) {
      g.start_pos[0] = sp[0].get<float>();
      g.start_pos[1] = sp[1].get<float>();
    }
    for (const json& nj : member(gj, "nodes")) {
      if (!nj.is_object()) continue;
      PromptNode n;
      if (const json& v = member(nj, "id"); v.is_string()) n.id = v.get<std::string>();
      if (n.id.empty()) continue;  // a node with no id cannot be linked to
      if (const json& v = member(nj, "title"); v.is_string()) n.title = v.get<std::string>();
      if (const json& v = member(nj, "file"); v.is_string()) n.file = v.get<std::string>();
      if (const json& v = member(nj, "kind"); v.is_string()) n.kind = kind_from(v.get<std::string>());
      if (const json& v = member(nj, "enabled"); v.is_boolean()) n.enabled = v.get<bool>();
      if (const json& v = member(nj, "cwd"); v.is_string()) n.cwd = v.get<std::string>();
      if (const json& v = member(nj, "pos"); v.is_array() && v.size() == 2) {
        n.pos[0] = v[0].get<float>();
        n.pos[1] = v[1].get<float>();
      }
      for (const json& t : member(nj, "triggers"))
        if (t.is_string()) n.triggers.push_back(t.get<std::string>());
      if (!n.file.empty() && !read_file(dir / fs::path(n.file), &n.body)) {
        // A declared body that is not on disk is worth saying out loud: it is
        // exactly the shape of the avatar-seed bug, and composing silently
        // without it would be the same silent degradation.
        //
        // Three places, because one was not enough: on the node, so the
        // inspector can say this prompt is not in Claude's head; in
        // `problems()`, so the caller sees it whatever `load()` returned; and
        // in `*error`, which is only the last of them and is why this needed
        // the other two.
        n.body.clear();  // belt and braces: nothing composes from a failed read
        n.body_error = "cannot read " + n.file;
        problems_.push_back("prompt `" + n.id + "`: " + n.body_error);
        if (error) *error = problems_.back();
      }
      g.nodes.push_back(std::move(n));
    }
    for (const json& lj : member(gj, "links")) {
      if (!lj.is_object()) continue;
      PromptLink l;
      if (const json& v = member(lj, "from"); v.is_string()) l.from = v.get<std::string>();
      if (const json& v = member(lj, "to"); v.is_string()) l.to = v.get<std::string>();
      if (l.from.empty() || l.to.empty()) continue;
      if (const json& v = member(lj, "order"); v.is_number_integer()) l.order = v.get<int>();
      if (const json& v = member(lj, "enabled"); v.is_boolean()) l.enabled = v.get<bool>();
      g.links.push_back(std::move(l));
    }
    graphs_.push_back(std::move(g));
  }
  // The graphs themselves in a fixed order, so anything that iterates them —
  // M5's inspector, a save — does not depend on JSON object key order.
  std::sort(graphs_.begin(), graphs_.end(),
            [](const PromptGraph& a, const PromptGraph& b) { return a.name < b.name; });
  return true;
}

bool PromptStore::save(std::string* error) const {
  const fs::path dir = root();
  std::error_code ec;
  fs::create_directories(dir, ec);
  json root_j;
  root_j["version"] = 1;
  json graphs_j = json::object();
  for (const PromptGraph& g : graphs_) {
    json gj;
    gj["start"] = {{"id", g.start_id}, {"pos", {g.start_pos[0], g.start_pos[1]}}};
    json nodes_j = json::array();
    for (const PromptNode& n : g.nodes) {
      json nj;
      nj["id"] = n.id;
      nj["title"] = n.title;
      nj["file"] = n.file;
      nj["kind"] = kind_name(n.kind);
      nj["enabled"] = n.enabled;
      nj["pos"] = {n.pos[0], n.pos[1]};
      if (!n.triggers.empty()) nj["triggers"] = n.triggers;
      if (!n.cwd.empty()) nj["cwd"] = n.cwd;
      nodes_j.push_back(std::move(nj));
      if (n.file.empty()) continue;
      // A body we failed to *read* is never written back. `body` is empty for
      // one of those, and writing it would truncate a file that may be perfectly
      // good and merely locked when the store was read -- turning a temporary
      // failure into a permanent one, on a file the user wrote by hand.
      if (!n.body_error.empty()) continue;
      const fs::path bp = dir / fs::path(n.file);
      fs::create_directories(bp.parent_path(), ec);
      // Binary, so the bytes that were loaded are the bytes written back: a
      // text-mode write would turn every LF into CRLF and make the next launch
      // compose a different string from the same prompt.
      std::ofstream out(bp, std::ios::binary | std::ios::trunc);
      if (!out) {
        if (error) *error = "cannot write " + bp.string();
        return false;
      }
      out.write(n.body.data(), static_cast<std::streamsize>(n.body.size()));
    }
    gj["nodes"] = std::move(nodes_j);
    json links_j = json::array();
    for (const PromptLink& l : g.links)
      links_j.push_back({{"from", l.from}, {"to", l.to}, {"order", l.order}, {"enabled", l.enabled}});
    gj["links"] = std::move(links_j);
    graphs_j[g.name] = std::move(gj);
  }
  root_j["graphs"] = std::move(graphs_j);
  std::ofstream out(dir / "graph.json", std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "cannot write " + (dir / "graph.json").string();
    return false;
  }
  const std::string text = root_j.dump(2) + "\n";
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return true;
}

const PromptGraph* PromptStore::graph(const std::string& name) const {
  for (const PromptGraph& g : graphs_)
    if (g.name == name) return &g;
  return nullptr;
}

std::vector<std::string> PromptStore::compose_order(const std::string& name) const {
  const PromptGraph* g = graph(name);
  if (!g) return {};

  // ---- the ordering rule, which prompt caching depends on ----
  //
  // The system prompt is a launch argument and the CLI caches on it, so two
  // launches that produce different bytes from the same store throw the cache
  // away. Everything here is therefore ordered by data, never by the order
  // things happened to be written in a file:
  //
  //  - outgoing links are sorted by `order`, and ties are broken by the target
  //    node's id, so two links that an editor gave the same order can never
  //    swap between runs;
  //  - the walk is a depth-first traversal from Start with a visited set, so a
  //    node reachable twice is emitted once, at its first position, and a
  //    cycle terminates instead of hanging;
  //  - disabled nodes and disabled links are skipped, and a disabled node is
  //    *not* traversed through — disconnecting is how M6 disables a subtree;
  //  - bodies are trailing-trimmed and joined with exactly one blank line, so
  //    a text editor's opinion about the final newline cannot change the
  //    output, and the result never ends in a newline.
  //
  // M5.2 split this function in two at the last line: the walk decides *which*
  // nodes, in what order, and compose() below joins their bodies. A node whose
  // body is empty after trimming contributes nothing to the prompt, so it is
  // not named here either — the list and the text stay the same list.
  std::vector<std::string> parts;
  std::vector<std::string> seen;
  // Iterative DFS: `stack` holds ids still to visit, in reverse order, so the
  // first child is popped first.
  std::vector<std::string> stack{g->start_id};
  while (!stack.empty()) {
    const std::string id = stack.back();
    stack.pop_back();
    if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
    seen.push_back(id);

    if (id != g->start_id) {
      const PromptNode* n = g->find(id);
      if (!n || !n->enabled) continue;  // and do not walk on through it
      if (!trim_end(n->body).empty()) parts.push_back(n->id);
    }

    std::vector<const PromptLink*> out;
    for (const PromptLink& l : g->links)
      if (l.enabled && l.from == id) out.push_back(&l);
    std::sort(out.begin(), out.end(), [](const PromptLink* a, const PromptLink* b) {
      return a->order != b->order ? a->order < b->order : a->to < b->to;
    });
    for (auto it = out.rbegin(); it != out.rend(); ++it) stack.push_back((*it)->to);
  }

  return parts;
}

std::string PromptStore::compose(const std::string& name) const {
  const PromptGraph* g = graph(name);
  if (!g) return {};
  std::string composed;
  bool first = true;
  for (const std::string& id : compose_order(name)) {
    const PromptNode* n = g->find(id);
    if (!n) continue;  // cannot happen: compose_order only names nodes it found
    if (!first) composed += kSeparator;
    first = false;
    composed += trim_end(n->body);
  }
  return composed;
}

// ------------------------------------------------------- M3.3 / M3.4
namespace {

// ASCII-only lowering, applied to raw UTF-8. Safe by construction: every byte
// of a multi-byte UTF-8 sequence is >= 0x80 and is left exactly as it is, so
// Japanese passes through untouched (it has no case anyway) and only the Latin
// half of a mixed string is folded.
std::string ascii_lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// UTF-8 code points, not bytes: the minimum-length rule below is about how
// specific a word is, and "プロ" is two characters and six bytes.
std::size_t code_points(const std::string& s) {
  std::size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

bool is_ascii(const std::string& s) {
  for (unsigned char c : s)
    if (c >= 0x80) return false;
  return true;
}

// A trigger short enough to appear inside unrelated words is worse than no
// trigger, because the prompt it pulls in is then wrong *and* unrepeatable —
// the loaded set means it can never be un-sent. Three characters for ASCII,
// two for anything else. See the class comment.
bool usable_trigger(const std::string& s) {
  if (s.empty()) return false;
  const std::size_t n = code_points(s);
  return n >= (is_ascii(s) ? 3u : 2u);
}

void add_trigger(std::vector<std::string>* out, const std::string& s) {
  const std::string t = ascii_lower(trim_end(s));
  if (!usable_trigger(t)) return;
  if (std::find(out->begin(), out->end(), t) == out->end()) out->push_back(t);
}

// Attribute values come from the store, not from the transcript, but they are
// still spliced into markup the model reads as structure — so escape them
// rather than trusting that no prompt will ever be titled `a "b" & c`.
std::string xml_attr(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

}  // namespace

void PromptInjector::reset(const PromptStore& store) {
  lazy_.clear();
  for (const PromptGraph& g : store.graphs()) {
    for (const PromptNode& n : g.nodes) {
      if (n.kind == PromptKind::Global || !n.enabled) continue;
      Lazy z;
      z.id = n.id;
      z.title = n.title.empty() ? n.id : n.title;
      z.kind = n.kind == PromptKind::Project ? "project" : "skill";
      z.body = trim_end(n.body);
      if (z.body.empty()) continue;  // nothing to inject
      add_trigger(&z.match, n.id);
      add_trigger(&z.match, n.title);
      for (const std::string& t : n.triggers) add_trigger(&z.match, t);
      if (z.match.empty()) continue;  // unreachable by mention; `load` still finds it by id
      lazy_.push_back(std::move(z));
    }
  }
  // Store order, so two prompts mentioned in one sentence are always injected
  // in the same order — the same byte-stability argument as composition, one
  // level down.
  std::sort(lazy_.begin(), lazy_.end(), [](const Lazy& a, const Lazy& b) { return a.id < b.id; });
}

bool PromptInjector::is_loaded(const std::string& id) const {
  return std::find(loaded_.begin(), loaded_.end(), id) != loaded_.end();
}

const PromptInjector::Lazy* PromptInjector::resolve(const std::string& name) const {
  const std::string want = ascii_lower(trim_end(name));
  if (want.empty()) return nullptr;
  for (const Lazy& z : lazy_)
    if (ascii_lower(z.id) == want) return &z;
  for (const Lazy& z : lazy_)
    if (ascii_lower(z.title) == want) return &z;
  for (const Lazy& z : lazy_)
    for (const std::string& t : z.match)
      if (t == want) return &z;
  return nullptr;
}

bool PromptInjector::request(const std::string& name) {
  const Lazy* z = resolve(name);
  if (!z) return false;                                 // not in the store: refused
  if (is_loaded(z->id)) return true;                    // already in this session's context
  if (std::find(pending_.begin(), pending_.end(), z->id) == pending_.end())
    pending_.push_back(z->id);
  return true;
}

std::string PromptInjector::decorate(const std::string& user_text) {
  if (lazy_.empty()) return user_text;
  const std::string hay = ascii_lower(user_text);

  std::vector<const Lazy*> take;
  const auto queue = [&](const Lazy* z) {
    if (!z || is_loaded(z->id)) return;
    if (std::find(take.begin(), take.end(), z) == take.end()) take.push_back(z);
  };
  // `load name=` first: the model asked for it explicitly on the previous turn,
  // so it leads even when this turn also mentions something.
  for (const std::string& id : pending_)
    for (const Lazy& z : lazy_)
      if (z.id == id) queue(&z);
  pending_.clear();

  for (const Lazy& z : lazy_) {
    if (is_loaded(z.id)) continue;
    for (const std::string& t : z.match) {
      if (hay.find(t) != std::string::npos) {  // substring, deliberately
        queue(&z);
        break;
      }
    }
  }
  if (take.empty()) return user_text;

  std::string out;
  for (const Lazy* z : take) {
    out += "<context name=\"" + xml_attr(z->title) + "\" kind=\"" + xml_attr(z->kind) + "\">\n";
    out += z->body;
    out += "\n</context>\n\n";
    loaded_.push_back(z->id);
  }
  out += user_text;
  return out;
}

// ------------------------------------------------- the pre-prompt beside the exe

// Remove every `<!-- … -->` span. The file next to the exe is the one prompt
// surface a user finds without being told where to look, so it carries a header
// explaining what it is and when an edit takes effect — and that header must not
// reach Claude. An unterminated `<!--` swallows the rest of the file, which is
// the safe direction: a half-written comment sends nothing rather than sending
// the explanation as if it were an instruction.
//
// M3.15 is the second file with a header like that (`system/handoff.md`), and
// a second copy of this loop is how the two would come to disagree about what
// a comment is — so it is declared in the header now rather than being local.
std::string strip_html_comments(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    const std::size_t open = s.find("<!--", i);
    if (open == std::string::npos) {
      out.append(s, i, std::string::npos);
      break;
    }
    out.append(s, i, open - i);
    const std::size_t close = s.find("-->", open + 4);
    if (close == std::string::npos) break;
    i = close + 3;
  }
  return out;
}

namespace {

// Leading blank lines and spaces, so that a body written under a stripped
// comment composes to the same bytes as the same body written on line one.
std::string trim_front(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  return s.substr(i);
}

}  // namespace

fs::path local_prompt_path() {
  if (const std::string over = env_or("AII_PRE_PROMPT", ""); !over.empty()) return fs::path(over);
  return exe_dir() / "pre-prompt.md";
}

std::string local_prompt() {
  const fs::path p = local_prompt_path();
  std::error_code ec;
  // Create-if-missing, never refresh. See the header: a rebuild must not
  // overwrite prose the user has rewritten, and this file exists to be
  // rewritten.
  if (!fs::exists(p, ec)) {
    const fs::path shipped = fs::path(AII_ASSETS_DIR) / "pre-prompt.md";
    if (fs::exists(shipped, ec)) {
      fs::create_directories(p.parent_path(), ec);
      fs::copy_file(shipped, p, ec);
      if (ec) std::fprintf(stderr, "[prompts] seeding %s: %s\n", p.string().c_str(), ec.message().c_str());
    }
  }
  std::string text;
  if (!read_file(p, &text)) return {};  // no file, or unreadable: run on the store alone
  return trim_end(trim_front(strip_html_comments(text)));
}

namespace {

// Is a conditional key true? `*known` says whether it is a key at all, which
// the caller needs in order to tell a false section from a typo.
bool section_truth(const std::string& key, const ToolPolicy& p, bool* known) {
  *known = true;
  // `tools`: any group in force. Not a group itself, and the one key that is
  // not in the table, because "you have no tools of your own" is a sentence
  // about the whole grant rather than about any one row.
  if (key == "tools") {
    for (int i = 0; i < kToolGroupCount; ++i)
      if (tool_group_active(p, i)) return true;
    return false;
  }
  for (int i = 0; i < kToolGroupCount; ++i)
    if (key == tool_group(i).key) return tool_group_active(p, i);
  *known = false;
  return false;
}

// Three or more newlines down to two, so that a paragraph dropped by a
// conditional leaves no gap behind it. Nothing else about the text is touched.
std::string collapse_blank_runs(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  std::size_t runs = 0;
  for (const char c : s) {
    if (c == '\n') {
      if (++runs > 2) continue;
    } else {
      runs = 0;
    }
    out += c;
  }
  return out;
}

}  // namespace

std::string expand_tool_sections(const std::string& text, const ToolPolicy& policy,
                                 std::vector<std::string>* problems) {
  const auto note = [&](const std::string& what) {
    if (problems) problems->push_back(what);
  };
  // One frame per open section, holding what emission was *before* it, so a
  // closing tag restores rather than guesses. `emit` is the conjunction of
  // every condition currently open.
  struct Frame {
    std::string key;
    bool emit_before;
  };
  std::vector<Frame> open;
  bool emit = true;
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const std::size_t t = text.find("{{", i);
    if (t == std::string::npos) {
      if (emit) out.append(text, i, std::string::npos);
      break;
    }
    if (emit) out.append(text, i, t - i);
    const std::size_t e = text.find("}}", t + 2);
    if (e == std::string::npos) {
      // An unterminated `{{` is prose, not a tag: pass it through rather than
      // swallowing the rest of the prompt.
      if (emit) out.append(text, t, std::string::npos);
      break;
    }
    const std::string tag = text.substr(t + 2, e - t - 2);
    i = e + 2;
    const char sigil = tag.empty() ? '\0' : tag[0];
    if (sigil != '#' && sigil != '^' && sigil != '/') {
      // Not a section tag at all. `{{` is rare enough in this prose that
      // passing it through unchanged is safer than deciding it meant
      // something.
      if (emit) out.append(text, t, i - t);
      continue;
    }
    const std::string key = tag.substr(1);
    if (sigil == '/') {
      if (open.empty() || open.back().key != key) {
        note("prompt: `{{/" + key + "}}` closes a section that is not open");
        continue;
      }
      emit = open.back().emit_before;
      open.pop_back();
      continue;
    }
    bool known = false;
    const bool value = section_truth(key, policy, &known);
    open.push_back({key, emit});
    if (!known) {
      note("prompt: `{{" + std::string(1, sigil) + key + "}}` is not a tool group; its text is kept as written");
      continue;  // emission unchanged: the prose stays, the typo is reported
    }
    emit = emit && (sigil == '#' ? value : !value);
  }
  for (const Frame& f : open) note("prompt: `{{#" + f.key + "}}` is never closed");
  return trim_end(collapse_blank_runs(out));
}

namespace {
// M3.14. Set before the session is built and read when the prompt is
// composed. A plain string rather than a callback: `core` is not allowed to
// know what a settings file is, and a value it can neither parse nor validate
// is exactly the right amount of knowledge to hand it.
std::string g_settings_digest;
// M10.5. The same shape and the same reasoning, one file along: the list of
// actions this machine has, generated because it is different on every machine
// and after every turn the model writes a file in.
std::string g_actions_digest;
// M13.3. The same shape again: the voice syntax line and the per-language
// counts, empty unless secondary voices are configured.
std::string g_voices_digest;
// The resolved scripts folder, so the reference sheet can be pointed at rather
// than merely mentioned.
std::string g_scripts_dir;
}  // namespace

void set_settings_digest(std::string text) { g_settings_digest = std::move(text); }
void set_actions_digest(std::string text) { g_actions_digest = std::move(text); }
void set_voices_digest(std::string text) { g_voices_digest = std::move(text); }
void set_scripts_dir(std::string path) { g_scripts_dir = std::move(path); }

const std::string& system_prompt(const ToolPolicy& policy) {
  // Keyed on the policy rather than computed once and for all — see the
  // header. One process normally asks for one policy and gets the cached
  // bytes every time after the first.
  //
  // M3.14 adds the settings digest to the key. The policy alone stopped being
  // enough the moment a *model* change could restart the child: that restart
  // composes a new prompt with an unchanged `ToolPolicy`, and a cache keyed on
  // the policy would have handed the new child the previous file's values —
  // this app stating, in the system prompt, something it had itself just made
  // untrue.
  static ToolPolicy cached_for;
  static std::string cached_digest;
  static std::string cached_actions;
  static std::string cached_voices;
  static std::string cached;
  static bool have = false;
  if (have && cached_for == policy && cached_digest == g_settings_digest &&
      cached_actions == g_actions_digest && cached_voices == g_voices_digest)
    return cached;

  PromptStore store;
  std::string err;
  if (!store.load(&err) && !err.empty()) std::fprintf(stderr, "[prompts] %s\n", err.c_str());
  // This is the string that actually becomes `--system-prompt`, so a prompt
  // that is declared and missing from it is the exact failure worth shouting
  // about, whatever `load()` returned.
  for (const std::string& p : store.problems()) std::fprintf(stderr, "[prompts] %s\n", p.c_str());
  // M3.9. The store's own text first, then the conditionals resolved against
  // what the user has actually granted. Done here and not in `compose()`
  // because the policy is the *caller's*: the store knows the prose and this
  // function knows the app.
  std::vector<std::string> section_problems;
  // M3.14. Substituted before the conditionals are resolved, so the two never
  // interact: `expand_tool_sections` looks for `{{#` and `{{^` and this is
  // neither, and by the time it runs there is no `{{settings}}` left for a
  // future change to that parser to trip over. A store whose `settings.md`
  // has been deleted or emptied simply has no slot and loses nothing.
  std::string text = store.compose("system");
  if (const size_t at = text.find("{{settings}}"); at != std::string::npos) {
    // `trim_end` so a digest ending in a newline does not leave a blank line
    // the collapse pass would then have to reason about.
    text.replace(at, std::strlen("{{settings}}"), trim_end(g_settings_digest));
  }
  // M10.5. Same rule, same place, same reason: a list of what exists on this
  // machine cannot be written in the Markdown, and the prose around it stays
  // in the Markdown where the user can edit it.
  if (const size_t at = text.find("{{scripts}}"); at != std::string::npos) {
    text.replace(at, std::strlen("{{scripts}}"), trim_end(g_actions_digest));
  }
  // M13.3. Same rule once more, with one difference that is the whole point:
  // this digest is usually empty, and an empty one leaves `voices.md` as a file
  // containing nothing. The blank-run collapse below removes the gap, so a user
  // with no secondary voices pays nothing for a feature they have not set up.
  if (const size_t at = text.find("{{voices}}"); at != std::string::npos) {
    text.replace(at, std::strlen("{{voices}}"), trim_end(g_voices_digest));
  }
  // A path, not a digest, and the fallback is prose rather than nothing: an
  // unset one still has to read as a sentence, because a human may be reading
  // this file too.
  if (const size_t at = text.find("{{scripts_dir}}"); at != std::string::npos) {
    text.replace(at, std::strlen("{{scripts_dir}}"),
                 g_scripts_dir.empty() ? std::string("%APPDATA%\\AIInterface\\scripts")
                                       : g_scripts_dir);
  }
  std::string composed = expand_tool_sections(text, policy, &section_problems);
  for (const std::string& p : section_problems) std::fprintf(stderr, "[prompts] %s\n", p.c_str());
  // Appended, never substituted, and last so that it has the final word. Not
  // expanded: the file next to the exe is the user's own prose and nothing
  // here rewrites it.
  if (const std::string local = local_prompt(); !local.empty()) {
    if (!composed.empty()) composed += kSeparator;
    composed += local;
  }
  cached = std::move(composed);
  cached_for = policy;
  cached_digest = g_settings_digest;
  cached_actions = g_actions_digest;
  cached_voices = g_voices_digest;
  have = true;
  return cached;
}

// ------------------------------------------------------- M5.2: the inventory

const char kCliSource[] = "Claude Code CLI";
const char kNotManagedHere[] = "not managed here";

std::vector<PromptRow> cli_context_rows() {
  // Measured, not guessed. M3.5 audited a live model rather than reading
  // `--help`, and these are the things it could still recite after
  // `--system-prompt`, `--safe-mode` and `--disable-slash-commands` had all
  // been applied. They are listed one per item rather than as a single
  // "CLI preamble" row because the user's own email address being in there is
  // a specific fact they should be able to see, not a footnote inside a
  // summary.
  //
  // No values are shown. This window answers "what is in Claude's head", and
  // for these rows the honest answer is the category — the values change with
  // the directory and the day, and printing a stale one would be its own small
  // lie. The row's job is to stop the list above it being read as complete.
  static const char* const kItems[] = {
      "Harness preamble (tool and safety instructions)",
      // M3.7. Not irreducible like the rest of this list — this app chose it,
      // in engines.cpp — but it is delivered the same way, as tool schemas the
      // CLI writes into the context, and a reader asking "what is in Claude's
      // head" should not have to infer it from the fact that answers sometimes
      // come back current.
      "Tool definitions for whatever is ticked in Settings > Tools",
      "Working directory",
      "Git status of the working directory",
      "Platform and OS version",
      "Model id",
      "Token budget",
      "Today's date",
      "Your account email address",
  };
  std::vector<PromptRow> rows;
  for (const char* item : kItems) {
    PromptRow r;
    r.section = PromptSection::Cli;
    r.title = item;
    r.source = kCliSource;
    r.injected = true;          // from the child's first token
    r.at_session_start = true;  // it *is* the session's start
    r.at = 0.0;
    rows.push_back(std::move(r));
  }
  return rows;
}

PromptInventory build_inventory(const PromptStore& store, const PromptInjector& injector,
                                const std::vector<std::pair<std::string, double>>& loaded_at) {
  PromptInventory inv;
  inv.ready = true;
  inv.rows = cli_context_rows();

  // ---- Global: the composed system prompt, in the order it was composed ----
  //
  // Composition order rather than file order, because that is the order Claude
  // read them in and this window claims to show what Claude read.
  if (const PromptGraph* g = store.graph("system")) {
    for (const std::string& id : store.compose_order("system")) {
      const PromptNode* n = g->find(id);
      if (!n) continue;
      PromptRow r;
      r.section = PromptSection::Global;
      r.title = n->title.empty() ? n->id : n->title;
      r.source = n->file.empty() ? n->id : n->file;
      r.est_tokens = estimate_tokens(n->body);
      r.injected = true;
      r.at_session_start = true;
      inv.rows.push_back(std::move(r));
    }
    // A global prompt whose body could not be read is *not* in the composed
    // order -- an empty body drops out of `compose_order()` -- so without this
    // it would not appear here at all, and a window whose whole purpose is to
    // say what is in Claude's head would answer a question about a declared
    // prompt with silence. It is listed where the user declared it, marked
    // failed, injected false, tokens unknown: every one of those is true.
    for (const PromptNode& n : g->nodes) {
      // Disabled nodes are left out for the same reason they are left out of
      // composition: the user switched them off, so their body not being there
      // is not a failure of anything.
      if (n.kind != PromptKind::Global || !n.enabled || n.body_error.empty()) continue;
      PromptRow r;
      r.section = PromptSection::Global;
      r.title = n.title.empty() ? n.id : n.title;
      r.source = n.file.empty() ? n.id : n.file;
      r.failed = true;
      r.injected = false;
      r.at_session_start = true;
      inv.rows.push_back(std::move(r));
    }
  }
  // `pre-prompt.md` beside the exe is part of the system prompt and is the one
  // piece of it the user was explicitly invited to edit, so leaving it out
  // would be the same omission as leaving out the CLI's rows, one file closer
  // to home. It is last because it is appended last.
  if (!local_prompt().empty()) {
    PromptRow r;
    r.section = PromptSection::Global;
    r.title = "Pre-prompt";
    r.source = local_prompt_path().string();
    r.est_tokens = estimate_tokens(local_prompt());
    r.injected = true;
    r.at_session_start = true;
    inv.rows.push_back(std::move(r));
  }

  // ---- Project and Skill: everything injectable, loaded or not ----
  //
  // Taken by `kind` and not by which graph a node sits in, for the same reason
  // PromptInjector::reset does: the grouping the store chooses must not be
  // able to change what this window says. Nodes that have not fired are
  // carried with `injected == false` — M5.2 does not draw them, M5.4 greys
  // them in, and neither needs a second enumeration.
  for (const PromptGraph& g : store.graphs()) {
    for (const PromptNode& n : g.nodes) {
      if (n.kind == PromptKind::Global || !n.enabled) continue;
      PromptRow r;
      r.section = n.kind == PromptKind::Project ? PromptSection::Project : PromptSection::Skill;
      r.title = n.title.empty() ? n.id : n.title;
      r.source = n.file.empty() ? n.id : n.file;
      r.triggers = n.triggers;
      // Estimated whether or not it has fired: an unloaded row shows what it
      // *would* cost, which is half of why M5.4 lists it at all.
      // Same for a project or skill prompt: a body that could not be read can
      // never be injected, however often its trigger words are said, so the
      // row says failed rather than "available, not loaded".
      if (!n.body_error.empty()) {
        r.failed = true;
        r.injected = false;
        r.at_session_start = false;
        inv.rows.push_back(std::move(r));
        continue;
      }
      r.est_tokens = estimate_tokens(n.body);
      r.injected = injector.is_loaded(n.id);
      // Injected *during* the session unless the caller says otherwise: these
      // are never part of the launch argument, so "session start" would be
      // wrong even for one that fired on the first turn.
      r.at_session_start = false;
      for (const auto& [id, when] : loaded_at)
        if (id == n.id) r.at = when;
      inv.rows.push_back(std::move(r));
    }
  }
  return inv;
}

}  // namespace aii
