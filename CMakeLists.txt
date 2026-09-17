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
  src/core/app_bus.cpp
  src/core/button_registry.cpp
  src/core/config.cpp
  src/core/engines.cpp
  src/core/language.cpp
  src/core/prompt_store.cpp
  src/core/sentence_splitter.cpp
  src/core/speech_queue.cpp
  src/core/text_util.cpp
  src/core/user_paths.cpp
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

# ---- voiceloop: the console push-to-talk loop (milestone 1) ----
add_executable(voiceloop src/main.cpp)
target_link_libraries(voiceloop PRIVATE aii_core)
if(MSVC)
  target_compile_options(voiceloop PRIVATE /utf-8 /W4 /wd4996)
endif()
aii_copy_runtime_dlls(voiceloop)

# ---- avatar: the corner window on the Renderer engine (sibling checkout) ----
option(AII_BUILD_AVATAR "Build the avatar window (needs the Renderer checkout and the Vulkan SDK's dxc)" ON)
set(AII_RENDERER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Renderer" CACHE PATH "Checkout of the rend engine")
if(AII_BUILD_AVATAR)
  if(NOT EXISTS "${AII_RENDERER_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Renderer not found at ${AII_RENDERER_DIR}; set AII_RENDERER_DIR or AII_BUILD_AVATAR=OFF")
  endif()
  include(cmake/FetchImgui.cmake)
  set(REND_BUILD_APPS OFF CACHE BOOL "" FORCE)
  set(REND_PYTHON OFF CACHE BOOL "" FORCE)
  add_subdirectory("${AII_RENDERER_DIR}" "${CMAKE_BINARY_DIR}/renderer")
  add_subdirectory(src/avatar)
endif()
