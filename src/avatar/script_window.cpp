#include "script_window.h"

#include "rend/core/log.h"
#include "rend/gpu/command_context.h"
#include "rend/gpu/device.h"
#include "rend/gpu/frame_renderer.h"
#include "rend/gpu/instance.h"
#include "rend/gpu/swapchain.h"
#include "rend/platform/backend.h"

#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

#include "imgui.h"
#include "imnodes.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "imgui_layer.h"
#include "tool_window_core.h"
#include "win_text_input.h"

using namespace rend;

namespace aii {
namespace {

// The window's floor, and the same reachability rule WorkerWindow uses: a
// remembered position is honoured only while enough of it lands on a work
// area to see and drag back; otherwise it is centred rather than nudged.
constexpr int kMinW = 200;
constexpr int kMinH = 120;
constexpr int kMinVisibleW = 160;
constexpr int kMinVisibleH = 32;

struct WorkAreas {
  std::vector<RECT> rects;
};

BOOL CALLBACK collect_work_area(HMONITOR mon, HDC, LPRECT, LPARAM user) {
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(mon, &mi)) reinterpret_cast<WorkAreas*>(user)->rects.push_back(mi.rcWork);
  return TRUE;
}

RECT work_area_for(const RECT& r) {
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
      mon && GetMonitorInfoW(mon, &mi))
    return mi.rcWork;
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  return work;
}

bool reachable(const RECT& window) {
  WorkAreas areas;
  EnumDisplayMonitors(nullptr, nullptr, collect_work_area, reinterpret_cast<LPARAM>(&areas));
  if (areas.rects.empty()) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    areas.rects.push_back(work);
  }
  for (const RECT& work : areas.rects) {
    RECT hit{};
    if (!IntersectRect(&hit, &window, &work)) continue;
    if (hit.right - hit.left >= kMinVisibleW && hit.bottom - hit.top >= kMinVisibleH) return true;
  }
  return false;
}

void fit_to_desktop(ScriptWindowGeometry& g) {
  RECT want{g.x, g.y, g.x + static_cast<int>(g.w), g.y + static_cast<int>(g.h)};
  const RECT work = work_area_for(want);
  const int work_w = std::max(kMinW, static_cast<int>(work.right - work.left));
  const int work_h = std::max(kMinH, static_cast<int>(work.bottom - work.top));
  g.w = static_cast<unsigned>(std::clamp(static_cast<int>(g.w), kMinW, work_w));
  g.h = static_cast<unsigned>(std::clamp(static_cast<int>(g.h), kMinH, work_h));

  const auto centre = [&] {
    g.x = static_cast<int>(work.left) +
          (static_cast<int>(work.right - work.left) - static_cast<int>(g.w)) / 2;
    g.y = static_cast<int>(work.top) +
          (static_cast<int>(work.bottom - work.top) - static_cast<int>(g.h)) / 2;
    g.placed = true;
  };
  if (!g.placed) {
    centre();
    return;
  }
  want = {g.x, g.y, g.x + static_cast<int>(g.w), g.y + static_cast<int>(g.h)};
  if (reachable(want)) return;
  log::info("script window: {},{} is off-screen; centring", g.x, g.y);
  centre();
}

