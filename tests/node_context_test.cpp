// node_context_test: every script window's imnodes context gets its own
// navigation settings, not just the first one's (src/avatar/node_context.h).
//
// Reproduces the frame loop's order for two windows: create window A's ImGui
// and imnodes contexts, make A current as the frame loop does, then create
// window B's. Before create_node_context made the new context current, B's
// settings were written into A's context: B panned with the middle button
// only, and A's modifiers pointed at B's key state. No window, no GPU.
#include <cstdio>

#include "imgui.h"
#include "imnodes.h"

#include "node_context.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

struct Window {
  ImGuiContext* imgui = nullptr;
  ImNodesContext* nodes = nullptr;
};

Window open_window() {
  Window w;
  w.imgui = ImGui::CreateContext();
  ImGui::SetCurrentContext(w.imgui);
  w.nodes = aii::create_node_context(w.imgui);
  return w;
}

// What a window's frame does first: make its own contexts current.
void make_current(const Window& w) {
  ImGui::SetCurrentContext(w.imgui);
  ImNodes::SetCurrentContext(w.nodes);
}

void check_window(const Window& w, const char* name) {
  make_current(w);
  const ImNodesIO& io = ImNodes::GetIO();
  char what[160];
  std::snprintf(what, sizeof what, "%s pans with the right button", name);
  check(io.AltMouseButton == ImGuiMouseButton_Right, what);
  std::snprintf(what, sizeof what, "%s's Alt modifier is its own key state", name);
  check(io.EmulateThreeButtonMouse.Modifier == &ImGui::GetIO().KeyAlt, what);
  std::snprintf(what, sizeof what, "%s's Ctrl modifier is its own key state", name);
  check(io.LinkDetachWithModifierClick.Modifier == &ImGui::GetIO().KeyCtrl, what);
}

}  // namespace

int main() {
  std::printf("node_context_test\n");

  Window a = open_window();
  check(ImNodes::GetCurrentContext() == a.nodes, "a new context is current after creation (first)");
  make_current(a);  // the frame loop draws A before B is opened
  Window b = open_window();
  check(ImNodes::GetCurrentContext() == b.nodes, "a new context is current after creation (second)");

  check_window(a, "window A (opened first)");
  check_window(b, "window B (opened second)");

  // A third while B is current, then closing B: A and C are untouched.
  make_current(b);
  Window c = open_window();
  check_window(c, "window C (opened third)");
  make_current(b);
  ImNodes::DestroyContext(b.nodes);
  ImGui::DestroyContext(b.imgui);
  check_window(a, "window A after B closed");
  check_window(c, "window C after B closed");

  const Window* open[] = {&a, &c};
  for (const Window* w : open) {
    make_current(*w);
    ImNodes::DestroyContext(w->nodes);
    ImGui::DestroyContext(w->imgui);
  }

  std::printf(failures ? "FAILED (%d)\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
