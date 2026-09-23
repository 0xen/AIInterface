#include "node_context.h"

#include "imgui.h"
#include "imnodes.h"

namespace aii {

ImNodesContext* create_node_context(ImGuiContext* imgui) {
  ImNodes::SetImGuiContext(imgui);
  ImNodesContext* ctx = ImNodes::CreateContext();
  // CreateContext() only makes `ctx` current when none was; see the header.
  ImNodes::SetCurrentContext(ctx);
  // How a graph is navigated (M30.1, from the first user report: "I cannot
  // navigate it"). imnodes pans with one configurable button, middle by
  // default; the user reached for the right button, so that is the pan
  // button here, and Alt+left drag pans as well for a mouse without one.
  // Ctrl+click on a pin detaches its link (imnodes then reports it destroyed,
  // which reaches the script as `link_destroyed`). These pointers are into
  // this window's own ImGuiIO, which lives as long as its context does.
  ImNodesIO& nio = ImNodes::GetIO();
  nio.AltMouseButton = ImGuiMouseButton_Right;
  nio.EmulateThreeButtonMouse.Modifier = &ImGui::GetIO().KeyAlt;
  nio.LinkDetachWithModifierClick.Modifier = &ImGui::GetIO().KeyCtrl;
  return ctx;
}

}  // namespace aii
