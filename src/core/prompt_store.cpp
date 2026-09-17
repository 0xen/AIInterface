#include "core/prompt_store.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <system_error>

#include "core/config.h"
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
  const fs::path dir = root();
  // Seeded every start, not only when the directory is absent — see
  // `seed_tree`, and commit 636f24e for what the other rule cost.
  std::string seed_err;
  if (!seed_tree(fs::path(AII_ASSETS_DIR) / "prompts", dir, &seed_err) && error)
    *error = seed_err;  // not fatal: an existing store still loads

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
        if (error) *error = "prompt `" + n.id + "`: cannot read " + n.file;
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

std::string PromptStore::compose(const std::string& name) const {
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
      if (const std::string body = trim_end(n->body); !body.empty()) parts.push_back(body);
    }

    std::vector<const PromptLink*> out;
    for (const PromptLink& l : g->links)
      if (l.enabled && l.from == id) out.push_back(&l);
    std::sort(out.begin(), out.end(), [](const PromptLink* a, const PromptLink* b) {
      return a->order != b->order ? a->order < b->order : a->to < b->to;
    });
    for (auto it = out.rbegin(); it != out.rend(); ++it) stack.push_back((*it)->to);
  }

  std::string composed;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) composed += kSeparator;
    composed += parts[i];
  }
  return composed;
}

const std::string& system_prompt() {
  static const std::string kComposed = [] {
    PromptStore store;
    std::string err;
    if (!store.load(&err) && !err.empty()) std::fprintf(stderr, "[prompts] %s\n", err.c_str());
    return store.compose("system");
  }();
  return kComposed;
}

}  // namespace aii
