#pragma once
// One script window's imnodes context, created and configured for that window.
//
// Every script window has its own ImGui context and its own imnodes context,
// because imnodes' state is one global (`GImNodes`) exactly like ImGui's. The
// navigation settings -- right-drag pans, Alt+left drag pans, Ctrl+click on a
// pin detaches its link -- live in the imnodes context's IO, so they have to
// be written into *this* window's context.
//
// That is the whole reason this is a function of its own. imnodes'
// CreateContext() makes the new context current only when no context is
// current yet. Before this was split out, the settings were written through
// ImNodes::GetIO() right after CreateContext(). For the first script window
// that was its own context. For every window after it, it was whichever
// window's context the frame loop had last made current. So the second
// node-graph window panned with the middle button only, and the first was
// left holding pointers to the second's key state. `tests/node_context_test.cpp`
// holds the two-window case.

struct ImGuiContext;
struct ImNodesContext;

namespace aii {

// Creates an imnodes context bound to `imgui` (which must be current), makes
// it current, and configures its IO for this window. The caller owns it and
// destroys it with ImNodes::DestroyContext.
ImNodesContext* create_node_context(ImGuiContext* imgui);

}  // namespace aii