std::wstring wide_from_utf8(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(std::max(n, 0)), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// This window's own input, queued rather than fed to ImGui as it arrives —
// the same rule WorkerWindow follows and for the same reason: the messages
// are pumped inside the widget's own pumpEvents() call, where another ImGui
// context is current.
struct ScriptInput {
  std::vector<std::pair<int, bool>> buttons;  // (ImGui button index, pressed)
  float wheel = 0.0f;
  bool close_requested = false;
};

LRESULT CALLBACK scriptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
  auto* in = reinterpret_cast<ScriptInput*>(ref);
  if (in) {
    switch (msg) {
      case WM_LBUTTONDOWN: in->buttons.emplace_back(0, true); break;
      case WM_LBUTTONUP: in->buttons.emplace_back(0, false); break;
      case WM_RBUTTONDOWN: in->buttons.emplace_back(1, true); break;
      case WM_RBUTTONUP: in->buttons.emplace_back(1, false); break;
      // M30.1. The middle button was never forwarded, and it is imnodes'
      // default pan button -- the first node graph a user opened could not be
      // moved at all. Right-drag pans too (see the IO setup in create()).
      case WM_MBUTTONDOWN: in->buttons.emplace_back(2, true); break;
      case WM_MBUTTONUP: in->buttons.emplace_back(2, false); break;
      case WM_MOUSEWHEEL: in->wheel += GET_WHEEL_DELTA_WPARAM(wp) / 120.0f; break;
      case WM_CLOSE:
        // Swallowed and latched, WorkerWindow's reason exactly: the SDL
        // backend's close event is identityless and the frame loop quits the
        // whole application on it, so letting this reach SDL would make a
        // script window's close box take the widget down with it.
        in->close_requested = true;
        return 0;
      default: break;
    }
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}

// sRGB <-> linear, the inverse pair `imgui_layer.cpp`'s `to_linear` needs but
// does not export (ColorEdit is the one op that has to hand a script back
// what it gave, and `ui_color()` only goes one way).
float to_linear(float c) {
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float to_srgb(float c) {
  c = std::clamp(c, 0.0f, 1.0f);
  return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

}  // namespace

// M24.3's shell, exactly as WorkerWindow uses it. What is new here is the
// replay state: the id stack and overrides `docs/design-script-ui.md` §3 and
// `ui_bridge.h`'s "The override" section describe, plus the depth trackers
// that keep every Begin/End pair balanced for ImGui even when the script's
// own recording is not.
struct ScriptWindow::Impl : ToolWindowCore {
  std::unique_ptr<ScriptInput> input;
  std::string key;
  ScriptWindowGeometry remembered;
  std::string title;

  // The last frame copied from the bridge, and the generation it was copied
  // at — copying is skipped when the bridge's generation has not moved on,
  // per `UiBridge::frame_for`'s comment.
  UiFrame frame;
  std::uint64_t generation = 0;
  // The bridge's epoch for this key as of the last frame. Moving on means a
  // new run of a script took the window over (ui_bridge.h, "Takeover"), and
  // its node placements and values should land as if the window were new.
  std::uint64_t epoch = 0;
  // Widget id -> the user's latest edit, kept until the script submits a
  // later generation. `ui_bridge.h`'s "why a slider does not snap back".
  std::map<std::string, UiResult> overrides;

  // M30: this window's own imnodes context -- imnodes' state is one global
  // (`GImNodes`), exactly like ImGui's own ambient context, so every window
  // that draws a node editor needs its own and must make it current before
  // drawing, the same rule `ui`/`make_current()` already follows for ImGui.
  // Created right after the ImGui layer exists (create()) and destroyed
  // before it goes (~ScriptWindow()).
  ImNodesContext* node_ctx = nullptr;
  // M30.1. Keys, through the same HWND subclass the widget uses, bound to this
  // window's ImGui context (win_text_input.cpp). Without it a script window
  // has no keyboard: no typing into input_text, no Delete for a selected link.
  std::unique_ptr<WinTextInput> keys;
  // Node ids this window has placed at least once via SetNodePos. A script
  // re-records every ~100ms; a node already positioned must not be re-pinned
  // under the user's drag unless the script forces it (SetNodePos's `force`).
  // Cleared only when the window itself is destroyed, never on a new
  // generation -- ui_bridge.h's "why a slider does not snap back" applies to
  // node positions too, and clearing on generation would re-pin every node on
  // every recording.
  std::set<int> positioned;
};

namespace {

float ov_f(ScriptWindow::Impl& s, const std::string& id, int idx, float fallback) {
  auto it = s.overrides.find(id);
  return it == s.overrides.end() ? fallback : it->second.f[idx];
}
int ov_i(ScriptWindow::Impl& s, const std::string& id, int fallback) {
  auto it = s.overrides.find(id);
  return it == s.overrides.end() ? fallback : it->second.i;
}
bool ov_b(ScriptWindow::Impl& s, const std::string& id, bool fallback) {
  auto it = s.overrides.find(id);
  return it == s.overrides.end() ? fallback : it->second.b;
}
std::string ov_s(ScriptWindow::Impl& s, const std::string& id, const std::string& fallback) {
  auto it = s.overrides.find(id);
  return it == s.overrides.end() ? fallback : it->second.s;
}
void set_ov(ScriptWindow::Impl& s, const UiResult& r) { s.overrides[r.id] = r; }

// Replays one recorded frame through real ImGui, inside the caller's already-
// open full-client window. Everything this function opens it also closes,
// including whatever the script's own recording forgot to — the balancing
// rule in `docs/design-script-ui.md` §3, because ImGui's asserts are compiled
// out in Release and an unbalanced stack is otherwise a silent corruption of
// every window drawn after this one.
void replay(ScriptWindow::Impl& s, std::vector<std::string>& id_stack,
            std::vector<UiResult>& results) {
  // Depth trackers, one per Begin/End pair, exactly as the design's
  // "Balancing" section lists them.
  int child_depth = 0;
  int group_depth = 0;
  int disabled_depth = 0;
  int tabbar_open = 0;
  int tabitem_open = 0;
  int table_open = 0;
  int style_color_depth = 0;
  int item_width_depth = 0;
  bool columns_active = false;
  // M30: node-editor state. `editor_open` caps at 1 (a second BeginNodeEditor
  // while one is open is ignored, per the header comment); `attr_open` is
  // 0/1/2/3 for none/input/output/static since imnodes' three attribute kinds
  // cannot nest inside each other. `nodes_this_frame` is every node id
  // BeginNode drew since the last BeginNodeEditor, which is what
  // EndNodeEditor reports positions and selection for.
  int editor_open = 0;
  int node_open = 0;
  int title_bar_open = 0;
  int attr_open = 0;
  int node_color_depth = 0;
  std::vector<int> nodes_this_frame;

  // Closes whatever of a node's pieces are open, innermost first: an
  // attribute, then the title bar, then the node itself. Used both by the
  // explicit EndNode/EndNodeEditor handling and by the end-of-frame
  // balancing, so a script that forgets an End never leaves imnodes with an
  // unbalanced stack.
  const auto close_attr = [&] {
    switch (attr_open) {
      case 1: ImNodes::EndInputAttribute(); break;
      case 2: ImNodes::EndOutputAttribute(); break;
      case 3: ImNodes::EndStaticAttribute(); break;
      default: break;
    }
    attr_open = 0;
  };
  const auto close_node = [&] {
    close_attr();
    if (title_bar_open) {
      ImNodes::EndNodeTitleBar();
      title_bar_open = 0;
    }
    if (node_open) {
      ImNodes::EndNode();
      node_open = 0;
    }
  };
  // Closes the node editor: whatever node it left open, then the editor
  // itself, then reads the results the design documents -- node_pos and
  // node_selected for every node id BeginNode drew this frame, and the one
  // link_created / link_destroyed pair, if any. Only called while
  // `editor_open`, so EndNodeEditor() is never called on a context that
  // never got a matching Begin.
  const auto close_editor = [&] {
    close_node();
    while (node_color_depth-- > 0) ImNodes::PopColorStyle();
    node_color_depth = 0;
    ImNodes::EndNodeEditor();
    editor_open = 0;
    for (int id : nodes_this_frame) {
      const ImVec2 pos = ImNodes::GetNodeGridSpacePos(id);
      UiResult rp;
      rp.id = "node_pos:" + std::to_string(id);
      rp.f[0] = pos.x;
      rp.f[1] = pos.y;
      results.push_back(rp);
      // The drawn size too (M30.3): a script cannot know how wide a title or
      // a hint line came out, and without it nothing on the Python side can
      // tell that two nodes overlap.
      const ImVec2 dim = ImNodes::GetNodeDimensions(id);
      UiResult rd;
      rd.id = "node_size:" + std::to_string(id);
      rd.f[0] = dim.x;
      rd.f[1] = dim.y;
      results.push_back(rd);
      UiResult rs;
      rs.id = "node_selected:" + std::to_string(id);
      rs.b = ImNodes::IsNodeSelected(id);
      results.push_back(rs);
    }
    nodes_this_frame.clear();
    int link_start = 0, link_end = 0;
    if (ImNodes::IsLinkCreated(&link_start, &link_end)) {
      UiResult r;
      r.id = "link_created:" + std::to_string(link_start) + ":" + std::to_string(link_end);
      r.clicked = true;
      results.push_back(r);
    }
    // Delete with links selected removes them. imnodes only *reports*
    // destruction for a detach-by-drag; deleting a selection is the app's
    // job, and it is the same result to the script either way.
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && ImNodes::NumSelectedLinks() > 0) {
      std::vector<int> selected(static_cast<std::size_t>(ImNodes::NumSelectedLinks()));
      ImNodes::GetSelectedLinks(selected.data());
      for (int id : selected) {
        UiResult r;
        r.id = "link_destroyed:" + std::to_string(id);
        r.clicked = true;
        results.push_back(r);
      }
      ImNodes::ClearLinkSelection();
    }
    int destroyed_id = 0;
    if (ImNodes::IsLinkDestroyed(&destroyed_id)) {
      UiResult r;
      r.id = "link_destroyed:" + std::to_string(destroyed_id);
      r.clicked = true;
      results.push_back(r);
    }
  };
  // TreeNode's per-node open state: TreePop must consume the matching entry
  // without calling ImGui::TreePop() when ImGui returned false for the node.
  std::vector<bool> tree_stack;

  // A closed tab item or tree node skips its children up to the matching
  // end. The script only records children when the widget last answered
  // "open", but that answer is one recording old, so on the frame the user
  // switches tabs the stale children would otherwise be drawn below the tab
  // bar as loose widgets.
  const auto skip_to = [&](std::size_t& i, UiOp begin_op, UiOp end_op) {
    int depth = 1;
    for (++i; i < s.frame.cmds.size(); ++i) {
      if (s.frame.cmds[i].op == begin_op) ++depth;
      if (s.frame.cmds[i].op == end_op && --depth == 0) return;  // consumed the end
    }
    --i;  // ran off the end; the loop's ++ finishes it
  };
  for (std::size_t ci = 0; ci < s.frame.cmds.size(); ++ci) {
    const UiCommand& cmd = s.frame.cmds[ci];
    switch (cmd.op) {
      // ---- text and layout ------------------------------------------------
      case UiOp::Text: ImGui::TextUnformatted(cmd.label.c_str()); break;
      case UiOp::TextWrapped: ImGui::TextWrapped("%s", cmd.label.c_str()); break;
      case UiOp::TextDisabled: ImGui::TextDisabled("%s", cmd.label.c_str()); break;
      case UiOp::BulletText: ImGui::BulletText("%s", cmd.label.c_str()); break;
      case UiOp::TextColored:
        ImGui::TextColored(ui_color(cmd.f[0], cmd.f[1], cmd.f[2], cmd.f[3]), "%s",
                            cmd.label.c_str());
        break;
      case UiOp::LabelText: ImGui::LabelText(cmd.label.c_str(), "%s", cmd.text.c_str()); break;
      case UiOp::Separator: ImGui::Separator(); break;
      case UiOp::SeparatorText: ImGui::SeparatorText(cmd.label.c_str()); break;
      case UiOp::SameLine: ImGui::SameLine(cmd.f[0], cmd.f[1] == 0.0f ? -1.0f : cmd.f[1]); break;
      case UiOp::NewLine: ImGui::NewLine(); break;
      case UiOp::Spacing: ImGui::Spacing(); break;
      case UiOp::Dummy: ImGui::Dummy(ImVec2(cmd.f[0], cmd.f[1])); break;
      case UiOp::Indent: ImGui::Indent(cmd.f[0]); break;
      case UiOp::Unindent: ImGui::Unindent(cmd.f[0]); break;

      // ---- interactive: each writes a result under its composed id --------
      case UiOp::Button: {
        UiResult r;
        r.id = ui_compose_id(id_stack, cmd.label);
        r.clicked = ImGui::Button(cmd.label.c_str(), ImVec2(cmd.f[0], cmd.f[1]));
        results.push_back(r);
        break;
      }
      case UiOp::SmallButton: {
        UiResult r;
        r.id = ui_compose_id(id_stack, cmd.label);
        r.clicked = ImGui::SmallButton(cmd.label.c_str());
        results.push_back(r);
        break;
      }
      case UiOp::Checkbox: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        bool value = ov_b(s, id, cmd.b);
        UiResult r;
        r.id = id;
        r.changed = ImGui::Checkbox(cmd.label.c_str(), &value);
        r.b = value;
        if (r.changed) set_ov(s, r);
        results.push_back(r);
        break;
      }
      case UiOp::RadioButton: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        bool value = ov_b(s, id, cmd.b);
        const bool clicked = ImGui::RadioButton(cmd.label.c_str(), value);
        UiResult r;
        r.id = id;
        r.changed = clicked;
        r.b = clicked ? true : value;
        if (r.changed) set_ov(s, r);
        results.push_back(r);
        break;
      }
      case UiOp::Selectable: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        bool value = ov_b(s, id, cmd.b);
        const bool clicked = ImGui::Selectable(cmd.label.c_str(), value);
        UiResult r;
        r.id = id;
        r.clicked = clicked;
        r.b = clicked ? true : value;
        if (clicked) set_ov(s, r);
        results.push_back(r);
        break;
      }
      case UiOp::SliderFloat: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        float value = ov_f(s, id, 0, cmd.f[0]);
        const char* fmt = cmd.text.empty() ? "%.3f" : cmd.text.c_str();
        UiResult r;
        r.id = id;
        r.changed = ImGui::SliderFloat(cmd.label.c_str(), &value, cmd.f[1], cmd.f[2], fmt);
        r.f[0] = value;
        if (r.changed) set_ov(s, r);
        results.push_back(r);
        break;
      }
      case UiOp::DragFloat: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        float value = ov_f(s, id, 0, cmd.f[0]);
        const char* fmt = cmd.text.empty() ? "%.3f" : cmd.text.c_str();
        const float speed = cmd.f[3] == 0.0f ? 1.0f : cmd.f[3];
        UiResult r;
        r.id = id;
        r.changed = ImGui::DragFloat(cmd.label.c_str(), &value, speed, cmd.f[1], cmd.f[2], fmt);
        r.f[0] = value;
        if (r.changed) set_ov(s, r);
        results.push_back(r);
        break;
      }
      case UiOp::SliderInt: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        int value = ov_i(s, id, cmd.i[0]);
        const char* fmt = cmd.text.empty() ? "%d" : cmd.text.c_str();
        UiResult r;
        r.id = id;
        r.changed =
            ImGui::SliderInt(cmd.label.c_str(), &value, cmd.i[1], static_cast<int>(cmd.f[2]), fmt);
        r.i = value;
        if (r.changed) set_ov(s, r);
        results.push_back(r);
        break;
      }
      case UiOp::DragInt: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        int value = ov_i(s, id, cmd.i[0]);
        const char* fmt = cmd.text.empty() ? "%d" : cmd.text.c_str();
        const float speed = cmd.f[3] == 0.0f ? 1.0f : cmd.f[3];
        UiResult r;
        r.id = id;
        r.changed = ImGui::DragInt(cmd.label.c_str(), &value, speed, cmd.i[1],
                                    static_cast<int>(cmd.f[2]), fmt);
        r.i = value;
        if (r.changed) set_ov(s, r);
        results.push_back(r);
        break;
      }
      case UiOp::InputText:
      case UiOp::InputTextMultiline: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        const std::string initial = cmd.items.empty() ? std::string() : cmd.items[0];
        std::string value = ov_s(s, id, initial);
        char buf[kUiTextMax];
        const std::size_t n = std::min(value.size(), kUiTextMax - 1);
        std::memcpy(buf, value.data(), n);
        buf[n] = '\0';
        bool changed = false;
        if (cmd.op == UiOp::InputText) {
          const auto flags = static_cast<ImGuiInputTextFlags>(cmd.i[1]);
          changed = cmd.text.empty()
                        ? ImGui::InputText(cmd.label.c_str(), buf, sizeof(buf), flags)
                        : ImGui::InputTextWithHint(cmd.label.c_str(), cmd.text.c_str(), buf,
                                                    sizeof(buf), flags);
        } else {
          changed = ImGui::InputTextMultiline(cmd.label.c_str(), buf, sizeof(buf),
                                              ImVec2(cmd.f[0], cmd.f[1]));
        }
        UiResult r;
        r.id = id;
        r.changed = changed;
        r.s = changed ? std::string(buf) : value;
        if (changed) set_ov(s, r);
        results.push_back(r);
        break;
      }
      case UiOp::InputInt: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        int value = ov_i(s, id, cmd.i[0]);
        const int step = cmd.i[1] == 0 ? 1 : cmd.i[1];
        UiResult r;
        r.id = id;
        r.changed = ImGui::InputInt(cmd.label.c_str(), &value, step);
        r.i = value;
        if (r.changed) set_ov(s, r);
        results.push_back(r);
        break;
      }
      case UiOp::InputFloat: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        float value = ov_f(s, id, 0, cmd.f[0]);
        const char* fmt = cmd.text.empty() ? "%.3f" : cmd.text.c_str();
        UiResult r;
        r.id = id;
        r.changed = ImGui::InputFloat(cmd.label.c_str(), &value, cmd.f[1], 0.0f, fmt);
        r.f[0] = value;
        if (r.changed) set_ov(s, r);
        results.push_back(r);
        break;
      }
      case UiOp::Combo: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        int index = ov_i(s, id, cmd.i[0]);
        std::vector<const char*> items;
        items.reserve(cmd.items.size());
        for (const std::string& it : cmd.items) items.push_back(it.c_str());
        UiResult r;
        r.id = id;
        r.changed = ImGui::Combo(cmd.label.c_str(), &index, items.data(),
                                  static_cast<int>(items.size()));
        r.i = index;
        if (r.changed) set_ov(s, r);
        results.push_back(r);
        break;
      }
      case UiOp::ListBox: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        int index = ov_i(s, id, cmd.i[0]);
        std::vector<const char*> items;
        items.reserve(cmd.items.size());
        for (const std::string& it : cmd.items) items.push_back(it.c_str());
        const int height = cmd.i[1] == 0 ? -1 : cmd.i[1];
        UiResult r;
        r.id = id;
        r.changed = ImGui::ListBox(cmd.label.c_str(), &index, items.data(),
                                    static_cast<int>(items.size()), height);
        r.i = index;
        if (r.changed) set_ov(s, r);
        results.push_back(r);
        break;
      }
      case UiOp::ColorEdit: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        // The value crosses in and out in sRGB, as any colour picker gives
        // it; ImGui itself only ever sees the linear form, the same rule
        // every other colour in this replayer follows.
        float srgb[4] = {ov_f(s, id, 0, cmd.f[0]), ov_f(s, id, 1, cmd.f[1]),
                          ov_f(s, id, 2, cmd.f[2]), ov_f(s, id, 3, cmd.f[3])};
        float col[4] = {to_linear(srgb[0]), to_linear(srgb[1]), to_linear(srgb[2]), srgb[3]};
        const auto flags = static_cast<ImGuiColorEditFlags>(cmd.i[1]);
        UiResult r;
        r.id = id;
        r.changed = ImGui::ColorEdit4(cmd.label.c_str(), col, flags);
        r.f[0] = to_srgb(col[0]);
        r.f[1] = to_srgb(col[1]);
        r.f[2] = to_srgb(col[2]);
        r.f[3] = col[3];
        if (r.changed) set_ov(s, r);
        results.push_back(r);
        break;
      }

      // ---- display, no result -----------------------------------------
      case UiOp::ProgressBar:
        ImGui::ProgressBar(cmd.f[0], ImVec2(cmd.f[1], cmd.f[2]),
                           cmd.text.empty() ? nullptr : cmd.text.c_str());
        break;
      case UiOp::PlotLines:
        ImGui::PlotLines(cmd.label.c_str(), cmd.values.data(), static_cast<int>(cmd.values.size()),
                         0, cmd.text.empty() ? nullptr : cmd.text.c_str(), cmd.f[0], cmd.f[1],
                         ImVec2(cmd.f[2], cmd.f[3]));
        break;
      case UiOp::PlotHistogram:
        ImGui::PlotHistogram(cmd.label.c_str(), cmd.values.data(),
                             static_cast<int>(cmd.values.size()), 0,
                             cmd.text.empty() ? nullptr : cmd.text.c_str(), cmd.f[0], cmd.f[1],
                             ImVec2(cmd.f[2], cmd.f[3]));
        break;

      // ---- containers: balanced by the depth trackers above ------------
      case UiOp::CollapsingHeader: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        auto flags = static_cast<ImGuiTreeNodeFlags>(cmd.i[1]);
        if (cmd.b) flags |= ImGuiTreeNodeFlags_DefaultOpen;
        const bool open = ImGui::CollapsingHeader(cmd.label.c_str(), flags);
        UiResult r;
        r.id = id;
        r.clicked = ImGui::IsItemToggledOpen();
        r.b = open;
        results.push_back(r);
        break;
      }
      case UiOp::TreeNode: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        const bool open = ImGui::TreeNode(cmd.label.c_str());
        UiResult r;
        r.id = id;
        r.clicked = ImGui::IsItemToggledOpen();
        r.b = open;
        results.push_back(r);
        if (open)
          tree_stack.push_back(true);
        else
          skip_to(ci, UiOp::TreeNode, UiOp::TreePop);
        break;
      }
      case UiOp::TreePop:
        if (!tree_stack.empty()) {
          const bool was_open = tree_stack.back();
          tree_stack.pop_back();
          if (was_open) ImGui::TreePop();
        }
        break;
      case UiOp::BeginChild: {
        auto flags = static_cast<ImGuiChildFlags>(cmd.i[1]);
        if (cmd.b) flags |= ImGuiChildFlags_Borders;
        ImGui::BeginChild(cmd.label.c_str(), ImVec2(cmd.f[0], cmd.f[1]), flags);
        ++child_depth;
        break;
      }
      case UiOp::EndChild:
        if (child_depth > 0) {
          ImGui::EndChild();
          --child_depth;
        }
        break;
      case UiOp::BeginGroup:
        ImGui::BeginGroup();
        ++group_depth;
        break;
      case UiOp::EndGroup:
        if (group_depth > 0) {
          ImGui::EndGroup();
          --group_depth;
        }
        break;
      case UiOp::BeginDisabled:
        ImGui::BeginDisabled(cmd.b);
        ++disabled_depth;
        break;
      case UiOp::EndDisabled:
        if (disabled_depth > 0) {
          ImGui::EndDisabled();
          --disabled_depth;
        }
        break;
      case UiOp::BeginTabBar:
        if (ImGui::BeginTabBar(cmd.label.c_str())) ++tabbar_open;
        break;
      case UiOp::EndTabBar:
        if (tabbar_open > 0) {
          ImGui::EndTabBar();
          --tabbar_open;
        }
        break;
      case UiOp::BeginTabItem: {
        const std::string id = ui_compose_id(id_stack, cmd.label);
        const bool open = ImGui::BeginTabItem(cmd.label.c_str());
        UiResult r;
        r.id = id;
        r.clicked = ImGui::IsItemActivated();
        r.b = open;
        results.push_back(r);
        if (open)
          ++tabitem_open;
        else
          skip_to(ci, UiOp::BeginTabItem, UiOp::EndTabItem);
        break;
      }
      case UiOp::EndTabItem:
        if (tabitem_open > 0) {
          ImGui::EndTabItem();
          --tabitem_open;
        }
        break;
      case UiOp::BeginTable:
        if (ImGui::BeginTable(cmd.label.c_str(), std::max(1, cmd.i[0]),
                              static_cast<ImGuiTableFlags>(cmd.i[1]), ImVec2(cmd.f[0], cmd.f[1])))
          ++table_open;
        break;
      case UiOp::EndTable:
        if (table_open > 0) {
          ImGui::EndTable();
          --table_open;
        }
        break;
      case UiOp::TableNextRow: ImGui::TableNextRow(); break;
      case UiOp::TableNextColumn: ImGui::TableNextColumn(); break;
      case UiOp::TableSetupColumn:
        ImGui::TableSetupColumn(cmd.label.c_str(), static_cast<ImGuiTableColumnFlags>(cmd.i[1]),
                                cmd.f[0]);
        break;
      case UiOp::TableHeadersRow: ImGui::TableHeadersRow(); break;
      case UiOp::Columns: {
        const int count = cmd.i[0] <= 0 ? 1 : cmd.i[0];
        ImGui::Columns(count, nullptr, cmd.b);
        columns_active = count > 1;
        break;
      }
      case UiOp::NextColumn: ImGui::NextColumn(); break;

      // ---- ids and style --------------------------------------------------
      case UiOp::PushId:
        id_stack.push_back(cmd.label);
        ImGui::PushID(cmd.label.c_str());
        break;
      case UiOp::PopId:
        if (!id_stack.empty()) {
          id_stack.pop_back();
          ImGui::PopID();
        }
        break;
      case UiOp::PushStyleColor:
        ImGui::PushStyleColor(static_cast<ImGuiCol>(cmd.i[0]),
                              ui_color(cmd.f[0], cmd.f[1], cmd.f[2], cmd.f[3]));
        ++style_color_depth;
        break;
      case UiOp::PopStyleColor: {
        const int count = std::min(cmd.i[0] <= 0 ? 1 : cmd.i[0], style_color_depth);
        if (count > 0) {
          ImGui::PopStyleColor(count);
          style_color_depth -= count;
        }
        break;
      }
      case UiOp::PushItemWidth:
        ImGui::PushItemWidth(cmd.f[0]);
        ++item_width_depth;
        break;
      case UiOp::PopItemWidth:
        if (item_width_depth > 0) {
          ImGui::PopItemWidth();
          --item_width_depth;
        }
        break;
      case UiOp::SetNextItemWidth: ImGui::SetNextItemWidth(cmd.f[0]); break;
      case UiOp::SetTooltip: ImGui::SetTooltip("%s", cmd.label.c_str()); break;

      // ---- meta -------------------------------------------------------
      case UiOp::SetScrollHereY: ImGui::SetScrollHereY(cmd.f[0]); break;

      // ---- node graphs (M30, imnodes) ----------------------------------
      case UiOp::BeginNodeEditor:
        if (editor_open == 0) {
          ImNodes::BeginNodeEditor();
          editor_open = 1;
          nodes_this_frame.clear();
        }
        break;
      case UiOp::EndNodeEditor:
        if (editor_open) close_editor();
        break;
      case UiOp::BeginNode:
        if (editor_open && !node_open) {
          ImNodes::BeginNode(cmd.i[0]);
          node_open = 1;
          nodes_this_frame.push_back(cmd.i[0]);
        }
        break;
      case UiOp::EndNode:
        if (node_open) close_node();
        break;
      case UiOp::BeginNodeTitleBar:
        if (node_open && !title_bar_open) {
          ImNodes::BeginNodeTitleBar();
          title_bar_open = 1;
        }
        break;
      case UiOp::EndNodeTitleBar:
        if (title_bar_open) {
          ImNodes::EndNodeTitleBar();
          title_bar_open = 0;
        }
        break;
      case UiOp::BeginInputAttribute:
        if (node_open && attr_open == 0) {
          ImNodes::BeginInputAttribute(cmd.i[0], static_cast<ImNodesPinShape>(cmd.i[1]));
          attr_open = 1;
        }
        break;
      case UiOp::EndInputAttribute:
        if (attr_open == 1) {
          ImNodes::EndInputAttribute();
          attr_open = 0;
        }
        break;
      case UiOp::BeginOutputAttribute:
        if (node_open && attr_open == 0) {
          ImNodes::BeginOutputAttribute(cmd.i[0], static_cast<ImNodesPinShape>(cmd.i[1]));
          attr_open = 2;
        }
        break;
      case UiOp::EndOutputAttribute:
        if (attr_open == 2) {
          ImNodes::EndOutputAttribute();
          attr_open = 0;
        }
        break;
      case UiOp::BeginStaticAttribute:
        if (node_open && attr_open == 0) {
          ImNodes::BeginStaticAttribute(cmd.i[0]);
          attr_open = 3;
        }
        break;
      case UiOp::EndStaticAttribute:
        if (attr_open == 3) {
          ImNodes::EndStaticAttribute();
          attr_open = 0;
        }
        break;
      case UiOp::NodeLink:
        if (editor_open)
          ImNodes::Link(cmd.i[0], static_cast<int>(cmd.f[0]), static_cast<int>(cmd.f[1]));
        break;
      case UiOp::SetNodePos: {
        const int id = cmd.i[0];
        const bool force = cmd.i[1] != 0;
        if (editor_open && (force || s.positioned.find(id) == s.positioned.end())) {
          ImNodes::SetNodeGridSpacePos(id, ImVec2(cmd.f[0], cmd.f[1]));
          s.positioned.insert(id);
        }
        break;
      }
      case UiOp::NodeMiniMap:
        // imnodes wants this called inside the editor, after the nodes and
        // links, just before EndNodeEditor -- if the script recorded it
        // anywhere else, `editor_open` is false here and it is ignored.
        if (editor_open) {
          const float frac = cmd.f[0] <= 0.0f ? 0.2f : cmd.f[0];
          ImNodes::MiniMap(frac, static_cast<ImNodesMiniMapLocation>(cmd.i[0]));
        }
        break;
      case UiOp::PushNodeColor: {
        // Same linearisation every other colour in this replayer gets
        // (ui_color(), sRGB in -> linear for the swapchain), then packed to
        // the ImU32 imnodes' own PushColorStyle takes.
        const ImVec4 c = ui_color(cmd.f[0], cmd.f[1], cmd.f[2], cmd.f[3]);
        const ImU32 packed =
            IM_COL32(static_cast<int>(std::clamp(c.x, 0.0f, 1.0f) * 255.0f + 0.5f),
                     static_cast<int>(std::clamp(c.y, 0.0f, 1.0f) * 255.0f + 0.5f),
                     static_cast<int>(std::clamp(c.z, 0.0f, 1.0f) * 255.0f + 0.5f),
                     static_cast<int>(std::clamp(c.w, 0.0f, 1.0f) * 255.0f + 0.5f));
        ImNodes::PushColorStyle(static_cast<ImNodesCol>(cmd.i[0]), packed);
        ++node_color_depth;
        break;
      }
      case UiOp::PopNodeColor:
        if (node_color_depth > 0) {
          ImNodes::PopColorStyle();
          --node_color_depth;
        }
        break;

      case UiOp::Count: break;  // never recorded; ignored if it somehow arrives
    }
  }

  // Whatever the script's recording forgot to close, closed here, so ImGui
  // never sees an unbalanced stack even though its own asserts for one are
  // compiled out in Release. Order: innermost-typical first, matching how
  // these actually nest in practice; each pair is an independent stack to
  // ImGui so the order between different kinds does not matter, only that
  // each kind closes LIFO with itself, which the loop above already keeps.
  while (child_depth-- > 0) ImGui::EndChild();
  while (group_depth-- > 0) ImGui::EndGroup();
  while (disabled_depth-- > 0) ImGui::EndDisabled();
  while (tabitem_open-- > 0) ImGui::EndTabItem();
  while (tabbar_open-- > 0) ImGui::EndTabBar();
  while (table_open-- > 0) ImGui::EndTable();
  while (!tree_stack.empty()) {
    if (tree_stack.back()) ImGui::TreePop();
    tree_stack.pop_back();
  }
  if (columns_active) ImGui::Columns(1);
  if (editor_open) close_editor();  // attribute -> title bar -> node -> editor
  if (style_color_depth > 0) ImGui::PopStyleColor(style_color_depth);
  while (item_width_depth-- > 0) ImGui::PopItemWidth();
  while (!id_stack.empty()) {
    id_stack.pop_back();
    ImGui::PopID();
  }
}

}  // namespace

