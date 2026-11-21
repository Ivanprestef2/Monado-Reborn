# Local changes to the inherited runtime

LASRecomp attribution is kept in LICENSE/NOTICE. This continuation does not incorporate code from GPL projects or
game data.

## 2026-11-04 — T247–T255

- T247: `runtime/host/settings_ini.c`, `runtime/hle/bindings.c`; shared writer in `runtime/host/ini_writer.h` and `atomic_file.h`.
- T248: `runtime/settings.h`, `runtime/nvn/vulkan/device.c`, `runtime/ui/menu_vk.cpp`; policy/probe/selector shared with the launcher.
- T249: `runtime/hle/framepace.c`, `runtime/hle/party.c`, `runtime/frame_clock.h`; millisecond paths in `runtime/diag/diag.c`/`.h`.
- T250: `runtime/nvn/vulkan/vk.h`, `dispatch.h`, `device.c`, `imgui_glue.cpp`, `shaders.c`, `dlss.cpp`, `temporal.c`, `temporal_fg.inc`.
- T251: `runtime/nvn/output_scale.c`, `runtime/nvn/vulkan/exec.c`, `precompile.c`, `runtime/resource_policy.h`.
- T252–T254: `runtime/nvn/vulkan/exec.c`, `device.c`, `textures.c`, `shaders.c`, `runtime/cache_validation.h`, `staging_copy.h`; file diagnostics in `runtime/services/fs.c`.
- T255: `runtime/nvn/objects.c`, `nvn.h`, `runtime/nvn/vulkan/exec.c`, `runtime/window_reuse.h`: per-window-image reuse wait.

Tests and limits: `docs/history/port/PORT_VALIDATION.md`. The game-timer-specific changes keep the existing hooks and
patches of `games/xcde`; no addresses were invented and no game code was generated.

## T256–T258 — Vulkan paths, DLSS and FPS HUD

- `runtime/settings.h`, `host/settings_ini.c`, `host/settings_boot.c`, `gpu_policy.h`, `vulkan_policy.h`, `nvn/vulkan/gpu_probe.h`, `device.c`, `vk.h`: API path preference, capabilities and automatic upscaler.
- `runtime/nvn/vulkan/dlss.cpp`, `exec.c`, `temporal.c`, `runtime/dlss_policy.h`, `host/nvidia_signature.h`: official NVIDIA runtime, per-GPU/SDK gating, queues, surface hook, resources/state and signature verification.
- `runtime/ui/menu_vk.cpp`, `menu.h`, `gpu_picker.h`, `runtime/fps_meter.h`: on-screen counter and options. (the earlier host menu stub was removed in the 2026-11 cleanup; `runtime/ui/menu_vk.cpp` owns the overlay.) The launcher shares the options and preserves the new keys.

Details and tests in `docs/VULKAN_DLSS_FPS.md`. The NVIDIA binaries live only in `sdk/` and `build/`, excluded from
version control.

## T259–T261 — conservative performance, profiles and interface

- `runtime/settings.h`, `quality_policy.h`, `host/settings_ini.c`, `services/romfs_virtual.c`: profiles, compatible keys, C/C++ bindings and optional in-memory effects.
- `runtime/nvn/output_scale.c`, `nvn/vulkan/exec.c`, `device.c`, `textures.c`, `vk.h`: presentation budget, asynchronous GPU time, descriptor cache, index scratch, texture index and GPU properties.
- New helpers `render_policy.h`, `descriptor_cache.h`, `native_intervals.h`, `services/romfs_fd_cache.h`, `nvn/vulkan/texture_index.h`; they share logic with the tests.
- `runtime/ui/menu_vk.cpp`, new UI helpers, `tools/launcher/launcher.cpp`: theme, navigation and shared configuration. `scripts/native_tests.sh` and `tools/diag/validate_port.py`: checks without game data.

