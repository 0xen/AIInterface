# The avatar window: a small borderless transparent always-on-top window on
# the rend engine's D3D12 backend (the only backend whose swapchain
# composites with alpha on the AMD driver here). It draws the 2D pixel
# avatar, an ImGui panel (status bar, collapsible chat, buttons) and runs the
# voice loop from aii_core.
add_executable(avatar
  main.cpp
  avatar_appearance.cpp
  avatar_appearance.h
  avatar_controller.cpp
  avatar_controller.h
  avatar_def.cpp
  avatar_def.h
  action_store.cpp
  action_store.h
  agent_menu_window.cpp
  agent_menu_window.h
  approval_window.cpp
  approval_window.h
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
  tool_window_core.cpp
  tool_window_core.h
  voice_session.cpp
  voice_session.h
  watermark.cpp
  watermark.h
  worker_strip_window.cpp
  worker_strip_window.h
  worker_window.cpp
  worker_window.h
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
# No console window (user, 19 Sep 2026).
#
# The console this app opened was not a child process and not the engine: a
# console-subsystem binary is *given* one by Windows at startup whether or not
# it ever writes a byte, and this target had no WIN32_EXECUTABLE. The child
# `claude` processes were never the cause -- claude_code_client.cpp already
# spawns them CREATE_NO_WINDOW.
#
# /ENTRY:mainCRTStartup keeps `int main` rather than adding a WinMain shim. A
# shim would buy nothing here: main() ignores argc/argv already and parses
# CommandLineToArgvW(GetCommandLineW()) instead, so the windowed subsystem
# costs this app no argument handling at all.
#
# What it *does* cost is stdout, and that is paid for in routeDiagnostics() at
# the top of main.cpp -- read that before changing anything here, because
# without it every diagnostic in this project goes nowhere and says nothing
# about having done so.
set_target_properties(avatar PROPERTIES WIN32_EXECUTABLE TRUE)
if(MSVC)
  target_link_options(avatar PRIVATE /ENTRY:mainCRTStartup)
endif()
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
# Named rather than inlined because the staging step at the bottom of this
# file has to know which shader artifacts *this* build declares, rather than
# whatever happens to be lying in the build tree.
set(AII_SHADER_STAGES
    "avatar_grid.hlsl|VSMain|vs_6_0|avatar_grid.vert.spv"
    "avatar_grid.hlsl|PSMain|ps_6_0|avatar_grid.frag.spv"
    "loader.hlsl|VSMain|vs_6_0|loader.vert.spv"
    "loader.hlsl|PSMain|ps_6_0|loader.frag.spv")
rend_add_shaders(avatar_shaders
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
  OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/data/shaders"
  STAGES ${AII_SHADER_STAGES})
set_target_properties(avatar_shaders PROPERTIES FOLDER "assets")
add_dependencies(avatar avatar_shaders)

# ---- latest/: the runnable application, in one folder ----------------------
# `build\bin\<config>\` is a build tree, not a place to hand someone: it
# accumulates (a shader whose .hlsl was deleted is still there; an executable
# built while an option was on is not removed when it goes off), and its path
# changes with the configuration. So the build also assembles exactly what the
# app needs into `latest/` beside the sources.
#
# What goes in is enumerated, not swept up: the executable, the four DLLs it
# loads by name, the Python host trio (only when it is built -- it is
# LoadLibrary'd, and only on a run that has a script), and the shader
# artifacts this build declares. Nothing else in the build tree is required to
# start the app. cmake/StageLatest.cmake also removes anything a previous
# build staged that this one does not want, so latest/ cannot go stale behind
# a name that says it is current.
#
# It is one config at a time -- whatever was built most recently, named in
# BUILD-INFO.txt -- and every file in it comes from that same build, so the
# folder is never a mixture.
if(AII_STAGE_LATEST)
  set(_aii_bin "$<TARGET_FILE_DIR:avatar>")
  set(_aii_manifest_lines
    "copy|$<TARGET_FILE_NAME:avatar>|$<TARGET_FILE:avatar>"
    "copy|$<TARGET_FILE_NAME:rend::engine>|$<TARGET_FILE:rend::engine>")
  if(AII_BUILD_SCRIPTS)
    # python*.dll is a glob because the engine's FetchPython.cmake picks the
    # CPython version; hard-coding python313.dll here would rot silently.
    list(APPEND _aii_manifest_lines
      "copy|$<TARGET_FILE_NAME:rend_pyhost>|$<TARGET_FILE:rend_pyhost>"
      "copy|$<TARGET_FILE_NAME:aii_pyhost>|$<TARGET_FILE:aii_pyhost>"
      "glob|.|${_aii_bin}/python*.dll")
  endif()
  foreach(_dll sherpa-onnx-c-api.dll onnxruntime.dll onnxruntime_providers_shared.dll)
    list(APPEND _aii_manifest_lines "copy|${_dll}|${SHERPA_DIR}/lib/${_dll}")
  endforeach()
  list(APPEND _aii_manifest_lines
    "copy|voicevox_core.dll|${VV_CORE_DIR}/c_api/lib/voicevox_core.dll")

  foreach(_stage IN LISTS AII_SHADER_STAGES)
    string(REPLACE "|" ";" _parts "${_stage}")
    list(GET _parts 3 _artifact)
    set(_artifacts "${_artifact}")
    if(REND_DXIL)
      string(REGEX REPLACE "[.]spv$" ".dxil" _dxil "${_artifact}")
      list(APPEND _artifacts "${_dxil}")
    endif()
    foreach(_a IN LISTS _artifacts)
      list(APPEND _aii_manifest_lines
        "copy|data/shaders/${_a}|${_aii_bin}/data/shaders/${_a}")
    endforeach()
  endforeach()

  set(_aii_manifest "${CMAKE_BINARY_DIR}/aii-latest-$<CONFIG>.manifest")
  string(JOIN "\n" _aii_manifest_text ${_aii_manifest_lines})
  file(GENERATE OUTPUT "${_aii_manifest}" CONTENT "${_aii_manifest_text}\n")

  # Last POST_BUILD step on avatar, so the DLL copies above, the Python host's
  # own copy step and the shader target have all run by the time it looks.
  add_custom_command(TARGET avatar POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      "-DMANIFEST=${_aii_manifest}"
      "-DLATEST_DIR=${AII_LATEST_DIR}"
      "-DCONFIG=$<CONFIG>"
      "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
      -P "${CMAKE_SOURCE_DIR}/cmake/StageLatest.cmake"
    COMMENT "Staging the runnable app into ${AII_LATEST_DIR}"
    VERBATIM)
endif()