std::unique_ptr<ScriptWindow> ScriptWindow::create(platform::IPlatformBackend& backend,
                                                    gpu::Instance& instance, gpu::Device& device,
                                                    float font_px, const UiWindowSpec& spec,
                                                    const ScriptWindowGeometry& wanted,
                                                    std::string* error) {
  const auto fail = [&](std::string msg) -> std::unique_ptr<ScriptWindow> {
    if (error) *error = std::move(msg);
    return nullptr;
  };
  if (!ui_valid_key(spec.key)) return fail("bad script window key");

  ScriptWindowGeometry g = wanted;
  fit_to_desktop(g);

  auto self = std::unique_ptr<ScriptWindow>(new ScriptWindow());
  self->p_ = std::make_unique<Impl>();
  Impl& s = *self->p_;
  s.key = spec.key;
  s.title = spec.title;
  s.remembered = g;

  // Decorated, resizable, opaque: a script window is a page of information,
  // not a piece of chrome, and it is not always-on-top — WorkerWindow's
  // reasoning applies unchanged since this is the same kind of window.
  const ImVec4 bg = ui_color(0.086f, 0.094f, 0.118f, 1.0f);
  std::string err;
  if (!s.open(backend, instance, device, font_px,
              {
                  .style = platform::WindowStyle::Decorated,
                  .title = spec.title,
                  .w = g.w,
                  .h = g.h,
                  .transparent = false,
                  .placement = ToolWindowPlacement::Position,
                  .x = g.x,
                  .y = g.y,
                  .clear_r = bg.x,
                  .clear_g = bg.y,
                  .clear_b = bg.z,
                  .clear_a = bg.w,
                  .noun = "script window",
              },
              &err)) {
    return fail(err);
  }

  // M30: imnodes' context is bound to an ImGui context the same way ImGui's
  // own is bound to a window -- create it now, with this window's ImGui
  // context current (ToolWindowCore::open() leaves it so; see ImGuiLayer's
  // own comment on why SetCurrentContext is called explicitly).
  ImNodes::SetImGuiContext(ImGui::GetCurrentContext());
  s.node_ctx = ImNodes::CreateContext();
  // How a graph is navigated (M30.1, from the first user report: "I cannot
  // navigate it"). imnodes pans with one configurable button, middle by
  // default; the user reached for the right button, so that is the pan
  // button here, and Alt+left drag pans as well for a mouse without one.
  // Ctrl+click on a pin detaches its link (imnodes then reports it destroyed,
  // which reaches the script as `link_destroyed`). These pointers are into
  // this window's own ImGuiIO, which lives as long as its context does.
  // Disabled text is ImGui's dim grey, tuned for the widget's dark panel; on
  // an imnodes node body (mid grey) it is almost invisible, which a user read
  // as the Japanese font being missing. Lifted here, for this window's own
  // context only, so `text_disabled` reads as a hint rather than vanishing.
  ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] = ui_color(0.82f, 0.82f, 0.84f, 1.0f);
  {
    ImNodesIO& nio = ImNodes::GetIO();
    nio.AltMouseButton = ImGuiMouseButton_Right;
    nio.EmulateThreeButtonMouse.Modifier = &ImGui::GetIO().KeyAlt;
    nio.LinkDetachWithModifierClick.Modifier = &ImGui::GetIO().KeyCtrl;
  }

  s.input = std::make_unique<ScriptInput>();
  if (!s.subclass(scriptProc, s.input.get())) {
    // Not survivable, WorkerWindow's rule again: a close box that reaches
    // SDL's identityless CloseRequested quits the whole application.
    return fail("could not subclass the script window for input");
  }
  // The ImGui context is still this window's here, which is what install()
  // captures. A failure is survivable: the window works without a keyboard.
  s.keys = WinTextInput::install(s.hwnd);
  if (!s.keys) log::warn("script window: {} has no keyboard (subclass refused)", spec.key);
  log::info("script window: {} up, hwnd {:p}, {}x{} at {},{}", spec.key,
            static_cast<void*>(s.hwnd), s.w, s.h, s.remembered.x, s.remembered.y);
  return self;
}