Justification in ADR 0011. Attribution is kept and no code from other projects is incorporated. No change has been
sent upstream nor has a distribution been published.

## T271 — HUD size (GitHub #121)

- `runtime/nvn/vulkan/exec.c`: the 16:9 box classification also runs in 16:9 with the HUD scaled; per-draw anchoring and scaling folded into the viewport mapping. `runtime/settings.h`, `host/settings_ini.c`, `ui/menu_vk.cpp`, `tools/launcher/launcher.cpp`: `ui_scale` setting. New helper `runtime/ui_scale.h` (shared with `tests/native/test_ui_scale.c`). T271b: only the edge HUD is scaled (popups, menus, dialogs and markers stay as the game draws them), stable groups across frames (hysteresis anchors) and `ui_backdrop_view` in `exec.c` (shifted copy of the window texture for the scaled HUD draws that read the scene).

## Vertex and effect corruption with async on Windows (2026-11-10)

Two distinct failures of the Windows asynchronous renderer produced the same class of symptom: geometry and effects
drawn with another frame's vertex data. Both are inert on Linux, where the vertex cache and frame lag are not active
(RADV uses the dma-buf GTT path and `pt_init()` returns false without `MEM_WRITE_WATCH`).

- **T-VCACHE-ASYNC:** `vcache_lookup` (`nvn/vulkan/exec.c`) compared eviction eligibility against
  `vk_pending_seq()`. With the asynchronous worker, frame N's entries looked evictable before their sequence had been
  submitted, and their device-local buffers could be reused or freed while the worker's command buffer still
  referenced them: wrong geometry/textures and graphical corruption. Now, with `backend_async()`,
  `vk_completed_seq()` is used; the synchronous path does not change.
