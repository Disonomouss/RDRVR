cmake_minimum_required(VERSION 3.20)
project(RDRVR LANGUAGES CXX ASM_MASM)

# Two binaries (DESIGN.md §3.3, §5.1; DECISIONS.md D1):
#   dinput8.dll         the core: loaded early through the game's (hidden) DINPUT8 import; owns D3D12,
#                       OpenXR, the renderer hooks, instruments and the test channel.
#   RDRVR.Gameplay.red  a RedHook plugin: script-fiber work that needs natives. Talks to the core
#                       through the C API in src/common/rdrvr_api.h.

# Static CRT: no dependency on the player's VC++ runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(OpenXR CONFIG REQUIRED)
find_package(safetyhook CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

set(RDRVR_DEFS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set(RDRVR_OPTS /W4 /permissive- /utf-8 /Zi)

# Fails the build when anchors.txt changed without regenerating anchors.inc.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/research/RDR.exe")
    add_custom_target(anchors_check ALL
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen_anchors.py" --check
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Checking src/core/anchors.inc against anchors.txt and research/RDR.exe")
endif()

add_library(rdrvr_core SHARED
    src/core/dllmain.cpp
    src/core/proxy_dinput8.cpp
    src/core/log.cpp
    src/core/config.cpp
    src/core/anchors.cpp
    src/core/build_check.cpp
    src/core/bootstrap.cpp
    src/core/api.cpp
    src/core/hooks.cpp
    src/core/d3d_hooks.cpp
    src/core/camera_lever.cpp
    src/core/dual_pass.cpp
    src/core/draw_masks.asm
    src/core/exec_gate_thunk.asm
    src/core/eye_grab.cpp
    src/core/eye_shape.cpp
    src/core/lum_check.cpp
    src/core/burst_grab.cpp
    src/core/taa.cpp
    src/core/dlss.cpp
    src/core/exit_guard.cpp
    src/core/frame_grab.cpp
    src/core/gpu_timer.cpp
    src/core/render_settings.cpp
    src/core/ring_probe.cpp
    src/core/diag.cpp
    src/core/game_hooks.cpp
    src/core/xinput.cpp
    src/core/xr.cpp
    src/core/xr_blit.cpp
    src/core/ui_layer.cpp
    src/core/vr_mode.cpp
    src/core/hands.cpp
    src/core/controllers.cpp
    src/core/controls.cpp
    src/core/body.cpp
    src/core/holster.cpp
    src/core/zone_rings.cpp
    src/core/round_draw.cpp
    src/core/held_prop.cpp
    src/core/aim.cpp
    src/core/reload.cpp
    src/core/gestures.cpp
    src/core/gun_melee.cpp
    src/core/dual.cpp
    src/core/actions.cpp
    src/core/physics.cpp
    src/core/audio.cpp
    src/core/menu.cpp
    src/core/pose.cpp
    src/core/test_channel.cpp
    src/core/pso.cpp
    src/core/dinput8.def
)
set_target_properties(rdrvr_core PROPERTIES OUTPUT_NAME dinput8)
target_compile_features(rdrvr_core PRIVATE cxx_std_23)
target_compile_definitions(rdrvr_core PRIVATE ${RDRVR_DEFS})
target_compile_options(rdrvr_core PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${RDRVR_OPTS}>")
target_include_directories(rdrvr_core PRIVATE src)
target_link_options(rdrvr_core PRIVATE /DEBUG /OPT:REF /OPT:ICF "/PDBALTPATH:%_PDB%")
target_link_libraries(rdrvr_core PRIVATE
    safetyhook::safetyhook OpenXR::openxr_loader OpenXR::headers imgui::imgui
    d3d12 dxgi dxguid bcrypt version shlwapi ole32)

# The gameplay plugin needs the RedHook SDK (K3rhos/RedHookSDK, MIT), which this repository does not include: put its
# headers in <sdk>/include and RedHook.lib in <sdk>/lib (README, "Building"). Without it only dinput8.dll is built.
set(REDHOOK_SDK_DIR "${CMAKE_SOURCE_DIR}/extern/redhook-sdk" CACHE PATH "RedHook SDK: include/RedHook.h and lib/RedHook.lib")
if(NOT EXISTS "${REDHOOK_SDK_DIR}/include/RedHook.h" OR NOT EXISTS "${REDHOOK_SDK_DIR}/lib/RedHook.lib")
    message(WARNING "RedHook SDK not found at ${REDHOOK_SDK_DIR}: RDRVR.Gameplay.red is not built (see README)")
    return()
endif()

add_library(rdrvr_gameplay SHARED
    src/gameplay/main.cpp
)
set_target_properties(rdrvr_gameplay PROPERTIES OUTPUT_NAME RDRVR.Gameplay SUFFIX .red)
target_compile_features(rdrvr_gameplay PRIVATE cxx_std_20)
target_compile_definitions(rdrvr_gameplay PRIVATE ${RDRVR_DEFS})
target_compile_options(rdrvr_gameplay PRIVATE ${RDRVR_OPTS} /wd4100 /wd4244 /wd4505 /wd4828)
target_include_directories(rdrvr_gameplay PRIVATE src "${REDHOOK_SDK_DIR}/include")
target_link_options(rdrvr_gameplay PRIVATE /DEBUG /OPT:REF /OPT:ICF "/PDBALTPATH:%_PDB%")
target_link_libraries(rdrvr_gameplay PRIVATE "${REDHOOK_SDK_DIR}/lib/RedHook.lib" psapi)