ScriptWindow::~ScriptWindow() {
  if (!p_) return;
  Impl& s = *p_;
  // The imnodes context goes before the ImGui layer it is bound to -- the
  // reverse of create()'s order, and for the same reason ToolWindowCore's own
  // shutdown() releases `ui` before `renderer`: whatever a context is bound
  // to must still be alive while it is torn down.
  // The keyboard subclass first: it points at the input state and the ImGui
  // context, both of which go below.
  s.keys.reset();
  if (s.node_ctx) {
    if (s.ui) s.ui->make_current();
    ImNodes::SetCurrentContext(s.node_ctx);
    ImNodes::DestroyContext(s.node_ctx);
    s.node_ctx = nullptr;
  }
  s.shutdown();
  log::info("script window: {} torn down", s.key);
}

HWND ScriptWindow::hwnd() const { return p_->hwnd; }
const std::string& ScriptWindow::key() const { return p_->key; }
ScriptWindowGeometry ScriptWindow::geometry() const { return p_->remembered; }

void ScriptWindow::retitle_resize(const UiWindowSpec& spec) {
  Impl& s = *p_;
  if (spec.title != s.title) {
    s.title = spec.title;
    if (s.hwnd) SetWindowTextW(s.hwnd, wide_from_utf8(spec.title).c_str());
  }
  // Through setSize()/resize(), never a raw SetWindowPos — the rule every
  // window on ToolWindowCore follows, stated at agent_menu_window.cpp's own
  // content-driven resize and worker_window.h's header comment.
  if (spec.w != s.w || spec.h != s.h) {
    s.w = spec.w;
    s.h = spec.h;
    if (s.target) s.target->setSize({s.w, s.h});
    if (s.renderer) s.renderer->resize(s.w, s.h);
  }
}

