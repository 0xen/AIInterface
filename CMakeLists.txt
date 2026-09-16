cmake_minimum_required(VERSION 3.24)
project(AIInterface VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Prebuilt engines set up by the spikes (paths are kept there on purpose until
# the project graduates to a proper dependency layout).
set(SHERPA_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/spikes/stt/bin/sherpa-onnx-v1.13.8-win-x64-shared-MD-Release")
set(VV_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spikes/tts_cpu/voicevox/voicevox_core")
set(MODELS_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/models")
foreach(v SHERPA_DIR VV_CORE_DIR MODELS_DIR)
  cmake_path(NORMAL_PATH ${v})
endforeach()

add_executable(voiceloop
  src/main.cpp
  src/audio/miniaudio_impl.cpp
  src/audio/audio_out.cpp
  src/audio/mic_in.cpp
  src/stt/recognizer.cpp
  src/llm/claude_client.cpp
  src/llm/claude_code_client.cpp
  src/tts/kokoro_tts.cpp
  src/tts/voicevox_tts.cpp
  src/core/sentence_splitter.cpp
  src/core/speech_queue.cpp
  src/core/text_util.cpp)

target_include_directories(voiceloop PRIVATE
  src
  third_party
  "${SHERPA_DIR}/include"
  "${VV_CORE_DIR}/c_api/include")

target_link_libraries(voiceloop PRIVATE
  "${SHERPA_DIR}/lib/sherpa-onnx-c-api.lib"
  "${VV_CORE_DIR}/c_api/lib/voicevox_core.lib"
  winhttp shell32)

target_compile_definitions(voiceloop PRIVATE
  NOMINMAX WIN32_LEAN_AND_MEAN
  AII_MODELS_DIR="${MODELS_DIR}"
  AII_VV_CORE_DIR="${VV_CORE_DIR}")

if(MSVC)
  target_compile_options(voiceloop PRIVATE /utf-8 /W4 /wd4996)
endif()

add_custom_command(TARGET voiceloop POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SHERPA_DIR}/lib/sherpa-onnx-c-api.dll"
    "${SHERPA_DIR}/lib/onnxruntime.dll"
    "${SHERPA_DIR}/lib/onnxruntime_providers_shared.dll"
    "${VV_CORE_DIR}/c_api/lib/voicevox_core.dll"
    $<TARGET_FILE_DIR:voiceloop>)
