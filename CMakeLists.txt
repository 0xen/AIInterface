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
  src/audio/resample.cpp
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

  # ---- json_shape_test: a line from the CLI that is not the shape expected --
  # M15.2. Links aii_core because it drives a real ClaudeCodeClient -- one that
  # is constructed and never started, so no `claude` process and no engine is
  # loaded; the DLLs beside it are there only because aii_core imports them.
  add_executable(json_shape_test tests/json_shape_test.cpp)
  target_link_libraries(json_shape_test PRIVATE aii_core)
  if(MSVC)
    target_compile_options(json_shape_test PRIVATE /utf-8 /W4)
  endif()
  aii_copy_runtime_dlls(json_shape_test)

  # ---- client_kill_test: ending a child that will not end itself -----------
  # M17.3. Links aii_core for the same reason json_shape_test does, and starts
  # a real child -- but the child is `cmd.exe`, which ignores this app's whole
  # command line and then sits reading its stdin, which is exactly the shape of
  # the worker this finding is about. No `claude` process and no model call.
  add_executable(client_kill_test tests/client_kill_test.cpp)
  target_link_libraries(client_kill_test PRIVATE aii_core)
  if(MSVC)
    target_compile_options(client_kill_test PRIVATE /utf-8 /W4)
  endif()
  aii_copy_runtime_dlls(client_kill_test)

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

  # ---- seed_tree_test: what happens to a file the user has edited ----
  # user_paths.cpp is standard-library-plus-Win32, so the seeding rule is
  # checked against real directories under %TEMP% with no engine, no window
  # and -- the point of a test about %APPDATA% -- no %APPDATA%. The one symbol
  # it borrows, `env_or`, the test supplies itself rather than dragging in
  # config.cpp, which pulls the language table and the tool policy behind it.
  add_executable(seed_tree_test tests/seed_tree_test.cpp src/core/user_paths.cpp)
  target_include_directories(seed_tree_test PRIVATE src)
  target_compile_definitions(seed_tree_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(seed_tree_test PRIVATE /utf-8 /W4)
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

  # ---- barge_policy_test: when talking over the reply becomes a barge-in ----
  # Header-only, so there is no source but the test, for handoff_policy_test's
  # reason and one more: M18.0 measured the room, the leak and a person with the
  # speakers on, and the whole of M18.1 is arithmetic over those tables. Asking
  # "does it fire?" by talking over a reply is a test nobody can re-run and
  # nobody can make fail the same way twice; the frame sequences here are the
  # measured ones and they run in a millisecond, with no microphone, no speaker
  # and no room.
  add_executable(barge_policy_test tests/barge_policy_test.cpp)
  target_include_directories(barge_policy_test PRIVATE src)
  target_compile_definitions(barge_policy_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(barge_policy_test PRIVATE /utf-8 /W4)
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

  # ---- spsc_ring_test: the ring both audio callbacks now go through ---------
  # Header-only and standard-library-only, so M21.1's ring is tested with no
  # device, no engine DLL and no window -- including the concurrent case, which
  # is the whole point of it and cannot be reproduced by hand.
  add_executable(spsc_ring_test tests/spsc_ring_test.cpp)
  target_include_directories(spsc_ring_test PRIVATE src)
  target_compile_definitions(spsc_ring_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(spsc_ring_test PRIVATE /utf-8 /W4)
  endif()

  # ---- resample_test: a chunk that arrives at the wrong rate ----------------
  # resample.cpp is miniaudio and nothing else, so this pulls in the miniaudio
  # translation unit and no engine. It is the proof for M21.2's resampling
  # path, which no engine exercises today: both are 24 kHz, so the only way to
  # see it work is to hand it a synthetic 22.05 kHz chunk.
  add_executable(resample_test tests/resample_test.cpp src/audio/resample.cpp
                               src/audio/miniaudio_impl.cpp)
  target_include_directories(resample_test PRIVATE src third_party)
  target_compile_definitions(resample_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(resample_test PRIVATE /utf-8 /W4)
  endif()

  # ---- audio_out_bench: M21.1's measurement, not a test ---------------------
  # Opens the real playback device and takes twenty seconds, so it is not in
  # the acceptance list; it is how the before/after numbers in the milestone
  # were produced and how they can be reproduced. It carries its own copy of
  # the implementation it replaced so both halves run on one machine, one
  # device and one chunk.
  add_executable(audio_out_bench tests/audio_out_bench.cpp src/audio/audio_out.cpp
                                 src/audio/miniaudio_impl.cpp)
  target_include_directories(audio_out_bench PRIVATE src third_party)
  target_compile_definitions(audio_out_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(audio_out_bench PRIVATE /utf-8 /W4)
  endif()

  # ---- wake_word_test: M12's two decisions, both of them provable offline ----
  # Same rule as clip_utf8_test: wake_word.cpp is standard-library-only and
  # listen_restore.h is header-only, so this builds and runs with no engine DLL,
  # no window and -- the point -- no microphone. It is where M12's negative
  # cases live: the near-misses that must not open the mic, and the worker that
  # must not reopen a mic the user shut on purpose.
  #
  # text_util.cpp joins it at M24.2 and costs nothing: it is standard-library
  # only too, and it is where the one UTF-8 decoder now lives.
  add_executable(wake_word_test tests/wake_word_test.cpp src/core/wake_word.cpp
                                src/core/text_util.cpp)
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

  # ---- sentence_splitter_test: what the speech queue is handed, and when ----
  # M22.1, finding 26. Same rule as clip_utf8_test: sentence_splitter.cpp and
  # text_util.cpp are standard-library-only, so the early-chunk rule, the fence
  # handling and -- the case the probe behind commit 20676d5 never left behind
  # -- `break_now`'s promise to keep the stream state are all checked with no
  # engine, no window and no CLI.
  add_executable(sentence_splitter_test tests/sentence_splitter_test.cpp
                                        src/core/sentence_splitter.cpp
                                        src/core/text_util.cpp)
  target_include_directories(sentence_splitter_test PRIVATE src)
  target_compile_definitions(sentence_splitter_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(sentence_splitter_test PRIVATE /utf-8 /W4)
  endif()

  # ---- text_util_test: which voice says what --------------------------------
  # M22.1, finding 26. `split_by_script` is one of the four places that decide
  # "is this Japanese?" (docs/HANDOFF-2026-09-21.md) and the only one with no
  # test; `clip_utf8` keeps its own target above and is not repeated there.
  add_executable(text_util_test tests/text_util_test.cpp src/core/text_util.cpp)
  target_include_directories(text_util_test PRIVATE src)
  target_compile_definitions(text_util_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(text_util_test PRIVATE /utf-8 /W4)
  endif()

  # ---- app_bus_test: the bounds on a channel whose input is hostile ---------
  # M22.1, finding 26. app_bus.cpp needs json.hpp and text_util.cpp and nothing
  # else, so every cap, the inbox's drop-the-newest rule and the promise that
  # `post` is never fatal are checked without the script host anywhere near it.
  add_executable(app_bus_test tests/app_bus_test.cpp src/core/app_bus.cpp
                              src/core/text_util.cpp)
  target_include_directories(app_bus_test PRIVATE src third_party)
  target_compile_definitions(app_bus_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(app_bus_test PRIVATE /utf-8 /W4)
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

  # ---- prompt_format_test: a prompt written for another binary ------------
  # Same shape as the two above, and it needs the shipped assets for real:
  # the behaviour under test is "fall back to the copy that shipped with this
  # build", so `AII_ASSETS_DIR` -- already defined for aii_core -- is what it
  # falls back onto.
  add_executable(prompt_format_test tests/prompt_format_test.cpp)
  target_link_libraries(prompt_format_test PRIVATE aii_core)
  if(MSVC)
    target_compile_options(prompt_format_test PRIVATE /utf-8 /W4)
  endif()
  aii_copy_runtime_dlls(prompt_format_test)

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

  # ---- action_bytes_test: consent is to the bytes, not to the name -------
  # M19.1/M19.2, findings 11 and 12. Same shape as action_digest_test and for
  # the same reason -- the store is free of ImGui, the engine and the window --
  # but this one is about the consent record rather than the prompt, so it
  # works against real files under %TEMP% with APPDATA pointed at them.
  add_executable(action_bytes_test tests/action_bytes_test.cpp src/avatar/action_store.cpp)
  target_include_directories(action_bytes_test PRIVATE src src/avatar)
  target_link_libraries(action_bytes_test PRIVATE aii_core)
  target_compile_definitions(action_bytes_test PRIVATE AII_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
  if(MSVC)
    target_compile_options(action_bytes_test PRIVATE /utf-8 /W4)
  endif()
  aii_copy_runtime_dlls(action_bytes_test)
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