bool ScriptWindow::draw(float dt, UiBridge& bridge) {
  Impl& s = *p_;
  if (s.input && s.input->close_requested) return false;
  if (!s.ui) return true;

  // Minimised: nothing to draw and nothing worth remembering, the same rule
  // WorkerWindow follows for the same reason (a minimised rect sits at
  // -32000 and is not a position worth keeping).
  if (IsIconic(s.hwnd)) return true;

  const auto now = s.target->sizeInPixels();
  if (now.width != 0 && now.height != 0 && (now.width != s.w || now.height != s.h)) {
    s.w = now.width;
    s.h = now.height;
    s.renderer->resize(s.w, s.h);
  }
  if (s.w == 0 || s.h == 0) return true;

  if (!IsZoomed(s.hwnd)) {
    RECT wr{};
    if (GetWindowRect(s.hwnd, &wr)) {
      s.remembered.x = wr.left;
      s.remembered.y = wr.top;
      s.remembered.w = s.w;
      s.remembered.h = s.h;
      s.remembered.placed = true;
    }
  }

  // Copy the latest recording only when the bridge has a newer one — cheap
  // enough at the cap regardless, but the generation check is also what
  // clears the overrides at the right moment: a script that has recorded
  // again has, by the override's contract, already read the previous
  // results and decided what to draw next.
  const std::uint64_t ep = bridge.epoch_of(s.key);
  if (ep != s.epoch) {
    s.epoch = ep;
    s.overrides.clear();
    s.positioned.clear();
  }
  const std::uint64_t gen = bridge.generation_of(s.key);
  if (gen != s.generation) {
    UiFrame nf;
    if (bridge.frame_for(s.key, &nf)) {
      s.frame = std::move(nf);
      s.generation = gen;
      s.overrides.clear();
    }
  }

  s.ui->make_current();
  if (s.node_ctx) ImNodes::SetCurrentContext(s.node_ctx);
  ImGuiIO& io = ImGui::GetIO();

  POINT cursor{};
  if (GetCursorPos(&cursor)) {
    POINT local = cursor;
    if (ScreenToClient(s.hwnd, &local) && local.x >= 0 && local.y >= 0 &&
        local.x < static_cast<LONG>(s.w) && local.y < static_cast<LONG>(s.h))
      io.AddMousePosEvent(static_cast<float>(local.x), static_cast<float>(local.y));
    else
      io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  }
  for (const auto& [button, pressed] : s.input->buttons) io.AddMouseButtonEvent(button, pressed);
  s.input->buttons.clear();
  if (s.input->wheel != 0.0f) {
    io.AddMouseWheelEvent(0.0f, s.input->wheel);
    s.input->wheel = 0.0f;
  }

  s.ui->begin_frame(s.w, s.h, dt);
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(s.w), static_cast<float>(s.h)));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_color(0.086f, 0.094f, 0.118f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::Begin("##script", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

  std::vector<std::string> id_stack;
  std::vector<UiResult> results;
  replay(s, id_stack, results);

  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  if (!results.empty()) bridge.set_results(s.key, results);

  if (auto r = s.renderer->waitFrameSlot(); !r) {
    log::error("script window wait: {}", r.error().message);
    return true;
  }
  if (auto d = s.renderer->drawFrame(nullptr); !d)
    log::error("script window draw: {}", d.error().message);
  return true;
}

}  // namespace aii
