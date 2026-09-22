#pragma once
// M3.11. Which Claude model the *conversational* instance runs on, as a small
// table that the settings surface, `build_llm` and `main.cpp` all read — the
// same shape `core/tool_policy.h` uses, and for the same reason: the decision
// that matters is which values are on the list, and it should be visible in
// one place rather than spelled out in three.
//
// **Aliases, not dated ids.** `claude --help` on 2.1.275 says `--model` takes
// "an alias for the latest model (e.g. 'fable', 'opus', or 'sonnet') or a
// model's full name". A pinned dated id rots — the day it is retired the app
// stops being able to start a turn, and the user has no way to find out why
// from inside the window. An alias is resolved by the CLI at launch and keeps
// following the current model, so the list stays right without this file being
// edited. That is the whole justification for the `arg` column below.
//
// **Every value here was verified by completing a turn on it**, against
// 2.1.275, with the flags this app actually uses (`--safe-mode
// --disable-slash-commands --tools ""`), by reading the `canonicalModel` the
// CLI reports back:
//
//   (no --model)   -> claude-opus-5      (the CLI's own default for this login)
//   --model opus   -> claude-opus-5
//   --model sonnet -> claude-sonnet-5
//   --model haiku  -> claude-haiku-4-5
//
// `fable` is a real alias and is deliberately **not** on the list: the probe
// came back `429 ... You've reached your Fable limit`, so a turn could not be
// completed on it here. A picker entry that cannot be shown to work is exactly
// the failure this table exists to avoid — the app would fail to reply at the
// next launch and the window would not be able to say why.
//
// **The empty `arg` is the point of the first row.** Passing no `--model` at
// all leaves the choice to the CLI, which is what this app did before this
// setting existed and is still the default. It is a legal and reachable state,
// spelled `""`, the same way `tool_list()` spells an empty grant.
//
// **Workers do not read this row.** They have their own, `model.worker`
// (M31, `WorkerPool::set_model`), which draws on this same table but defaults
// to `opus` rather than to the CLI's own default; see the note on
// `kModelWorkersNote` and the Worker model row under the Model picker.
#include <string>

namespace aii {

// In the order the picker draws them. `kModelChoiceDefault` is index 0 both
// because it is the default and because it is what every fallback lands on.
enum ModelChoiceId {
  kModelChoiceDefault = 0,
  kModelChoiceOpus,
  kModelChoiceSonnet,
  kModelChoiceHaiku,
  kModelChoiceCount,
};

struct ModelChoice {
  // The settings.json value, under "model" / "name". On-disk format, so it has
  // to stay stable: it is what a person hand-editing the file reads and types.
  // "default" rather than "" because `Settings::set_string` ignores an empty
  // value, and a key that cannot be written is a setting that cannot be
  // returned to.
  const char* key;
  const char* label;  // the row's picker entry
  // What goes on `claude --model`. **Empty means the flag is not passed**,
  // which is how "whatever the CLI would pick" is expressed.
  const char* arg;
  // The full model id for the `api` backend, which is the raw Messages API and
  // has never understood an alias — `{"model": "opus"}` is a 404 there. These
  // are the canonical ids the CLI itself reported for each alias above, and
  // the default row carries the id this file's `build_llm` already hardcoded
  // before this table existed, so that path is unchanged by the new setting.
  const char* api_id;
  const char* tip;
};

const ModelChoice& model_choice(int id);

// The index for a settings.json value, or -1 when the file names something
// this build has never heard of — a value from a newer version, a typo, or an
// alias that has since been dropped from the table. The caller falls back to
// `kModelChoiceDefault` and says so: a model string the CLI rejects means the
// child starts and every turn fails, which is the worst outcome this setting
// has, and the CLI's own default is the one value that cannot be wrong.
int model_choice_for_key(const std::string& key);

// The index whose `arg` is exactly this, or -1. Used to read `AII_MODEL` back
// into the picker, so an environment override that happens to name a listed
// alias shows up as that row rather than as an unexplained mismatch.
int model_choice_for_arg(const std::string& arg);

// A `--model` argument as a person reads it: the table's label when it is one
// of ours, the raw string otherwise (an `AII_MODEL` naming a dated id, say),
// and "the CLI's default" for the empty string. Never returns an empty
// string, because this goes straight into a sentence.
std::string model_label(const std::string& arg);

// The Messages API id for a `--model` argument. An alias is translated through
// the table; anything else is passed through untouched, since a hand-set
// `AII_MODEL` on the `api` backend was always a full id and still should be.
std::string model_api_id(const std::string& arg);

// One line under the picker. Workers are a separate client with a separate
// grant and this control does not reach them — said on screen rather than left
// for someone to discover from a bill.
extern const char kModelWorkersNote[];

}  // namespace aii
