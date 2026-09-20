# Fetches Dear ImGui for the avatar window, pinned to the same tag the
# Renderer checkout uses. The repo ships no CMake project, so the core plus
# one backend is built here as a static lib.
#
# The D3D12 backend, not the Vulkan one the engine's own viewer uses: the
# avatar needs a per-pixel-alpha swapchain, which on the AMD (RDNA3) driver
# this was developed against only D3D12's composition swapchain provides --
# the engine's Vulkan path composites opaque (see src/avatar/main.cpp). There is
# no platform backend — the engine's SDL3 backend owns the window and its
# events, so imgui_layer.cpp feeds ImGui from rend::platform::Event instead.
include(FetchContent)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8
    GIT_SHALLOW ON
    SOURCE_SUBDIR does_not_exist_no_cmake_project
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC d3d12 dxgi)
set_target_properties(imgui PROPERTIES FOLDER "third_party")
