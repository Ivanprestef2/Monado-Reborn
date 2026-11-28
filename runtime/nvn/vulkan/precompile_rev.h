// [XCDE] T282: revision of the code that decides WHAT the precompile builds, for the parts that shader_sig.py cannot
// hash without invalidating on every gameplay change.
//
// tools/build/shader_sig.py fingerprints the translator (runtime/nvn/maxwell/), the GLSL-to-SPIR-V compiler
// (shaders.c/.h, vk.h) and the precompile's own key guesses (precompile.c). The rest of the build path - the fixed
// pipeline state, the vertex/fragment library keys and the whole-pipeline creation used by vk_precompile_program and
// vk_precompile_variant - lives in runtime/nvn/vulkan/exec.c, which changes for gameplay and renderer work too, so
// hashing it would re-precompile the shaders after most rebuilds (the thing T282 removes).
//
// When you change what those functions build (a new library variant, a different pipeline state, another cached
// object), bump XCDE_PRECOMPILE_ARTIFACTS_REV: precompile_sig.c mixes it into the run signature, so the next launch
// considers the cache stale and warms it again. Cosmetic refactors do not need a bump.
#pragma once

// 2: [XCDE] GTAO replacement shaders now participate in compile_program / the precompiled SPIR-V set.
// 3: [XCDE] GitHub #58 / #268: char_light_src anchors on the epilogue instruction so structured CFG applies it.
// 4: Attachment-less rasterizer-discard variants build pre-rasterization-only pipelines.
// 5: Use the 32-bit guest-heap addressing path in vertex shaders on discrete RADV devices.
#define XCDE_PRECOMPILE_ARTIFACTS_REV 5u
