#pragma once
// M28: script windows. Dear ImGui, from Python, without Python ever touching
// the frame loop.
//
// ## The problem this solves
//
// A script runs on its own thread and the frame loop never enters the
// interpreter (script_host.h). Dear ImGui is immediate-mode: a window exists
// only while something calls `ImGui::Begin()` ... `ImGui::End()` every frame.
// Those two facts cannot both hold if Python calls ImGui directly — either the
// frame loop takes the GIL every frame and a slow script stalls rendering, or
// the script draws on its own thread into a context the render thread is also
// using. Neither is acceptable here.
//
// So the script does not draw. It **records a frame**: a flat list of widget
// commands, in the order it would have called ImGui, and hands the list over.
// The frame loop **replays** the most recent list through real ImGui on every
// render frame, whether or not the script has submitted a new one since. What
// the user did to the widgets — clicked, typed, dragged — is written back as
// **results**, keyed by the widget's id, and the script reads them the next
// time it records. The script's API therefore still looks like ImGui (`text`,
// `button`, `slider_float`, `begin_child`), and `button()` still returns
// whether it was clicked; it is just answering for the frames the app drew
// since the script last asked.
//
// The script can record at any rate it likes — twice a second for a status
// panel, twenty times for a live meter — and the window is smooth regardless,
// because the app replays at its own frame rate.
//
// ## Where this lives, and why it is a value type
//
// `aii_pyhost.dll` compiles `app_bus.cpp` rather than linking `aii_core`, and
// is handed the app's `AppBus*` over a C ABI, because two copies of a
// singleton talk past each other (aii_pyhost.h). This header follows that
// rule to the letter: `ui_bridge.cpp` is compiled into both halves, and the
// one instance is created by the frame loop and its pointer handed to the DLL
// through `aiiPyHostSetUiBridge`. Keep this header free of Python, ImGui and
// Windows; both halves include it and neither wants the other's dependencies.
//
// ## Threads
//
// Every method takes the one mutex and copies in or out. Nothing here blocks
// on anything but that mutex, and nothing is held across a call into ImGui or
// Python. The script side (`open`, `submit`, `close`, `take_results`) may be
// called from any thread; the app side (`snapshot`, `frame_for`, `set_results`,
// `mark_closed`, `remove`) is the frame loop's, by the same rule as
// `AppBus::apply_pending()`.
//
// ## Bounds
//
// A frame is capped at `kUiCommandsMax` commands and a string at
// `kUiTextMax` bytes; `submit()` truncates and reports rather than refusing,
// because a status panel that lost its last row is a better outcome than a
// status panel that vanished. Windows are capped at `kUiWindowsMax`: each is
// a real OS window with its own ImGui context and font atlas (worker_window.h
// says why that is expensive), and a seventh `open()` is refused with a reason.
//
// ## Results latch
//
// ImGui reports a click on exactly one render frame. The script may not record
// again for another 400 ms, so a result that was only true for one frame would
// be missed almost every time. `clicked` and `changed` therefore **latch**: set
// by the app when it happens, cleared only when the script takes them.
// Values (`f`, `i`, `b`, `s`) are the latest and never clear.
//
// ## The override, and why a slider does not snap back
//
// The command the app replays carries the *script's* value for a slider. If
// the user drags it, ImGui writes the new value into a local copy, the app
// records it as a result, and on the next render frame it would replay the
// script's stale value again — the knob would snap back under the pointer
// until the script re-recorded. So the app keeps a per-window map of
// **overrides**: widget id → the value the user last gave it, used instead of
// the command's value while it exists. An override lives until the script
// submits a frame with a *later generation*, by which point a well-behaved
// script has read the result and re-recorded with the new value. That is the
// `ScriptWindow`'s business, not this header's, but the rule is stated here
// because it is why `UiResult` carries values as well as flags.
//
// ## Ids
//
// A widget's id is what ImGui's would be: its label, under the ids pushed
// around it. Both sides compute the same string — `"a/b/label"` for a `label`
// inside `push_id("a")`, `push_id("b")` — the Python side to know which result
// to return, the app side to know which result to write. The app also pushes
// the same ids onto ImGui's own stack so two buttons with the same label under
// different ids are two buttons to ImGui too. A label carries an ImGui
// `##suffix` or `###suffix` exactly as ImGui would read it; this code does not
// interpret it beyond passing it through.
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace aii {

constexpr std::size_t kUiWindowsMax = 6;      // OS windows open at once
constexpr std::size_t kUiCommandsMax = 2048;  // commands in one frame
constexpr std::size_t kUiTextMax = 4096;      // bytes of one string
constexpr std::size_t kUiItemsMax = 256;      // entries of one list
constexpr std::size_t kUiValuesMax = 1024;    // floats in a plot
constexpr std::size_t kUiKeyMax = 64;         // bytes of a window key

// One replayed call. Which fields matter depends on `op`; the table is in
// `docs/design-script-ui.md` and mirrored in `aii_pyhost.cpp` and
// `script_window.cpp`, which are the only two places that know an op's shape.
enum class UiOp : std::uint8_t {
  // layout
  Text, TextColored, TextWrapped, TextDisabled, BulletText, LabelText,
  Separator, SeparatorText, SameLine, NewLine, Spacing, Dummy, Indent, Unindent,
  // interactive; each writes a result under its id
  Button, SmallButton, Checkbox, RadioButton, Selectable,
  SliderFloat, SliderInt, DragFloat, DragInt,
  InputText, InputTextMultiline, InputInt, InputFloat,
  Combo, ListBox, ColorEdit,
  // display
  ProgressBar, PlotLines, PlotHistogram,
  // containers; each Begin* has an End*, and a frame that forgets one is
  // closed for it by the app so ImGui never sees an unbalanced stack
  CollapsingHeader,  // result `b` = open; the app draws the children only when open
  TreeNode, TreePop,
  BeginChild, EndChild,
  BeginGroup, EndGroup,
  BeginDisabled, EndDisabled,
  BeginTabBar, EndTabBar, BeginTabItem, EndTabItem,
  BeginTable, EndTable, TableNextRow, TableNextColumn, TableSetupColumn, TableHeadersRow,
  Columns, NextColumn,
  // ids and style
  PushId, PopId,
  PushStyleColor, PopStyleColor,
  PushItemWidth, PopItemWidth,
  SetNextItemWidth,
  SetTooltip,  // applies to the item before it, as ImGui's does
  // meta
  SetScrollHereY,
  Count
};

struct UiCommand {
  UiOp op = UiOp::Text;
  std::string label;   // the ImGui label, or the text for text ops
  std::string text;    // second string: overlay, hint, format, LabelText value
  float f[4] = {0, 0, 0, 0};  // value / min / max / step, or rgba for colours
  int i[2] = {0, 0};           // int value / min, max in i[0],i[1] for SliderInt; flags
  bool b = false;              // Checkbox value, Selectable selected, BeginTabItem open
  std::vector<std::string> items;  // Combo / ListBox entries
  std::vector<float> values;       // PlotLines / PlotHistogram
};

struct UiFrame {
  std::vector<UiCommand> cmds;
  // Incremented by the bridge on every submit(); the window uses it to know a
  // new recording arrived and drops its overrides.
  std::uint64_t generation = 0;
};

// What happened to one interactive widget since the script last looked.
struct UiResult {
  std::string id;  // the composed id, see the header comment
  bool clicked = false;  // latched: button pressed, selectable clicked, tab selected
  bool changed = false;  // latched: a value differs from the command's
  bool b = false;        // checkbox, collapsing header open, tab item selected
  float f[4] = {0, 0, 0, 0};
  int i = 0;
  std::string s;         // input text
};

// The OS window a set of frames is drawn into.
struct UiWindowSpec {
  std::string key;    // the script's handle, [A-Za-z0-9_.-], <= kUiKeyMax
  std::string title;  // the OS window's title bar
  unsigned w = 360;   // client size, pixels, as the script asked
  unsigned h = 240;
};

class UiBridge {
 public:
  // ---- script side ----------------------------------------------------
  // Opens, or re-titles and re-sizes, a window. False with a one-line reason
  // when the key is malformed or the window cap is reached. Reopening a key
  // the user closed clears the closed flag: the window comes back.
  bool open(const UiWindowSpec& spec, std::string* error = nullptr);
  // Replaces the window's frame. Bounds are applied here: commands past
  // `kUiCommandsMax` are dropped, strings and lists truncated, and `error`
  // (if given) says so in one line. False only when the key is unknown.
  bool submit(const std::string& key, UiFrame frame, std::string* error = nullptr);
  // Asks the app to destroy the window. The key is forgotten; a later open()
  // makes a fresh one.
  void close(const std::string& key);
  // True while the window exists and the user has not closed it. A script's
  // loop reads this to know when to stop recording.
  bool is_open(const std::string& key) const;
  // Every result for this window, and clears the latched flags. Values stay.
  std::vector<UiResult> take_results(const std::string& key);
  std::vector<std::string> keys() const;

  // ---- app side (frame loop) ------------------------------------------
  // Every window the app should have right now: specs for open ones, and the
  // keys the script closed (in `closed`), which the caller destroys and then
  // remove()s.
  void snapshot(std::vector<UiWindowSpec>* open, std::vector<std::string>* closing) const;
  // A copy of the latest frame. Cheap enough at the cap; the window may skip
  // the copy when `generation` matches what it last replayed.
  bool frame_for(const std::string& key, UiFrame* out) const;
  std::uint64_t generation_of(const std::string& key) const;
  // Merges this render frame's results into the window's: flags OR in (latch),
  // values replace.
  void set_results(const std::string& key, const std::vector<UiResult>& results);
  // The user closed the OS window. `is_open()` goes false; the entry stays so
  // the script can see that and decide. The window itself is destroyed by the
  // caller.
  void mark_closed(const std::string& key);
  // Forgets an entry the script close()d, after its window is gone.
  void remove(const std::string& key);

  // The one instance, created by the frame loop and handed to the DLL. Not a
  // singleton accessor on purpose — see the header comment.
  UiBridge() = default;
  UiBridge(const UiBridge&) = delete;
  UiBridge& operator=(const UiBridge&) = delete;

 private:
  struct Entry {
    UiWindowSpec spec;
    UiFrame frame;
    std::map<std::string, UiResult> results;
    bool user_closed = false;    // mark_closed()
    bool script_closed = false;  // close(), awaiting remove()
  };
  mutable std::mutex mutex_;
  std::map<std::string, Entry> entries_;
  std::uint64_t next_generation_ = 1;
};

// Shared by both sides so the two id computations cannot drift: joins the
// pushed ids and the label with '/'.
std::string ui_compose_id(const std::vector<std::string>& stack, const std::string& label);
// The key rule, in one place: non-empty, <= kUiKeyMax, [A-Za-z0-9_.-].
bool ui_valid_key(const std::string& key);

}  // namespace aii
