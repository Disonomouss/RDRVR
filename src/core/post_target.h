#pragma once
// The post chain's output target, which the eye captures read: the Post FXAA Target (PostFx+0x898) under FXAA
// (technique 1); FXAATarget (PostFx+0x880) under every other technique, where there is no FXAA pass and the tonemap's
// target is the output (ENGINE-NOTES item 7, research\native-taa-study.md).

#include <cstddef>

namespace rdrvr::post_target {

inline bool fxaa(const char* postfx) { return *reinterpret_cast<const int*>(postfx + 0x868) == 1; }
inline size_t field(const char* postfx) { return fxaa(postfx) ? 0x898 : 0x880; }
inline const char* name(const char* postfx) { return fxaa(postfx) ? "Post FXAA Target" : "FXAATarget"; }

}  // namespace rdrvr::post_target
