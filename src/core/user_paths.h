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

namespace aii {

// `%APPDATA%\AIInterface`, the roaming per-user root. `settings.json`,
// `avatars/` and `prompts/` are all directly under it. Falls back to a
// relative path when APPDATA is somehow unset, which keeps a headless or
// service context from writing to the filesystem root.
std::filesystem::path user_data_root();

// Copy a shipped asset tree into the user's copy, refreshing a file only when
// the shipped one is *newer*.
//
// This is the installer rule and it is deliberate. The obvious alternative —
// "copy it if the destination does not exist" — was what this project shipped
// until commit 636f24e, and it is a silent one-way door: the copy runs exactly
// once, so anything added to `assets/` afterwards never reaches a machine that
// has already started the app. It cost a round on 16 Sep 2026, when a sprite
// was authored, declared, wired up and simply never appeared, because the
// definition being read was the one seeded days earlier.
//
// `update_existing` keeps the property the early return was reaching for — a
// file the user has edited since the last release keeps their edit, because
// their copy is the newer one — while `recursive` still adds files that are
// not there yet, which is most of what this ever has to do.
//
// Returns false only when the copy itself failed; a missing source is not an
// error here, because the caller is always in a better position to say what a
// missing asset means than this function is.
bool seed_tree(const std::filesystem::path& source, const std::filesystem::path& dest,
               std::string* error = nullptr);

}  // namespace aii
