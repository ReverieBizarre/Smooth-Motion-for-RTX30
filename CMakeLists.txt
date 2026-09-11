cmake_minimum_required(VERSION 3.20)
project(sm86_smooth CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(/utf-8 /W3 /MP /EHsc)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------- Road 2: Shader VFI core
add_library(sm86_vfi STATIC src/vfi.cpp)
target_include_directories(sm86_vfi PUBLIC src)
target_link_libraries(sm86_vfi PRIVATE d3d12 d3dcompiler dxgi)

add_executable(vfi_selftest tools/selftest.cpp)
target_link_libraries(vfi_selftest PRIVATE sm86_vfi d3d12 dxgi)

# copy shader next to binary
add_custom_command(TARGET vfi_selftest POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:vfi_selftest>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/vfi.hlsl
          $<TARGET_FILE_DIR:vfi_selftest>/shaders/vfi.hlsl)

# ---------------------------------------------------------------- Road 1: NvPresent64 Live Test & Perf Bench
add_executable(nvp_live_test tools/nvp_live_test.cpp)
target_link_libraries(nvp_live_test PRIVATE d3d12 dxgi user32)

add_executable(nvp_perf_bench tools/nvp_perf_bench.cpp)
target_link_libraries(nvp_perf_bench PRIVATE d3d12 dxgi user32)

# ---------------------------------------------------------------- Road 1: Proxy DLL (version.dll)
add_library(version SHARED src/proxy/sm86_rehost.cpp src/proxy/ui_mask.cpp src/proxy/osd_overlay.cpp)
target_link_libraries(version PRIVATE d3d12 d3d11 dxgi user32 gdi32 d3dcompiler)
set_target_properties(version PROPERTIES PREFIX "")

# ---------------------------------------------------------------- UI Mask & OSD Test
add_executable(test_osd_uimask tools/test_osd_uimask.cpp src/proxy/ui_mask.cpp src/proxy/osd_overlay.cpp)
target_link_libraries(test_osd_uimask PRIVATE d3d12 dxgi user32 gdi32 d3dcompiler)


# ---------------------------------------------------------------- Road 1 / ReShade: Add-on (sm86_smooth.addon64)
add_library(sm86_smooth_addon SHARED src/addon/sm86_addon.cpp src/proxy/ui_mask.cpp)
target_include_directories(sm86_smooth_addon PRIVATE deps/reshade/include deps/imgui src)
target_link_libraries(sm86_smooth_addon PRIVATE d3d12 dxgi user32 d3dcompiler)
set_target_properties(sm86_smooth_addon PROPERTIES OUTPUT_NAME "sm86_smooth" SUFFIX ".addon64" PREFIX "")

# ---------------------------------------------------------------- Proxy Verification Test
add_executable(proxytest tools/proxytest.cpp)

# ---------------------------------------------------------------- PE Pattern Scan Test
add_executable(test_pe_scan tools/test_pe_scan.cpp)
target_link_libraries(test_pe_scan PRIVATE user32)

# ---------------------------------------------------------------- ReShade Addon Simulation Test
add_executable(test_addon_simulation tools/test_addon_simulation.cpp src/proxy/ui_mask.cpp)
target_include_directories(test_addon_simulation PRIVATE deps/reshade/include deps/imgui src)
target_link_libraries(test_addon_simulation PRIVATE d3d12 dxgi user32 d3dcompiler)

