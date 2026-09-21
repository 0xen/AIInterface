cmake_minimum_required(VERSION 3.24)
project(AIInterface VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# One shared bin directory: the Renderer engine writes rend.dll to
# ${CMAKE_BINARY_DIR}/bin/<Config>, so every executable here lands beside it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Prebuilt engines set up by the spikes (paths are kept there on purpose until
# the project graduates to a proper dependency layout). See README.md,
# "Getting the engines and models".
set(SHERPA_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/spikes/stt/bin/sherpa-onnx-v1.13.8-win-x64-shared-MD-Release")
set(VV_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spikes/tts_cpu/voicevox/voicevox_core")
set(MODELS_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/models")
foreach(v SHERPA_DIR VV_CORE_DIR MODELS_DIR)
  cmake_path(NORMAL_PATH ${v})
endforeach()

# ---- aii_core: everything the front-ends share ----
add_library(aii_core STATIC
  src/audio/miniaudio_impl.cpp
  src/audio/audio_out.cpp
  src/audio/mic_in.cpp
  src/stt/recognizer.cpp
  src/llm/claude_client.cpp
  src/llm/claude_code_client.cpp
  src/tts/kokoro_tts.cpp
  src/tts/voicevox_tts.cpp
  src/core/aii_block.cpp
  src/core/app_bus.cpp
  src/core/app_strings.cpp
  src/core/button_registry.cpp
  src/core/config.cpp
  src/core/cwd_policy.cpp
  src/core/directive_filter.cpp
  src/core/engines.cpp
  src/core/language.cpp
  src/core/memory_store.cpp
  src/core/model_choice.cpp
  src/core/prompt_store.cpp
  src/core/schedule.cpp
  src/core/sentence_splitter.cpp
  src/core/speech_queue.cpp
  src/core/text_util.cpp
  src/core/tool_policy.cpp
  src/core/user_paths.cpp
  src/core/wake_word.cpp
  src/core/worker_pool.cpp)
target_include_directories(aii_core PUBLIC
  src
  third_party
  "${SHERPA_DIR}/include"
  "${VV_CORE_DIR}/c_api/include")
target_link_libraries(aii_core PUBLIC
  "${SHERPA_DIR}/lib/sherpa-onnx-c-api.lib"
  "${VV_CORE_DIR}/c_api/lib/voicevox_core.lib"
  winhttp shell32 gdi32 user32)
target_compile_definitions(aii_core
  PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN
  PRIVATE AII_MODELS_DIR="${MODELS_DIR}" AII_VV_CORE_DIR="${VV_CORE_DIR}"
          AII_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
if(MSVC)
  target_compile_options(aii_core PRIVATE /utf-8 /W4 /wd4996)
endif()

# Copies the engine DLLs next to an executable after it links.
function(aii_copy_runtime_dlls target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${SHERPA_DIR}/lib/sherpa-onnx-c-api.dll"
      "${SHERPA_DIR}/lib/onnxruntime.dll"
      "${SHERPA_DIR}/lib/onnxruntime_providers_shared.dll"
      "${VV_CORE_DIR}/c_api/lib/voicevox_core.dll"
      $<TARGET_FILE_DIR:${target}>)
endfunction()

# ---- What a default build produces ----------------------------------------
# Only `avatar.exe`, the app. Everything else below is a development tool, and
# a tool built by default lands in the same folder as the thing the user runs.
# Each keeps its own switch so it can still be built on demand; see README.md,
# "Building the development targets".
option(AII_BUILD_TESTS "Build the unit tests (default OFF; -DAII_BUILD_TESTS=ON to run them)" OFF)
option(AII_BUILD_VOICELOOP "Build the milestone-1 console voice loop (no window)" OFF)

# `latest/` is for release packages, and this project is not at that stage, so
# staging is off by default (user's instruction, 19 Sep 2026). A default build
# produces `build/bin/Release/avatar.exe` and that is the thing to run: it
# already sits beside every DLL it loads, `data/` and `pre-prompt.md`.
#
# The machinery is intact, not removed -- turn it on explicitly with
# `-DAII_STAGE_LATEST=ON` when assembling a release package, and it will
# assemble the app into one folder holding the executable and everything it
# loads. Only the app is staged: the tests and voiceloop above are development
# tools and do not belong beside the thing the user runs. See
# src/avatar/CMakeLists.txt and cmake/StageLatest.cmake.
option(AII_STAGE_LATEST "Assemble the runnable app into <source>/latest after each build (default OFF; for release packaging, -DAII_STAGE_LATEST=ON)" OFF)
set(AII_LATEST_DIR "${CMAKE_SOURCE_DIR}/latest" CACHE PATH "Where the build stages the runnable app")

if(AII_BUILD_VOICELOOP)
  # ---- voiceloop: the console push-to-talk loop (milestone 1) ----
  add_executable(voiceloop src/main.cpp)
  target_link_libraries(voiceloop PRIVATE aii_core)
  if(MSVC)
    target_compile_options(voiceloop PRIVATE /utf-8 /W4 /wd4996)
  endif()
  aii_copy_runtime_dlls(voiceloop)
endif()

if(AII_BUILD_TESTS)
  # ---- schedule_test: M2b.1's harness ----
  # Deliberately *not* linked against aii_core. The schedule primitive depends on
  # nothing but the standard library, and a test that dragged in sherpa-onnx and
  # VOICEVOX would need their DLLs beside it to start — a timing harness should
  # not be able to fail for a reason that has nothing to do with timing.
  # text_util.cpp comes along because schedule.cpp now clips its strings with the
  # shared clip_utf8; it is standard-library-only too, so the rule above holds.
  add_executable(schedule_test tests/schedule_test.cpp src/core/schedule.cpp
                               src/core/cwd_policy.cpp
                               src/core/text_util.cpp)
  target_include_directories(schedule_test PRIVATE src)
  target_link_libraries(schedule_test PRIVATE winmm)
  target_compile_definitions(schedule_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(schedule_test PRIVATE /utf-8 /W4)
  endif()

  # ---- cwd_policy_test: where a worker lands, and who decides it ----
  # Same rule as schedule_test: cwd_policy.cpp is standard-library-only, so the
  # one decision that can point a bypassPermissions child at a directory is
  # testable without an engine DLL anywhere near it.
  add_executable(cwd_policy_test tests/cwd_policy_test.cpp src/core/cwd_policy.cpp)
  target_include_directories(cwd_policy_test PRIVATE src)
  target_compile_definitions(cwd_policy_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(cwd_policy_test PRIVATE /utf-8 /W4)
  endif()

  # ---- aii_block_test: what the assistant is allowed to ask this app to do ----
  # M15.1. The reason this target can exist at all is that `aii_block.cpp` was
  # split out of `worker_pool.cpp`, which owns a ClaudeCodeClient and so drags
  # the whole process-spawning half of the app behind it; `schedule_test.cpp`
  # says in as many words that this is why the parser had no test. It now needs
  # text_util.cpp and nothing else.
  add_executable(aii_block_test tests/aii_block_test.cpp src/core/aii_block.cpp
                                src/core/text_util.cpp)
  target_include_directories(aii_block_test PRIVATE src)
  target_compile_definitions(aii_block_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(aii_block_test PRIVATE /utf-8 /W4)
  endif()

  # ---- memory_store_test: the file the user's "remember this" lands in ----
  # Same rule as cwd_policy_test: memory_store.cpp is standard-library-only, so
  # stable ids, the cap and the hand-edit rules are all checked against a temp
  # file with no engine, no window and no CLI.
  add_executable(memory_store_test tests/memory_store_test.cpp src/core/memory_store.cpp)
  target_include_directories(memory_store_test PRIVATE src)
  target_compile_definitions(memory_store_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(memory_store_test PRIVATE /utf-8 /W4)
  endif()

  # ---- handoff_policy_test: when the session hands over to itself ----
  # Header-only, so there is no source but the test: `core/handoff_policy.h` is
  # M3.15's whole firing rule, lifted out of VoiceSession for exactly this --
  # the question "does it fire?" is arithmetic, and answering it by holding an
  # 80,000-token conversation is not a test anybody would re-run.
  add_executable(handoff_policy_test tests/handoff_policy_test.cpp)
  target_include_directories(handoff_policy_test PRIVATE src)
  target_compile_definitions(handoff_policy_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(handoff_policy_test PRIVATE /utf-8 /W4)
  endif()

  # ---- clip_utf8_test: the shared byte cap that must not split a character ----
  # Same rule as schedule_test: text_util.cpp is standard-library-only, so this
  # builds and runs without an engine DLL anywhere near it. It reads the two caps
  # out of app_bus.h and schedule.h (both header-only constants) so the test
  # fails if either one drifts.
  add_executable(clip_utf8_test tests/clip_utf8_test.cpp src/core/text_util.cpp)
  target_include_directories(clip_utf8_test PRIVATE src)
  target_compile_definitions(clip_utf8_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(clip_utf8_test PRIVATE /utf-8 /W4)
  endif()

  # ---- wake_word_test: M12's two decisions, both of them provable offline ----
  # Same rule as clip_utf8_test: wake_word.cpp is standard-library-only and
  # listen_restore.h is header-only, so this builds and runs with no engine DLL,
  # no window and -- the point -- no microphone. It is where M12's negative
  # cases live: the near-misses that must not open the mic, and the worker that
  # must not reopen a mic the user shut on purpose.
  add_executable(wake_word_test tests/wake_word_test.cpp src/core/wake_word.cpp)
  target_include_directories(wake_word_test PRIVATE src)
  target_compile_definitions(wake_word_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(wake_word_test PRIVATE /utf-8 /W4)
  endif()

  # ---- module_reference_test: the script reference, against the module ------
  # Reads two files and compares them, so it links nothing at all. Both paths
  # are baked in: the assets directory the same way every other target gets it,
  # and the pyhost source because the reference is prose *about* that file and
  # the point is to notice when the two drift apart.
  add_executable(module_reference_test tests/module_reference_test.cpp)
  target_compile_definitions(module_reference_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
    AII_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets"
    AII_PYHOST_SOURCE="${CMAKE_SOURCE_DIR}/src/pyhost/aii_pyhost.cpp")
  if(MSVC)
    target_compile_options(module_reference_test PRIVATE /utf-8 /W4)
  endif()

  # ---- directive_filter_test: what is machine traffic and what is English ----
  # Same rule as clip_utf8_test: directive_filter.cpp is standard-library-only,
  # so M13.1's grammar and -- the part a probe caught the obvious design failing
  # -- its one-delta/one-byte equivalence are both checked with no engine, no
  # window and no CLI. The streaming property cannot be reproduced by hand at
  # all: it needs the same reply fed at eight different chunk sizes.
  add_executable(directive_filter_test tests/directive_filter_test.cpp
                                       src/core/directive_filter.cpp)
  target_include_directories(directive_filter_test PRIVATE src)
  target_compile_definitions(directive_filter_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(directive_filter_test PRIVATE /utf-8 /W4)
  endif()

  # ---- app_strings_test: the canned lines, and that their bytes survive MSVC ----
  # Same rule as schedule_test and clip_utf8_test: app_strings.cpp and
  # text_util.cpp are standard-library-only, so this builds and runs with no
  # engine DLL anywhere near it. /utf-8 matters here more than anywhere else in
  # the build -- without it MSVC reads the Japanese in app_strings.cpp as the
  # system codepage and the byte assertions in this test are what catches it.
  add_executable(app_strings_test tests/app_strings_test.cpp src/core/app_strings.cpp
                                  src/core/text_util.cpp)
  target_include_directories(app_strings_test PRIVATE src)
  target_compile_definitions(app_strings_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(app_strings_test PRIVATE /utf-8 /W4)
  endif()

  # ---- worker_report_test: what a finished worker hands to the speech path ----
  # Links aii_core because it drives a real WorkerPool and a real Claude Code
  # child; no engines are loaded, so the DLLs beside it are only there because
  # aii_core imports them.
  add_executable(worker_report_test tests/worker_report_test.cpp)
  target_link_libraries(worker_report_test PRIVATE aii_core)
  if(MSVC)
    target_compile_options(worker_report_test PRIVATE /utf-8 /W4)
  endif()
  aii_copy_runtime_dlls(worker_report_test)

  # ---- failure_reason_test: the CLI's jargon, mapped to something hearable ----
  # Same rule as app_strings_test, and the same two files: the mapping lives
  # beside the table it selects from, so it needs nothing else. The Japanese in
  # it is checked by app_strings_test's byte assertions.
  add_executable(failure_reason_test tests/failure_reason_test.cpp src/core/app_strings.cpp
                                     src/core/text_util.cpp)
  target_include_directories(failure_reason_test PRIVATE src)
  target_compile_definitions(failure_reason_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(failure_reason_test PRIVATE /utf-8 /W4)
  endif()

  # ---- prompt_body_test: a declared prompt body that cannot be read ----
  # Links aii_core: PromptStore reaches user_paths and config, and build_inventory
  # is in the same translation unit. It builds its own store under the temp
  # directory and points AII_PROMPTS_DIR at it, so it never touches the user's.
  add_executable(prompt_body_test tests/prompt_body_test.cpp)
  target_link_libraries(prompt_body_test PRIVATE aii_core)
  if(MSVC)
    target_compile_options(prompt_body_test PRIVATE /utf-8 /W4)
  endif()
  aii_copy_runtime_dlls(prompt_body_test)

  # ---- prompt_tools_test: the prompt describes the grant that was made ----
  # Same shape as prompt_body_test, and the same care with AII_PROMPTS_DIR --
  # except that this one seeds the *real* assets/prompts into its temp store,
  # because the property it checks is about the shipped prose.
  add_executable(prompt_tools_test tests/prompt_tools_test.cpp)
  target_link_libraries(prompt_tools_test PRIVATE aii_core)
  if(MSVC)
    target_compile_options(prompt_tools_test PRIVATE /utf-8 /W4)
  endif()
  aii_copy_runtime_dlls(prompt_tools_test)

  # ---- action_digest_test: an unarmed action is visible as unarmed --------
  # Compiles `src/avatar/action_store.cpp` directly rather than linking the
  # avatar target: the store is deliberately free of ImGui, the engine and the
  # window, so it can be checked without a GPU. AII_ASSETS_DIR is needed
  # because `load()` seeds the shipped example.
  add_executable(action_digest_test tests/action_digest_test.cpp src/avatar/action_store.cpp)
  target_include_directories(action_digest_test PRIVATE src src/avatar)
  target_link_libraries(action_digest_test PRIVATE aii_core)
  target_compile_definitions(action_digest_test PRIVATE AII_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
  if(MSVC)
    target_compile_options(action_digest_test PRIVATE /utf-8 /W4)
  endif()
  aii_copy_runtime_dlls(action_digest_test)
endif()

# ---- avatar: the corner window on the Renderer engine (submodule) ----
#
# The engine lives in `third_party/Renderer`, a submodule pinned to the commit
# this app is known to build against. That pin is the whole point: a clone on
# another machine gets *that* engine, not whatever its master happens to be.
#
# A sibling checkout is still honoured, and is how the engine itself is
# developed from here -- but only when asked for by name, so that a stray
# `..\Renderer` on someone else's disk can never silently replace the pin:
#
#     cmake -S . -B build-engine -DAII_RENDERER_DIR=C:/github/Renderer
#
# Note the variable is cached, so a build directory configured before the
# submodule existed keeps the sibling path it was configured with.
option(AII_BUILD_AVATAR "Build the avatar window (needs the Renderer submodule and the Vulkan SDK's dxc)" ON)
set(AII_RENDERER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Renderer"
    CACHE PATH "Checkout of the rend engine (default: the third_party/Renderer submodule)")
if(AII_BUILD_AVATAR)
  if(NOT EXISTS "${AII_RENDERER_DIR}/CMakeLists.txt")
    if(AII_RENDERER_DIR STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Renderer")
      message(FATAL_ERROR
        "The Renderer submodule at ${AII_RENDERER_DIR} is empty.\n"
        "This clone was made without it. Fetch it with:\n"
        "    git submodule update --init --recursive\n"
        "(It is a private repository, so this needs a GitHub account with access.)\n"
        "To build without the window instead, configure with -DAII_BUILD_AVATAR=OFF.")
    endif()
    message(FATAL_ERROR
      "Renderer not found at ${AII_RENDERER_DIR}; correct AII_RENDERER_DIR, or unset it "
      "to use the third_party/Renderer submodule, or configure with -DAII_BUILD_AVATAR=OFF.")
  endif()
  include(cmake/FetchImgui.cmake)
  set(REND_BUILD_APPS OFF CACHE BOOL "" FORCE)
  # M2.6. The engine's own embedded-CPython host, in process, as the user
  # asked: "just like the viewer application python is ran in our code. Not as
  # a separate process." It used to be unbuildable from here — the engine
  # gated `pyhost` behind REND_BUILD_APPS *and* REND_PYTHON, and we force the
  # first off — so the engine's gate was split and REND_PYTHON now stands
  # alone, which is what its own comment always claimed.
  #
  # Turning this off costs the app nothing but scripting: no script is ever
  # discovered, `avatar.exe` links none of it either way, and the built-in
  # policy is what runs when nothing is scripting.
  option(AII_BUILD_SCRIPTS "Build the Python script host (M2.6)" ON)
  set(REND_PYTHON ${AII_BUILD_SCRIPTS} CACHE BOOL "" FORCE)
  add_subdirectory("${AII_RENDERER_DIR}" "${CMAKE_BINARY_DIR}/renderer")
  if(AII_BUILD_SCRIPTS)
    add_subdirectory(src/pyhost)
  endif()
  add_subdirectory(src/avatar)

  if(AII_BUILD_TESTS)
    # ---- avatar_children_test: M7.5's child slime, without a window -----------
    # Inside the avatar block because avatar_def.h reaches AvatarGrid in
    # avatar_renderer.h, so the engine's headers have to be on the path; nothing
    # here draws, and no window is opened. AvatarController is a pure function of
    # a VoiceSession::Snapshot plus its own timers, so a hand-written worker list
    # run at a fixed 60 Hz reproduces exactly what a real pool makes the policy
    # do — in milliseconds, instead of minutes of real Claude child processes.
    add_executable(avatar_children_test tests/avatar_children_test.cpp
                                        src/avatar/avatar_controller.cpp
                                        src/avatar/avatar_def.cpp
                                        src/avatar/avatar_renderer.cpp)
    target_include_directories(avatar_children_test PRIVATE src src/avatar)
    target_link_libraries(avatar_children_test PRIVATE aii_core rend::engine)
    target_compile_definitions(avatar_children_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
      AII_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
    if(MSVC)
      target_compile_options(avatar_children_test PRIVATE /utf-8 /W4 /wd4996)
    endif()
    aii_copy_runtime_dlls(avatar_children_test)
  endif()
endif()
