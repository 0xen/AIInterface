# The avatar window: a small borderless transparent always-on-top window on
# the rend engine's D3D12 backend (the only backend whose swapchain
# composites with alpha on the AMD driver here). It draws the 2D pixel
# avatar, an ImGui panel (status bar, collapsible chat, buttons) and runs the
# voice loop from aii_core.
add_executable(avatar
  main.cpp
  avatar_controller.cpp
  avatar_controller.h
  avatar_def.cpp
  avatar_def.h
  avatar_renderer.cpp
  avatar_renderer.h
  bus_bindings.cpp
  bus_bindings.h
  avatar_ui.cpp
  avatar_ui.h
  imgui_layer.cpp
  imgui_layer.h
  inspector_list.cpp
  inspector_list.h
  inspector_window.cpp
  inspector_window.h
  loader_anim.cpp
  loader_anim.h
  pixel_icons.cpp
  pixel_icons.h
  script_host.cpp
  script_host.h
  settings.cpp
  settings.h
  sidebar_window.cpp
  sidebar_window.h
  voice_session.cpp
  voice_session.h
  watermark.cpp
  watermark.h
  win_text_input.cpp
  win_text_input.h)
target_include_directories(avatar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  # M2.6: the engine's Python-host C ABI, header-only. Included path-only with
  # no link, exactly as apps/viewer does it — the DLL is resolved at runtime,
  # and only on a run that has a script.
  "${AII_RENDERER_DIR}/pyhost/include")
# Comctl32 for SetWindowSubclass: the window's HWND is how this app gets a
# character event at all, since the engine's portable event set has none (B1).
target_link_libraries(avatar PRIVATE aii_core rend::engine imgui Comctl32)
# Where avatar definitions are seeded *from* on first run. Baked in the same
# way aii_core bakes the model directories, because the destination
# (%APPDATA%) is the only copy that is ever read afterwards — this path is
# touched once, when the user has no definition of their own yet.
target_compile_definitions(avatar PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
  AII_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
if(MSVC)
  target_compile_options(avatar PRIVATE /utf-8 /W4 /wd4996)
endif()
set_target_properties(avatar PROPERTIES FOLDER "apps")
# M2.6. Neither DLL is linked — both are LoadLibrary'd, and only on a run that
# has a script — but the documented build command is `--target avatar`, and a
# feature that silently is not there because a second target was not named is
# the kind of thing that gets debugged as a code bug.
if(AII_BUILD_SCRIPTS)
  add_dependencies(avatar aii_pyhost rend_pyhost)
endif()
aii_copy_runtime_dlls(avatar)

# Our HLSL, compiled by the engine's dxc rule into data/shaders next to the
# executable (SPIR-V plus DXIL; the D3D12 backend swaps .spv for .dxil).
rend_add_shaders(avatar_shaders
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
  OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/data/shaders"
  STAGES
    "avatar_grid.hlsl|VSMain|vs_6_0|avatar_grid.vert.spv"
    "avatar_grid.hlsl|PSMain|ps_6_0|avatar_grid.frag.spv"
    "loader.hlsl|VSMain|vs_6_0|loader.vert.spv"
    "loader.hlsl|PSMain|ps_6_0|loader.frag.spv")
set_target_properties(avatar_shaders PROPERTIES FOLDER "assets")
add_dependencies(avatar avatar_shaders)
