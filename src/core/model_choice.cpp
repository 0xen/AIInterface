#include "core/model_choice.h"

namespace aii {
namespace {

// The table. See model_choice.h for why aliases and not dated ids, and for the
// four probes each of these came from.
//
// Four rows, not ten. The CLI would take any full model name, and a text field
// would let the user type one — but a free-text model field is a field in
// which a typo starts a child that cannot answer, and the app has no way to
// tell them so until the first turn has already failed. Every row here is one
// that completed a turn on this machine. `AII_MODEL` is still the escape hatch
// for anyone who wants to pin something else by hand, and the surface says
// when it is in force rather than quietly showing the wrong row.
const ModelChoice kChoices[kModelChoiceCount] = {
    {"default", "Default", "", "claude-opus-5",
     "Whatever the Claude Code CLI would pick on its own, which is\n"
     "what this app did before the setting existed. On this login\n"
     "that is currently Opus."},
    {"opus", "Opus", "opus", "claude-opus-5",
     "The capable one, and the slowest and dearest per turn.\n"
     "A 1M-token context window, so a long conversation lasts."},
    {"sonnet", "Sonnet", "sonnet", "claude-sonnet-5",
     "The middle one: quicker to answer than Opus and cheaper\n"
     "per turn, which is worth having in a voice conversation\n"
     "where you are waiting for the reply out loud."},
    {"haiku", "Haiku", "haiku", "claude-haiku-4-5",
     "The fast, cheap one, with a 200K context window rather\n"
     "than 1M. Good for chatting and for dispatching workers;\n"
     "it is the weakest of the three at anything hard."},
};

}  // namespace

// M31. Rewritten: workers used to be untouched by any model setting at all
// and this said so. They now have their own row (Worker model, below this
// one), so the sentence has to say where to look instead of that nothing is
// there to look at.
const char kModelWorkersNote[] =
    "This is the model of the one you are talking to. Workers are separate `claude` "
    "processes with their own grant -- see Worker model below.";

const ModelChoice& model_choice(int id) {
  if (id < 0 || id >= kModelChoiceCount) return kChoices[kModelChoiceDefault];
  return kChoices[id];
}

int model_choice_for_key(const std::string& key) {
  for (int i = 0; i < kModelChoiceCount; ++i)
    if (key == kChoices[i].key) return i;
  return -1;
}

int model_choice_for_arg(const std::string& arg) {
  for (int i = 0; i < kModelChoiceCount; ++i)
    if (arg == kChoices[i].arg) return i;
  return -1;
}

std::string model_label(const std::string& arg) {
  if (arg.empty()) return "the CLI's default";
  const int i = model_choice_for_arg(arg);
  return i >= 0 ? kChoices[i].label : arg;
}

std::string model_api_id(const std::string& arg) {
  const int i = model_choice_for_arg(arg);
  if (i >= 0) return kChoices[i].api_id;
  return arg;  // a hand-set AII_MODEL: it was always a full id on this backend.
}

}  // namespace aii
