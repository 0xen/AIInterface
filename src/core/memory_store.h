#pragma once
// M14. What the user has asked the assistant to remember, as a file.
//
// The conversational instance forgets everything at every restart -- reset,
// model change, handoff, launch -- and that is deliberate: it runs with
// `--no-session-persistence` and `--safe-mode`, the second of which shuts the
// CLI's own auto-memory out on purpose (engines.cpp, M3.5). So "remember this"
// has to be the *app's* memory, and this is it: one plain file under the
// user's data folder, one memory per line, read into the system prompt at
// every launch and restart through the same `{{memories}}` slot mechanism the
// settings and scripts lists already use.
//
// ## The file
//
//     1. 2026-09-21 The user prefers metric units.
//     2. 2026-09-21 Their cat is called Miso.
//
// `<id>. <ISO date> <text>`. It is meant to be opened and edited by hand: a
// line the user adds without a number gets one the next time the file is
// written, a line they delete is gone, a blank line or a `#` comment is
// ignored. The date is when it was saved, for the human reader; the model
// sees it too, because "the user said this a month ago" and "this morning"
// are different facts.
//
// ## Ids never renumber
//
// The model forgets by id, and the id it uses is the one it read off the
// prompt at the last restart or the one the app just told it. If removing
// memory 2 renumbered 3 to 2, the next `forget id=3` would silently delete
// the wrong line. So an id is issued once (max + 1) and a gap stays a gap.
//
// ## The cap
//
// Every line is in the system prompt on every turn, so the list is bounded
// and the bound is in characters of text, which is roughly what it costs.
// Over it, `append` refuses and says why, and the app says so out loud rather
// than dropping the oldest -- the user asked for something to be kept, and
// the honest answer is "I am full", not a quiet eviction.
//
// Standard-library-only, like the other core policy files, so the test needs
// no engine, no window and no CLI: it points the store at a temp file.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aii {

struct Memory {
  std::uint64_t id = 0;
  std::string date;  // YYYY-MM-DD, the day it was saved
  std::string text;  // one line, no newlines
};

// Total characters of `text` across all memories the file may hold. About a
// thousand tokens of prompt at the worst; roomy for a person, and small
// enough that nobody notices it on a turn.
inline constexpr std::size_t kMemoryTextCap = 4000;
// One memory's text. Longer than a sentence a person would dictate, shorter
// than a paragraph the model might try to file as one item.
inline constexpr std::size_t kMemoryLineCap = 500;

class MemoryStore {
 public:
  // The store is bound to a path and reads it lazily on first use; nothing
  // touches the disk in the constructor, so building one is free and safe on
  // any thread.
  explicit MemoryStore(std::filesystem::path file);

  // Re-read the file. Called by `append`/`remove` before they write, so a hand
  // edit made while the app was running is never overwritten with the copy
  // the app had in memory. Returns false only when the file exists and could
  // not be read; a missing file is an empty store, not an error.
  bool load(std::string* error = nullptr);

  // Add one memory. `text` is trimmed and flattened to one line; empty text,
  // text over `kMemoryLineCap`, and a store already at `kMemoryTextCap` are
  // refused with `why` set to a short human-readable reason. On success `id`
  // is the new memory's id and the file has been written.
  bool append(const std::string& text, const std::string& date, std::uint64_t* id,
              std::string* why = nullptr);

  // Remove by id. False when no memory has that id; the file is not touched.
  bool remove(std::uint64_t id, std::string* why = nullptr);

  // The list, as the model reads it in the prompt: one `<id>. (<date>) <text>`
  // per line, or a single sentence saying there is nothing yet. Never empty,
  // so a `{{memories}}` slot always substitutes to a sentence.
  std::string digest();

  // Everything currently held, after a load.
  const std::vector<Memory>& all();
  // Characters of text in use, against `kMemoryTextCap`.
  std::size_t text_size();
  bool full();

  const std::filesystem::path& path() const { return file_; }

  // Today's date as YYYY-MM-DD, in local time. Public so the caller that
  // knows the date is being tested can pass a fixed one instead.
  static std::string today();

 private:
  bool save(std::string* error);
  void ensure_loaded();

  std::filesystem::path file_;
  std::vector<Memory> items_;
  bool loaded_ = false;
  // Set when a line in the file had no id and was given one on load: the
  // next save normalises the file, and the id it was given is stable from
  // then on.
  bool dirty_ = false;
};

}  // namespace aii
