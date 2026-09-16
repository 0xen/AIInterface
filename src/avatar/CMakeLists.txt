# The avatar window: a small borderless transparent always-on-top window on
# the rend engine's D3D12 backend (the only backend whose swapchain
# composites with alpha on the AMD driver here), drawing a placeholder cube.
add_executable(avatar main.cpp)
target_link_libraries(avatar PRIVATE rend::engine rend_warnings)
target_compile_definitions(avatar PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(avatar PRIVATE /utf-8)
endif()
set_target_properties(avatar PROPERTIES FOLDER "apps")

# Our HLSL, compiled by the engine's dxc rule into data/shaders next to the
# executable (SPIR-V plus DXIL; the D3D12 backend swaps .spv for .dxil).
rend_add_shaders(avatar_shaders
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
  OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/data/shaders"
  STAGES
    "cube.hlsl|VSMain|vs_6_0|cube.vert.spv"
    "cube.hlsl|PSMain|ps_6_0|cube.frag.spv")
set_target_properties(avatar_shaders PROPERTIES FOLDER "assets")
add_dependencies(avatar avatar_shaders)
