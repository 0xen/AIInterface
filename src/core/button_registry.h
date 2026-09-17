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
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace aii {

// Where a button is drawn (M4.4). The sidebar is a real second OS window down
// the left edge of the widget; the toolbar is the row inside the panel.
//
// The two surfaces are not interchangeable and the difference is what a button
// *is*, not where it happens to sit: the sidebar is 48 px of icon, so only a
// button with a pixel icon can live there, and the toolbar is a 360 px row of
// text, which is the only place an agent's `Projects` label can be read. A
// button therefore states its surface once, here, and neither drawing site has
// a list of its own to keep in step.
enum class ButtonSurface {
  Toolbar,  // the row in the panel: text labels, agent-registered buttons
  Sidebar,  // the strip beside the widget: icon-only, one pixel glyph each
};

// What a button does when it is clicked: a tagged value, not a callback, so
// that the set of things a *registered* button can do stays closed. Adding a
// kind later (open a URL, run a named app action, switch a setting) is a case
// here and a case in the panel's dispatch; it does not reshape the registry or
// widen what a script may ask for.
enum class ButtonActionKind {
  OpenSettings,  // toggles the panel's settings region
  OpenPath,      // opens `path` in Explorer — the only kind a script may ask for
  // M4.4. An in-process callback, reachable only through add_app_button() and
  // therefore never from anything a script or a reply can say. This is the
  // kind M5's inspector and M6's editor use: a window registers its own
  // button, with its own icon and its own toggle, and no layout code anywhere
  // learns that it exists. It does not widen what a *registered* button may
  // do, because the untrusted entry point (add_path_button) cannot produce it.
  Invoke,
};

struct ButtonAction {
  ButtonActionKind kind = ButtonActionKind::OpenPath;
  // OpenPath only. Validated at registration: it must already exist and be a
  // directory, or the button is refused rather than added, so a click can
  // never be the first time anyone finds out the path was nonsense.
  std::string path;
  // Invoke only. Called on the frame loop's thread, from whichever surface
  // drew the button.
  std::function<void()> callback;
};

// Which glyph a button draws. `Label` is short text; everything else names a
// picture. M4 decided against merging an icon font: this app already has an
// icon idiom — 13x13 ASCII grids drawn at an integer scale through ImDrawList
// (94ee2a7, the transport row) — and the sidebar uses the same one, so the
// widget and the strip look like one thing. The grids themselves live in
// `src/avatar/pixel_icons.*`; this enum is only the name, because the registry
// is in aii_core and knows nothing about drawing.
//
// Registered buttons are always `Label`: a script cannot invent a glyph, and
// letting it pick from the built-in ones would only produce two cogs.
// M5.1 adds `Prompts`, the inspector's page-of-text icon. A glyph is a name
// here and a grid in src/avatar/pixel_icons.cpp; nothing in this header draws.
enum class ButtonGlyph { Label, Cog, Folder, Prompts };

struct ToolbarButton {
  std::string id;
  ButtonGlyph glyph = ButtonGlyph::Label;
  std::string label;    // drawn when glyph == Label
  std::string tooltip;
  ButtonAction action;
  ButtonSurface surface = ButtonSurface::Toolbar;
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
// The sidebar's own cap, and it is a physical one: the strip is as tall as its
// buttons and it is docked to the top of the panel, which at its shortest is
// ~168 px. Eight 40 px buttons plus their gaps is 356 px, taller than the
// widget ever is when the chat is shut — so this is the number at which a
// strip stops looking like part of the widget, not a security bound. M5, M6,
// the workers panel and the terminal are four of the eight.
constexpr std::size_t kSidebarButtonsMax = 8;

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

  // M4.4. Registers a button from *inside this process*: a panel, a window or
  // the frame loop, never a script and never a reply. That is why it may carry
  // a callback and its own glyph when add_path_button may not — the input is
  // code, so there is nothing to validate but the shape.
  //
  // This is the call M5's inspector and M6's editor make to put themselves on
  // the sidebar. Neither the strip nor the panel is edited when they do: the
  // surface picks the button up from the registry on the next frame.
  //
  // A duplicate id replaces in place, exactly as add_path_button does, so a
  // window re-registering after a reopen does not grow a second button.
  bool add_app_button(const std::string& id, ButtonGlyph glyph, const std::string& tooltip,
                      ButtonSurface surface, ButtonAction action, std::string* error);

  // A copy, because the panel iterates it for a whole frame while the turn
  // thread may be registering into it. The list is tiny by construction.
  std::vector<ToolbarButton> snapshot() const;

  // The buttons one surface should draw this frame, in registration order.
  //
  // It is a method rather than a filter at each call site because of the
  // fallback rule, which has to live in exactly one place: **when there is no
  // sidebar, its buttons fall back into the toolbar.** The strip is a second
  // OS window, and there are three real ways not to have one — `--opaque`,
  // `--vulkan`, and a `createTarget` that failed — and a build where the
  // settings cog simply does not exist is a worse outcome than a cog in the
  // row it used to be in. Nothing is ever drawn on both surfaces at once.
  std::vector<ToolbarButton> snapshot_for(ButtonSurface surface) const;

  // Told once, by whoever tried to create the strip, before the first frame.
  void set_sidebar_available(bool available);

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
  bool sidebar_available_ = false;
};

// Opens a directory in Explorer. Separate from the registry because the
// registry is a store and this is a side effect on the user's desktop, and
// because the panel dispatches on the action kind — `OpenSettings` has no
// business coming through here.
bool open_directory(const std::string& path, std::string* error);

}  // namespace aii
