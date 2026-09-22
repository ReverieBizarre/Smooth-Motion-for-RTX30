cmake_minimum_required(VERSION 3.20)
project(sm86_smooth C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(/utf-8 /W3 /MP /EHsc)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------- Road 1: NvPresent64 Live Test & Perf Bench
add_executable(nvp_live_test tools/nvp_live_test.cpp)
target_link_libraries(nvp_live_test PRIVATE d3d12 dxgi user32)

add_executable(nvp_perf_bench tools/nvp_perf_bench.cpp)
target_link_libraries(nvp_perf_bench PRIVATE d3d12 dxgi user32)

# ---------------------------------------------------------------- MinHook (inline hooks for DXGI and nvcuda)
add_library(minhook STATIC
  deps/minhook/src/buffer.c
  deps/minhook/src/hook.c
  deps/minhook/src/trampoline.c
  deps/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC deps/minhook/include)

# ---------------------------------------------------------------- Road 1: Proxy DLL (version.dll)
add_library(version SHARED src/proxy/sm86_rehost.cpp src/proxy/ui_mask.cpp src/proxy/osd_overlay.cpp src/proxy/d3d11_to_d3d12_bridge.cpp)
target_link_libraries(version PRIVATE minhook d3d12 d3d11 dxgi user32 gdi32 d3dcompiler version advapi32)
set_target_properties(version PROPERTIES PREFIX "")

# ---------------------------------------------------------------- Present probe (loads the built proxy into a minimal D3D12 presenter)
add_executable(nvp_present_probe tools/nvp_present_probe.cpp)
target_link_libraries(nvp_present_probe PRIVATE d3d12 dxgi user32)

# ---------------------------------------------------------------- UI Mask & OSD Test
add_executable(test_osd_uimask tools/test_osd_uimask.cpp src/proxy/ui_mask.cpp src/proxy/osd_overlay.cpp)
target_link_libraries(test_osd_uimask PRIVATE d3d12 d3d11 dxgi user32 gdi32 d3dcompiler)

add_executable(test_d3d11_osd tools/test_d3d11_osd.cpp src/proxy/osd_overlay.cpp)
target_link_libraries(test_d3d11_osd PRIVATE d3d11 d3d12 dxgi user32 gdi32 d3dcompiler)

# ---------------------------------------------------------------- D3D11 to D3D12 Bridge Test
add_executable(test_d3d11_bridge_nvp tools/test_d3d11_bridge_nvp.cpp src/proxy/d3d11_to_d3d12_bridge.cpp)
target_link_libraries(test_d3d11_bridge_nvp PRIVATE d3d11 d3d12 dxgi user32)


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

# ---------------------------------------------------------------- Proxy Runtime Hardening Test (M1)
add_executable(test_proxy_hardening tests/test_proxy_hardening.cpp)
target_link_libraries(test_proxy_hardening PRIVATE d3d11 dxgi user32)

# ---------------------------------------------------------------- Challenger Stress Test Harness (M1)
add_executable(test_challenger_stress tests/test_challenger_stress.cpp)
target_link_libraries(test_challenger_stress PRIVATE d3d11 dxgi user32)

# ---------------------------------------------------------------- Challenger 1 R1 Stress Test Harness (M1)
add_executable(test_challenger_r1 tests/test_challenger_r1_probing.cpp)
target_link_libraries(test_challenger_r1 PRIVATE d3d11 dxgi user32)


