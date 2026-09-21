#pragma once
// Where this app's per-user data lives, and the one rule for getting shipped
// assets into it.
//
// Both halves were already in the tree, in `src/avatar/avatar_def.cpp`, and
// M3's prompt store needed exactly the same two things — but from `aii_core`,
// which the avatar target sits *above*, so it could not call them. Rather than
// a second copy of a rule this project has already got wrong once (see
// `seed_tree` below), they moved down here; `avatar_user_root()` and
// `seed_avatar_definition()` are now two-line wrappers over these.
#include <filesystem>
#include <string>
#include <vector>

namespace aii {

// The directory holding the running executable.
//
// This is the *per-install* root, as distinct from `user_data_root()`'s
// per-user one, and the two answer different questions. `%APPDATA%` survives a
// rebuild and follows the user between installs; the exe's own folder is what
// travels when the app is copied to another machine or handed to someone else,
// and it is the folder a user opens when they go looking for "the file next to
// the exe". Something meant to travel *with the app* belongs here; something
// meant to outlive a given copy of the app belongs in `user_data_root()`.
//
// Falls back to the current directory if the OS will not say, so a caller can
// always compose a path against it.
std::filesystem::path exe_dir();

// `%APPDATA%\AIInterface`, the roaming per-user root. `settings.json`,
// `avatars/` and `prompts/` are all directly under it. Falls back to a
// relative path when APPDATA is somehow unset, which keeps a headless or
// service context from writing to the filesystem root.
std::filesystem::path user_data_root();

// M14. `%APPDATA%\AIInterface\memories.md`: what the user has asked the
// assistant to remember (`core/memory_store.h`). Here rather than in the store
// so that the store stays standard-library-only and testable against a temp
// file, while the two callers that need the real path (main.cpp at launch,
// the session on `remember`) cannot derive it differently.
std::filesystem::path memories_path();

// Copy a shipped asset tree into the user's copy, deciding file by file from
// the *bytes*, never from the timestamps.
//
// This rule has been wrong twice, in opposite directions, and both failures are
// worth keeping in view.
//
//   * "copy it only if the destination does not exist" is what this project
//     shipped until 636f24e. It is a silent one-way door: the copy runs exactly
//     once, so anything added to `assets/` afterwards never reaches a machine
//     that has already started the app. It cost a round on 16 Sep 2026, when a
//     sprite was authored, declared, wired up and simply never appeared.
//   * `copy_options::update_existing` replaced it, and the promise this comment
//     used to make for it — "a file the user has edited keeps their edit,
//     because their copy is the newer one" — was not true. Git sets mtime to
//     *checkout* time, so any commit touching a shipped prompt makes the
//     shipped copy newer than a hand edit made weeks earlier, and the edit is
//     silently overwritten. The reverse holds too: one edit blocks every later
//     upstream refresh until upstream happens to change again.
//
// So a small manifest, `.seeded`, lives inside each destination tree, recording
// per relative path the hash of the shipped bytes that destination was last
// reconciled against. `user_paths.cpp` explains the format and why it sits
// there rather than once per install. With it, the decision is exact:
//
//   destination missing                                -> copied, hash recorded
//   destination already equals the shipped bytes        -> nothing, hash recorded
//   destination equals the record, shipped differs      -> refreshed
//   destination differs, shipped unchanged since record -> left alone, silently
//   destination differs and shipped changed too         -> left alone; the
//       shipped file is written beside it as `<name>.new` and one line about it
//       is appended to `notes`
//
// A file with *no* record — which is every file on every install predating this
// — is treated as possibly edited: when it differs from the shipped bytes it
// gets the `.new` treatment rather than being overwritten. The first run after
// an upgrade therefore records hashes for everything that already matches
// (silently, which is nearly all of it) and leaves a `.new` beside anything
// that does not. Nothing is ever deleted, and a file the user added themselves
// is never touched.
//
// `notes` collects that one line per `.new`, which is the only thing this
// function has to say out loud; every caller logs it. `error` is still only for
// a genuine failure — a file that cannot be read or written — and a missing
// source is not one, because the caller is always in a better position to say
// what a missing asset means than this function is.
bool seed_tree(const std::filesystem::path& source, const std::filesystem::path& dest,
               std::string* error = nullptr, std::vector<std::string>* notes = nullptr);

}  // namespace aii
