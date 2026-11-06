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
    src/core/render_res.cpp
    src/core/lum_check.cpp
    src/core/burst_grab.cpp
    src/core/align_grab.cpp
    src/core/taa.cpp
    src/core/dlss.cpp
    src/core/exit_guard.cpp
    src/core/frame_grab.cpp
    src/core/gpu_timer.cpp
    src/core/gpu_stages.cpp
    src/core/eye2_collector.cpp
    src/core/eye2_light.cpp
    src/core/eye2_images.cpp
    src/core/main_instanced.cpp
    src/core/main_region.cpp
    src/core/eye2_velocity.cpp
    src/core/pass2_skip.cpp
    src/core/fcensus.cpp
    src/core/frame_probe.cpp
    src/core/cut_log.cpp
    src/core/playback.cpp
    src/core/playback_head.asm
    src/core/render_settings.cpp
    src/core/ring_probe.cpp
    src/core/diag.cpp
    src/core/image_mem.cpp
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
    src/core/launcher_start.cpp
    src/core/zone_rings.cpp
    src/core/round_draw.cpp
    src/core/held_prop.cpp
    src/core/aim.cpp
    src/core/scope.cpp
    src/core/reload.cpp
    src/core/gestures.cpp
    src/core/whistle.cpp
    src/core/hogtie.cpp
    src/core/bgfocus.cpp
    src/core/wheel.cpp
    src/core/gun_melee.cpp
    src/core/offhand.cpp
    src/core/pgo.cpp
    src/core/probe.cpp
    src/core/dual.cpp
    src/core/deadeye.cpp
    src/core/deadeye_thunks.asm
    src/core/actions.cpp
    src/core/physics.cpp
    src/core/audio.cpp
    src/core/menu.cpp
    src/core/pose.cpp
    src/core/test_channel.cpp
    src/core/pso.cpp
    src/core/vi_auto.cpp
    src/core/prepass_arrays.cpp
    src/core/rec_state.cpp
    src/core/rec_skip.asm
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

# (run 29 item 2) [Holsters] InteractionHighlights=parts: the glow twins' pixel shaders (src/core/shaders/part_glow.hlsl),
# compiled at build time to DXIL by the Windows SDK's dxc.exe, signed by the dxil.dll beside it (D3D12 refuses unsigned DXIL),
# into build/generated/part_glow_weapon.h and part_glow_gold.h (pso.cpp embeds them). A dxc without its dxil.dll, or none:
# a warning, RDRVR_HAVE_PART_GLOW=0 and the parts mode makes no twins (its gun places keep their sprites).
find_program(RDRVR_DXC dxc
    HINTS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" "$ENV{WindowsSdkVerBinPath}/x64"
    DOC "The Windows SDK's dxc.exe (with dxil.dll beside it), for the part glow's pixel shaders")
set(RDRVR_HAVE_PART_GLOW 0)
if(RDRVR_DXC)
    get_filename_component(RDRVR_DXC_DIR "${RDRVR_DXC}" DIRECTORY)
    if(EXISTS "${RDRVR_DXC_DIR}/dxil.dll")
        set(RDRVR_HAVE_PART_GLOW 1)
    else()
        message(WARNING "${RDRVR_DXC} has no dxil.dll beside it (its DXIL would be unsigned): the part glow is not built")
    endif()
else()
    message(WARNING "dxc.exe not found (the Windows SDK 10.0.26100's): the part glow ([Holsters] InteractionHighlights=parts) is not built")
endif()
if(RDRVR_HAVE_PART_GLOW)
    set(RDRVR_GEN "${CMAKE_BINARY_DIR}/generated")
    set(RDRVR_GLOW_HLSL "${CMAKE_SOURCE_DIR}/src/core/shaders/part_glow.hlsl")
    file(MAKE_DIRECTORY "${RDRVR_GEN}")
    add_custom_command(OUTPUT "${RDRVR_GEN}/part_glow_weapon.h"
        COMMAND "${RDRVR_DXC}" -nologo -T ps_6_0 -E PSPartGlow -Vn kPartGlowWeapon -Fh "${RDRVR_GEN}/part_glow_weapon.h" "${RDRVR_GLOW_HLSL}"
        DEPENDS "${RDRVR_GLOW_HLSL}" COMMENT "dxc: the part glow's pixel shader (PSWeapon's interface)" VERBATIM)
    add_custom_command(OUTPUT "${RDRVR_GEN}/part_glow_gold.h"
        COMMAND "${RDRVR_DXC}" -nologo -T ps_6_0 -E PSPartGlowGold -Vn kPartGlowGold -Fh "${RDRVR_GEN}/part_glow_gold.h" "${RDRVR_GLOW_HLSL}"
        DEPENDS "${RDRVR_GLOW_HLSL}" COMMENT "dxc: the part glow's pixel shader (PSWeaponGold's interface)" VERBATIM)
    target_sources(rdrvr_core PRIVATE "${RDRVR_GEN}/part_glow_weapon.h" "${RDRVR_GEN}/part_glow_gold.h")
    target_include_directories(rdrvr_core PRIVATE "${RDRVR_GEN}")
