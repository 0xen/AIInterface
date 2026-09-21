#include "core/memory_store.h"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace aii {
namespace {

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// One line, single-spaced: a memory is a line in a file and a line in a
// prompt, and a newline in it would be a second memory to one reader and a
// broken one to the other.
std::string flatten(const std::string& s) {
  std::string out;
  bool space = false;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      space = true;
      continue;
    }
    if (space && !out.empty()) out.push_back(' ');
    space = false;
    out.push_back(c);
  }
  return out;
}

bool looks_like_date(const std::string& s) {
  if (s.size() != 10) return false;
  for (size_t i = 0; i < 10; ++i) {
    if (i == 4 || i == 7) {
      if (s[i] != '-') return false;
    } else if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
      return false;
    }
  }
  return true;
}

// `<id>. <date> <text>` -> a Memory. A line that does not open with `<id>.`
// is a memory with no id (a hand-written one); a line that has the id but no
// date gets an empty date. Anything else the line says is its text.
Memory parse_line(const std::string& raw) {
  Memory m;
  std::string line = trim(raw);
  size_t i = 0;
  while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
  if (i > 0 && i < line.size() && line[i] == '.') {
    m.id = std::strtoull(line.substr(0, i).c_str(), nullptr, 10);
    line = trim(line.substr(i + 1));
  }
  if (line.size() >= 10 && looks_like_date(line.substr(0, 10)) &&
      (line.size() == 10 || std::isspace(static_cast<unsigned char>(line[10])))) {
    m.date = line.substr(0, 10);
    line = trim(line.substr(10));
  }
  m.text = flatten(line);
  return m;
}

}  // namespace

MemoryStore::MemoryStore(fs::path file) : file_(std::move(file)) {}

std::string MemoryStore::today() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return buf;
}

bool MemoryStore::load(std::string* error) {
  items_.clear();
  dirty_ = false;
  loaded_ = true;
  std::error_code ec;
  if (!fs::exists(file_, ec)) return true;  // nothing remembered yet
  std::ifstream in(file_, std::ios::binary);
  if (!in) {
    if (error) *error = "could not read " + file_.string();
    return false;
  }
  std::uint64_t max_id = 0;
  std::vector<Memory> unnumbered;
  std::string raw;
  while (std::getline(in, raw)) {
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    const std::string line = trim(raw);
    if (line.empty() || line[0] == '#') continue;
    Memory m = parse_line(line);
    if (m.text.empty()) continue;
    if (m.id == 0) {
      unnumbered.push_back(std::move(m));
      continue;
    }
    // A duplicated id -- two hand edits, or a copy-paste -- is resolved by
    // treating the second as unnumbered. Nothing is lost and both become
    // addressable.
    bool dup = false;
    for (const Memory& have : items_)
      if (have.id == m.id) dup = true;
    if (dup) {
      m.id = 0;
      unnumbered.push_back(std::move(m));
      continue;
    }
    if (m.id > max_id) max_id = m.id;
    items_.push_back(std::move(m));
  }
  for (Memory& m : unnumbered) {
    m.id = ++max_id;
    if (m.date.empty()) m.date = today();
    items_.push_back(std::move(m));
    dirty_ = true;  // the file will be normalised on the next write
  }
  return true;
}

void MemoryStore::ensure_loaded() {
  if (!loaded_) load();
}

bool MemoryStore::save(std::string* error) {
  std::error_code ec;
  fs::create_directories(file_.parent_path(), ec);
  // Written whole, to a sibling, then renamed: a crash mid-write leaves the
  // old file, not half of the new one.
  const fs::path tmp = file_.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = "could not write " + tmp.string();
      return false;
    }
    out << "# What the assistant has been asked to remember. One per line:\n"
           "# <id>. <date saved> <the memory>. Edit freely; a line without an id gets one.\n";
    for (const Memory& m : items_) out << m.id << ". " << m.date << ' ' << m.text << '\n';
  }
  fs::rename(tmp, file_, ec);
  if (ec) {
    // `rename` over an existing file is allowed on Windows only since C++17's
    // std::filesystem uses MoveFileEx with replace; be defensive anyway.
    fs::remove(file_, ec);
    fs::rename(tmp, file_, ec);
  }
  if (ec) {
    if (error) *error = "could not replace " + file_.string() + ": " + ec.message();
    return false;
  }
  dirty_ = false;
  return true;
}

std::size_t MemoryStore::text_size() {
  ensure_loaded();
  std::size_t n = 0;
  for (const Memory& m : items_) n += m.text.size();
  return n;
}

bool MemoryStore::full() { return text_size() >= kMemoryTextCap; }

bool MemoryStore::append(const std::string& text, const std::string& date, std::uint64_t* id,
                         std::string* why) {
  // Re-read first: a hand edit since the last look must not be overwritten
  // with a stale copy.
  std::string err;
  if (!load(&err)) {
    if (why) *why = err;
    return false;
  }
  const std::string clean = flatten(trim(text));
  if (clean.empty()) {
    if (why) *why = "nothing to remember: the text was empty";
    return false;
  }
  if (clean.size() > kMemoryLineCap) {
    if (why) *why = "too long for one memory (" + std::to_string(clean.size()) + " > " +
                    std::to_string(kMemoryLineCap) + " characters)";
    return false;
  }
  const std::size_t have = text_size();
  if (have + clean.size() > kMemoryTextCap) {
    if (why) *why = "full: " + std::to_string(have) + " of " + std::to_string(kMemoryTextCap) +
                    " characters used, " + std::to_string(clean.size()) + " more would not fit";
    return false;
  }
  std::uint64_t max_id = 0;
  for (const Memory& m : items_)
    if (m.id > max_id) max_id = m.id;
  Memory m;
  m.id = max_id + 1;
  m.date = date.empty() ? today() : date;
  m.text = clean;
  items_.push_back(m);
  if (!save(&err)) {
    items_.pop_back();
    if (why) *why = err;
    return false;
  }
  if (id) *id = m.id;
  return true;
}

bool MemoryStore::remove(std::uint64_t id, std::string* why) {
  std::string err;
  if (!load(&err)) {
    if (why) *why = err;
    return false;
  }
  for (std::size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].id != id) continue;
    const Memory gone = items_[i];
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
    if (!save(&err)) {
      items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(i), gone);
      if (why) *why = err;
      return false;
    }
    return true;
  }
  if (why) *why = "no memory has id " + std::to_string(id);
  return false;
}

const std::vector<Memory>& MemoryStore::all() {
  ensure_loaded();
  return items_;
}

std::string MemoryStore::digest() {
  ensure_loaded();
  if (items_.empty()) return "(nothing yet -- the user has not asked you to remember anything)";
  std::string out;
  for (const Memory& m : items_) {
    out += std::to_string(m.id);
    out += ". (";
    out += m.date.empty() ? std::string("undated") : m.date;
    out += ") ";
    out += m.text;
    out += '\n';
  }
  return out;
}

}  // namespace aii
