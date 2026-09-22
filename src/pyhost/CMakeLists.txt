# aii_pyhost — the `aii` Python module, in a DLL of its own (M2.6).
#
# Nothing links it. `avatar.exe` LoadLibrary's it only on a run that has a
# script, exactly as the viewer treats rend_pyhost.dll, and for a reason
# beyond symmetry: linking Python into the exe would map python313.dll at
# process start for every user, scripting or not, because CPython is imported
# partly as data and the delay-load helper only covers functions.
#
# It compiles app_bus.cpp rather than linking aii_core, and it is handed the
# app's AppBus across the C ABI. aii_core is a static library: linking it here
# would give this DLL its own AppBus::instance(), and the two halves of the
# app would post into different queues in complete silence.
add_library(aii_pyhost SHARED
  aii_pyhost.cpp
  aii_pyhost.h
  ../core/app_bus.cpp
  ../core/text_util.cpp
  ../core/ui_bridge.cpp)
target_include_directories(aii_pyhost PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/third_party")
target_link_libraries(aii_pyhost PRIVATE pybind11::embed)
target_compile_definitions(aii_pyhost PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(aii_pyhost PRIVATE /utf-8 /W4 /wd4996)
endif()
set_target_properties(aii_pyhost PROPERTIES
  OUTPUT_NAME "aii_pyhost"
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  FOLDER "apps")
