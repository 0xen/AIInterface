#pragma once
// The button bar's registry (M1c.1): what the panel draws in the row above the
// status line. Two built-in buttons are simply its first two entries, so the
// panel has one loop and no special cases — adding a button later, from
// anywhere, means adding an entry rather than editing layout code.
//
// It lives in aii_core rather than next to the panel because it has two
// producers already and a third coming: the built-ins here, the ```aii``` block
// the assistant ends a reply with (M1c.2), and M2.5's app bus, which the
// milestones say must reuse this registry rather than grow a second one. The
// panel is only a consumer.
//
// **Registered buttons are untrusted input.** Everything a script can set is
// validated here, at the one entry point, rather than at each producer: the id
// shape, the label length, how many buttons there may be, and above all that
// the action is one of a closed set of kinds this app implements — there is no
// "run this command string" kind and there is not going to be one.
//
// **Registered buttons do not persist across runs** (decided M1c.2). They
// describe the project the agent is working on right now and the agent
// re-registers them in the turn that needs them, which is cheap; a persisted
// button is a path that was validated once, months ago, for a directory the
// user has moved on from — it either opens something irrelevant or fails at
// the click. The settings file is for preferences the *user* set, not for an
// agent's scratch state, so nothing here is written to it. The built-ins are
// rebuilt from the process's own working directory on every start, which is
// the value that can actually change between runs.
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace aii {

// What a button does when it is clicked: a tagged value, not a callback, so
// that the set of things a *registered* button can do stays closed. Adding a
// kind later (open a URL, run a named app action, switch a setting) is a case
// here and a case in the panel's dispatch; it does not reshape the registry or
// widen what a script may ask for.
enum class ButtonActionKind {
  OpenSettings,  // M1c.3 fills this in; today it opens a placeholder popup
  OpenPath,      // opens `path` in Explorer — the only kind a script may ask for
};

struct ButtonAction {
  ButtonActionKind kind = ButtonActionKind::OpenPath;
  // OpenPath only. Validated at registration: it must already exist and be a
  // directory, or the button is refused rather than added, so a click can
  // never be the first time anyone finds out the path was nonsense.
  std::string path;
};

// There is no icon font in this build and merging one is M4's job, so a button
// either draws one of the hand-drawn vector glyphs (the same register as the
// chat arrow and the avatar-mode disc beside it) or shows a short text label.
// Registered buttons are always `Label`: a script cannot invent a glyph, and
// letting it pick from the built-in ones would only produce two cogs.
enum class ButtonGlyph { Label, Cog, Folder };

struct ToolbarButton {
  std::string id;
  ButtonGlyph glyph = ButtonGlyph::Label;
  std::string label;    // drawn when glyph == Label
  std::string tooltip;
  ButtonAction action;
  bool builtin = false;
};

// The caps, in one place because they are a layout contract as much as a
// security one. The widget is 360 px wide; after the window padding and the
// two built-in squares that leaves roughly 290 px, so four labels of eight
// characters at 15 px text fit on the one row with room to spare. That is the
// failure mode the milestone names — a registered button must never push the
// transport buttons off the widget — and the panel enforces it a second time
// at draw, dropping anything that would not fit whatever the font measures.
constexpr std::size_t kButtonsMax = 4;         // registered buttons, built-ins excluded
constexpr std::size_t kButtonLabelMax = 8;     // in characters, not bytes
constexpr std::size_t kButtonTooltipMax = 96;  // ditto; a tooltip is one line
constexpr std::size_t kButtonIdMax = 32;

class ButtonRegistry {
 public:
  // Process-wide, because its producers are: the panel on the main thread, the
  // turn thread parsing a reply, and later the bus. One store, one mutex.
  static ButtonRegistry& instance();

  // Registers (or replaces) one button from untrusted input. Returns false and
  // fills `error` with one short line when the button is refused; a refusal
  // leaves the registry exactly as it was.
  //
  // A duplicate id **replaces** the existing button in place rather than
  // accumulating beside it, and keeps its position in the row — an assistant
  // that mentions the same button in two consecutive turns is the normal case,
  // not a mistake, and a bar that grows a second Git button every turn is the
  // failure this is designed out of. Replacing also means the cap is never
  // reached by re-registration.
  bool add_path_button(const std::string& id, const std::string& label,
                       const std::string& tooltip, const std::string& path,
                       std::string* error);

  // A copy, because the panel iterates it for a whole frame while the turn
  // thread may be registering into it. The list is tiny by construction.
  std::vector<ToolbarButton> snapshot() const;

  // Drops every registered button, keeping the built-ins. Nothing calls this
  // yet; it is what a future `button clear` verb or a project switch needs.
  void clear_registered();

  // Refusals since the last call, for the log, and emptied by it. Same purpose
  // as Settings::take_status_change(): the producer has no console of its own,
  // and a button that was quietly dropped is exactly the thing that is
  // impossible to debug later. A queue rather than one line because a single
  // block can be refused several times over — one bad path and three over the
  // cap — and the last one alone would hide the rest.
  std::vector<std::string> take_status();

 private:
  ButtonRegistry();

  mutable std::mutex mutex_;
  std::vector<ToolbarButton> buttons_;
  std::vector<std::string> status_;
};

// Opens a directory in Explorer. Separate from the registry because the
// registry is a store and this is a side effect on the user's desktop, and
// because the panel dispatches on the action kind — `OpenSettings` has no
// business coming through here.
bool open_directory(const std::string& path, std::string* error);

}  // namespace aii