- **T-FRAME-LAG-VCACHE:** with async + vertex cache + more than 60 FPS, `vc_flush` recorded the refresh as a GPU-time
  copy from guest memory; the copy could run after the game had rewritten those pages, so the effect/particle streams
  drew another frame's contents (stretched large quads). Now, while frame lag is active, the pages are copied to the
  transient ring at submit time; `XCDE_VC_STAGE=0` restores the previous behavior for an A/B. Host reads that frame
  lag still performs one frame late (SSBOs and the compute path's `gfast` copy) are not covered.

Details, symptoms, diagnostics and limits:
[summary.md](evidence/2026-11-10-vertex-corruption/summary.md).

## 2026-11-20 — T273 (GitHub #183)

- `runtime/nvn/vulkan/exec.c`: the screen fade the load screen paints over the picture (0 textures, the whole guest
  target, drawn after every other window draw) is classified as a full-window composite instead of a 16:9-boxed UI
  draw, so on an ultrawide window a skip travel no longer leaves the destination field visible at the sides while the
  box is black. `XCDE_LOAD_FADE_WIDE=0` is the A/B opt-out.

## 2026-11-21 — T224 (GitHub #181): 60+ FPS in-engine cutscenes

In-engine cutscenes (`rtmevt`) previously quantized their continuous playback clock and sequence tracks to `1/30 s`
even when running above 30 FPS (`cutscenes = same`), causing 30 Hz pose/camera stepping and uneven motion.

- `games/xcde/patches.py`: three `T224` patches bypass the 30 Hz quantization gates when cutscenes run above 30 FPS:
  `0x70004e6b9c` (main cutscene loop clock rounding before `f_70004e84c0`), `0x70004f55f8` (`f_70004f55b0`, enabling
  the camera sequence's built-in `floor`/`ceil` lerp+slerp path at `1.0x` playback speed), and `0x700050c374`
  (`f_700050c150`, per-track local time rounding).
- `runtime/hle/framepace.c`: `xcde_cutscene_unquantize()` enables the bypass only when `cutscenes = same` and target
  FPS > 30 (with `XCDE_CUTSCENE_UNQUANTIZE=0|1` for A/B diagnostics); 30 FPS and `cutscenes = 30` / `30fg` execute the
  original code paths bit-identically.
- `tools/launcher/launcher.cpp`: updated the Cutscenes option tooltip to reflect smooth FPS-matched cutscenes.

## Menu / map / load-screen backdrop corruption on dynamic-resolution resize (GitHub #172, 2026-11-21)

When dynamic internal resolution changed scale while the Main Menu, Area Map, or Load Game screen was open (sampling
a persistent captured 16:9 render target), `tex_backend` discarded the old image without copying its pixels into the
reallocated image, exposing uninitialized VRAM (decoding as tiled magenta/green/black DCC noise on RADV RDNA3).

- `runtime/nvn/vulkan/tex_resize.h`: shared pure-C11 resize preservation, color render-target zero-clear, and
  material-only temporal LOD-bias policy helpers.
- `runtime/nvn/vulkan/textures.c`, `vk.h`: `resize_backend_if_needed()` blits GPU-owned color targets across all
  shared mip levels before deferring `old.image` destruction (`XCDE_RESIZE_PRESERVE=0` opt-out) and invalidates upload
  stamps for CPU-backed scalable textures; `create_image()` zero-clears new uncompressed color render targets
  (`XCDE_CLEAR_NEW_COLOR_RT=0` opt-out, `XCDE_POISON_NEW_IMAGES=1` magenta diagnostic); `SamplerVariants` expands to
  `variant[5][XCDE_SAMPLER_BIAS_SLOTS]` keyed by quality bucket (`vk_sampler(NvnSamplerDesc *, bool apply_lod_bias)`).
- `runtime/nvn/vulkan/exec.c`, `temporal.c`, `temporal.h`: `texture_info()` enables temporal LOD bias only for
  CPU-uploaded mipmapped material textures (`XCDE_TMP_LOD_BIAS_ALL=1` restores global biasing); `tmp_lod_bias_forced()`
  tracks manual `XCDE_TMP_LOD_BIAS` overrides.
- `tests/native/test_tex_resize.c`, `scripts/native_tests.sh`, `tests/unit/test_vk_source_contracts.py`: unit and
  source-contract coverage for resize preservation, color render-target initialization, and material-only LOD bias.

## Grass distance: Ultra restored, Extreme added (GitHub #171, 2026-11-20)

The grass draw-distance presets from PR #46 scale the game's own MPF foliage selector. The 3x **Ultra** preset
covers nine times the authored area, and the first private MPF block was sized at exactly 9x the authored pool, so
in dense zones such as Gaur Plains it had no margin and corrupt patch data was drawn as exploded grass vertices.
Rather than retire the preset, the private block now supports **Extreme (4x)** as well:

- `runtime/hle/foliage_distance.h`: the pool is 24x the authored 3.5 MiB (16x for 4x coverage plus 50% headroom),
  the bitmap doubles to 512 KiB for the 2000-unit endpoint, and `XCDE_FOLIAGE_MAX_SCALE` is 4. The
  `XCDE_GRASS_DISTANCE` diagnostic override is clamped to 4x with a warning.
- `runtime/hle/foliage_pool.c`: arena slots grow from 32 MiB to 128 MiB (eight maps still fit the 1.5 GiB window).
  Each live foliage map commits about 85 MiB of guest pages instead of 32 MiB; the 8-slot cap is unchanged.
- `runtime/grass_lod.h`: **Ultra (3x)** returns and **Extreme (4x)** is added; a saved index 5 or `extreme` from
  the retired 6x preset now loads as 4x.
- F1 and the launcher list both presets; `tests/native/test_settings_roundtrip.c` and
  `tests/native/test_foliage_distance.c` cover the new scales, the 2000-unit bitmap worst case and the clamped
  diagnostic override.

The 3x/4x presets still need in-game verification at the Gaur Plains spot that produced #171.

Diagnostics and reporter context: [summary.md](evidence/2026-11-10-vertex-corruption/summary.md).
