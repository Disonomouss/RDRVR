# Third-party notices

RDRVR's binaries are linked statically with these libraries (through vcpkg). Their licences apply to those parts.

| Library | Licence | Source |
|---|---|---|
| OpenXR SDK (loader and headers) | Apache-2.0 | https://github.com/KhronosGroup/OpenXR-SDK |
| SafetyHook | BSL-1.0 | https://github.com/cursey/safetyhook |
| Zydis / Zycore (SafetyHook's disassembler) | MIT | https://github.com/zyantific/zydis |
| Dear ImGui | MIT | https://github.com/ocornut/imgui |

The headset's sharpening ([XR] Sharpness) is AMD FidelityFX CAS's no-scaling filter (ffx_cas.h; MIT, Copyright (c)
Advanced Micro Devices, Inc.; https://github.com/GPUOpen-Effects/FidelityFX-CAS), rewritten as HLSL in xr_blit.cpp.

RDRVR.Gameplay.red links against RedHook's import library (RedHook SDK by K3rhos, MIT,
https://github.com/K3rhos/RedHookSDK). RedHook itself is not part of RDRVR and is not distributed with it.

The shader converter (src/vi_convert, RDRVR_Converterdrvr_vi_convert.exe; single-pass stereo, in the release zip with
the DXC DLL beside it):
- Its rewrite (vi_rewrite.cpp) ports the rules of viewid_patch.py from optimumbox's fork of RDRVR
  (https://github.com/optimumbox/RDRVR, single-pass-stereo branch). MIT License, Copyright (c) 2026 Disonomouss;
  optimumbox's changes under the same licence. The MIT notice is RDRVR's own (LICENSE).
- It loads the DirectX Shader Compiler (dxcompiler.dll, v1.9.2609, https://github.com/microsoft/DirectXShaderCompiler)
  at run time, as rdrvr_dxcompiler.dll; nothing of it is linked in. University of Illinois/NCSA Open Source License
  (the LLVM Release License), Copyright (c) 2003-2015 University of Illinois at Urbana-Champaign, all rights reserved,
  developed by the LLVM Team, University of Illinois at Urbana-Champaign (http://llvm.org); and MIT License, Copyright
  (c) Microsoft Corporation. When the DLL is distributed, its LICENSE-LLVM.txt and LICENCE-MIT.txt go with it, as
  their terms require. Its dxil.dll (Microsoft's own licence) is not used.

Red Dead Redemption is a trademark of Take-Two Interactive Software. RDRVR is an unofficial fan project, not affiliated
with or endorsed by Rockstar Games or Take-Two Interactive. It contains no game files.