endif()
target_compile_definitions(rdrvr_core PRIVATE "RDRVR_HAVE_PART_GLOW=${RDRVR_HAVE_PART_GLOW}")

# (run 19 item 4) the converter's rule hashed at configure (any edit of these files reconfigures): the mod's cache
# folder and the converter's --version carry it, so a changed rule never trusts twins an older one made
set(RDRVR_VI_SOURCES src/vi_convert/main.cpp src/vi_convert/vi_rewrite.cpp src/vi_convert/vi_rewrite.h src/vi_convert/protocol.h)
set(RDRVR_VI_TEXT "")
foreach(f IN LISTS RDRVR_VI_SOURCES)
    file(READ "${CMAKE_SOURCE_DIR}/${f}" RDRVR_VI_ONE HEX)
    string(APPEND RDRVR_VI_TEXT "${RDRVR_VI_ONE}")
endforeach()
string(SHA256 RDRVR_VI_RULE_HASH "${RDRVR_VI_TEXT}")
string(SUBSTRING "${RDRVR_VI_RULE_HASH}" 0 12 RDRVR_VI_RULE_HASH)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RDRVR_VI_SOURCES})
target_compile_definitions(rdrvr_core PRIVATE "RDRVR_VI_RULE_HASH=\"${RDRVR_VI_RULE_HASH}\"")

# rdrvr_vi_convert.exe (single-pass stereo step C, run 19 item 4; test only, [Stereo] InstancedShadersAuto): the shader
# converter the mod runs as a child process (research/sps/step-b.md, fork-port-plan.md §5). It loads the DirectX Shader
# Compiler (Microsoft's signed release dxcompiler.dll, 1.9.2609.5; LLVM/NCSA and MIT) at run time as rdrvr_dxcompiler.dll,
# by full path; nothing of DXC is linked in. The header is the Windows SDK's dxcapi.h: an SDK without it (before
# 10.0.20348) fails the check below and the converter is simply not built (the mod then converts nothing).
include(CheckIncludeFiles)
check_include_files("windows.h;dxcapi.h" RDRVR_HAVE_DXCAPI_H LANGUAGE CXX)  # (dxcapi.h needs windows.h first)
if(RDRVR_HAVE_DXCAPI_H)
    add_executable(rdrvr_vi_convert src/vi_convert/main.cpp src/vi_convert/vi_rewrite.cpp)
    target_compile_features(rdrvr_vi_convert PRIVATE cxx_std_17)
    target_compile_definitions(rdrvr_vi_convert PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS "RDRVR_VI_RULE_HASH=\"${RDRVR_VI_RULE_HASH}\"")
    target_compile_options(rdrvr_vi_convert PRIVATE /W3 /EHsc /utf-8 /Zi)
    target_link_options(rdrvr_vi_convert PRIVATE /DEBUG "/PDBALTPATH:%_PDB%")  # (the PDB's name only, not the builder's path)
    target_link_libraries(rdrvr_vi_convert PRIVATE ole32)
    # For tests only: the DXC DLL copied next to the exe in the build folder under the name the converter loads. The
    # installer does not take it yet (research/run19/stepc.md, "Installing"). The public tree's CMake has no default.
    set(RDRVR_DXC_DLL "" CACHE FILEPATH
        "dxcompiler.dll (DXC 1.9.2609, Microsoft's release), copied as rdrvr_dxcompiler.dll next to rdrvr_vi_convert.exe")
    if(EXISTS "${RDRVR_DXC_DLL}")
        add_custom_command(TARGET rdrvr_vi_convert POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RDRVR_DXC_DLL}" "$<TARGET_FILE_DIR:rdrvr_vi_convert>/rdrvr_dxcompiler.dll")
    endif()
else()
    message(STATUS "dxcapi.h not found (Windows SDK 10.0.20348 or later has it): rdrvr_vi_convert.exe is not built")
endif()

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
