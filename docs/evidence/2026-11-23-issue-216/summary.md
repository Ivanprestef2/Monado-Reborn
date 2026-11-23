# Issue #216 — "Missing Particles on Monado": triage (2026-11-23)

Report: [Ivanprestef2/Monado-Reborn#216](https://github.com/Ivanprestef2/Monado-Reborn/issues/216) (SnowVXIII,
2026-11-22). Build `v17.0.1` (launcher update check in the attached ZIP), RTX 3060 Ti / driver 595.79 / Windows,
3440x1440 ultrawide, game image **2024x848 → upscaled 3440x1440** (`upscaler = dlss`, balanced, `antialias = smaa`,
`frame_gen = 1` i.e. FSR 3 frame generation too), `fps = 60`, `cutscenes = same`, `effects_resolution = original`,
`occlusion = auto`.

The report carries two **independent** symptoms, plus a ZIP with `last_game.log`, `last_game.prev.log`, one minidump
and the INI files. The video link (gofile) is not downloadable anonymously and the ZIP has no save, so the visual
symptom cannot be reproduced from it; the crash can be read from the logs and the minidump.

## 1. The crash is on exit, and it is a shutdown race (fixed)

Both sessions end identically: `window closed`, then `0xc0000005` at the same address
(`pc 0x7ffa54042cb0`, `host lr … (dlss_evaluate)`). It is not "after the battle": the earlier session is only
3 minutes long, the crash follows the window close in both.

- Parsed minidump: the faulting RIP is inside **no loaded module** (the hole between `nvspcap64.dll` and
  `libopus-0.dll`), so code that had just been unloaded had been executed. The crashing thread's stack points back
  into `dlss_evaluate` and below it into `nvoglv64.dll` (the NVIDIA ICD). The guest dump shows the game's threads
  still running (`nnMain` "running"; 41 threads, 40 blocked).
- Cause: `exit(0)` runs the `atexit` handlers in reverse order. `quick_exit_handler` (`device.c`, registered first in
  `ui_window_init`) ran last; `dlss_init_early()` registered `atexit(dlss_shutdown)` after it, so `slShutdown()` ran
  first — unloading `sl.*`/NGX while the render thread was inside `dlss_evaluate`. The next Streamline call executed
  the unloaded plugin.
- Fix (in `dlss.cpp`): the SDK shutdown functions stay but are no longer registered; the window-close path ends the
  process through `quick_exit_handler` + `plat_terminate()` (`TerminateProcess` / `_exit`), which skips the DLL
  teardown. Rationale and verification status: [CHANGELOG](../../dev/CHANGELOG.md).
- Regression guard: `tests/unit/test_vk_source_contracts.py`
  (`test_vendor_upscaler_sdks_are_not_unloaded_by_an_exit_handler`).

## 2. The Monado's ether/particles are missing (open)

Both attached images are cutscene frames of the Colony 9 attack (the climb and the Monado on the ground): the sword's
red frame, blue trim and ring render, but the **ether blade/glow layer and its particles are absent** (the ring's inner
area is black). The images cannot say whether other particle layers (dust, smoke, arts FX) are missing too, whether
the effect ever appeared in that session, or which draw is at fault — the ZIP has no save and no video.

Why the upscaler alone is an unlikely cause: DLSS is fed the game's own image (`img[0]` is a copy of the presented
target, or of the pre-HUD snapshot), and `tmp_hud.frag` re-adds anything drawn after the snapshot, so the temporal
path does not delete an entire effect layer. What is cutscene- or configuration-specific is where to look first:

| # | Hypothesis | Cheap A/B (no rebuild unless noted) | Expected if true |
|---|---|---|---|
| 1 | The #181 cutscene unquantize (v17.0.1, the day before) changes rtmevt track timing; an effect track's spawn is missed. **Checked and weakened** (see below): the patch only replaces the 1/30 s rounding of a track's local time | `cutscenes = 30` in the launcher, or `XCDE_CUTSCENE_UNQUANTIZE=0` | FX appears with the A/B |
| 2 | The game's own occlusion culling drops the effect emitter (documented class: #14 "floor disappears and comes back", T139) | `occlusion = off` (or `XCDE_OCCLUSION=0`) | FX appears with culling off |
| 3 | Ultrawide handling: `wide box remap` + `uvb()` sample remap for the 21:9 window | run a 16:9 window (e.g. 2560x1440) | FX appears at 16:9 |
| 4 | Vertex mirrors for GPU-written effect streams collapse a tail (reads go to the heap / clamp to the mirror end) | `XCDE_XFB_SHADOW=0`, `XCDE_STORE_SHADOW=0` | FX appears with mirrors off |
| 5 | DLSS/FSR 3 frame generation path (reactive mask / history) | `frame_gen = 0`; `upscaler = fsr1`; `XCDE_TMP_OFF=1`; `XCDE_TMP_NO_HUD_MASK=1` | FX appears without the temporal path |

Prior evidence that this layer can be wrong outside cutscenes: T180 (2026-10-20, still open) records the user's
"Shulk's sword looks flat / see-through" in a field shot, i.e. the sword's effect layer has been reported before and is
not specific to a cutscene.

**Checked: the #181 unquantize does not skip trigger logic.** The lifted, patched code of the third gate
(`generated/main/main_132.c`, function `f_700050c150`, site `0x700050c374`) shows that the bypass jumps over exactly
the snap-to-30 Hz block: `s1 = t / (1/30)`; `frame = int(s1 ± 0.5)`; `t = float(frame) * (1/30)`. The patch hands the
track evaluator the raw local time instead of the rounded one — no trigger, interval or keyframe test is skipped at
the gate. An effect track could only be lost if something downstream compares that time for exact equality with a
quantized keyframe; that was not traced further, but it is not what the patch itself changes. The row stays in the
table as the cheapest A/B, but it is no longer the leading explanation.

The log shows the mirror machinery active right after the cutscene starts (`[vk] mirror … shorter than a read …`,
`… retired (overlapped by …)`, `transform feedback mirror … in device memory`), so hypothesis 4 has a concrete trace
in the report, but nothing yet proves that those ranges feed the Monado's effect.

What a usable report needs: the save + the step to the cutscene, whether **all** particle layers or only the Monado's
are missing, and one A/B result from the table above.

### Update — the reporter's A/Bs (2026-11-22 19:35–19:40)

Two more reports, both loading the same save (`save_0/bfsgame00.sav`, ~13 s in) and both ending cleanly (`window
closed`, no `[crash]` line):

| Report | Upscaler | Temporal path | `[post]` line | Crash on exit | Monado FX |
|---|---|---|---|---|---|
| 19:35 (`xcde_report_20261122_193755`) | `fsr3`, native 1:1 + SMAA | FSR 3 SR + frame generation | `3440x1440 -> upscaled smaa 3440x1440` | **no** | missing |
| 19:39 (`xcde_report_20261122_194043`) | `bilinear` | none (no `[tmp]` lines at all) | `3440x1440 -> smaa 3440x1440` | **no** | missing |

Consequences:

- **The presentation stack is ruled out** (rows 1 and 5 of the table): the FX is missing with DLSS, with FSR 3 SR and
  frame generation, and with the plain spatial path (no jitter, no HUD snapshot, no DLSS/FSR, no frame generation).
  That matches the reporter's earlier "FG ON/OFF doesn't change anything".
- **The exit crash is DLSS-specific, which is the control the fix needed**: these sessions take the same exit path but
  never call `dlss_init_early()` (`[dlss] DLSS no solicitado`), so no `atexit(dlss_shutdown)` was registered and they
  exit cleanly. The crash tracks the Streamline shutdown that used to be registered — see section 1.
- Neither session contains an in-engine cutscene (`[cutscene]` is never logged), so the "same missing particles" was
  judged in gameplay on that save. That agrees with T180 ("Shulk's sword looks flat / see-through", field, open): the
  layer is wrong beyond cutscenes.
- Remaining checks, in order: `occlusion = off` (the game's own emitter culling), a 16:9 window (the ultrawide
  `wide box` / `uvb()` path), `XCDE_XFB_SHADOW=0` / `XCDE_STORE_SHADOW=0` (vertex mirrors), and if those are clean the
  per-effect class — the FX asset's texture decode, program or render state. For that last one the reporter's save
  slot 0 (`bfsgame*.sav`, 1.4 MB) is the fastest route to a local reproduction.

## 3. Streamline integration health (found in the log, not fixed)

The same ZIP shows the DLSS integration misbehaving independently of the crash (all Windows-only, needs a Windows
run to verify):

- `425,692` `streamline][error] vulkan.cpp:3077[isFormatSupported] Cannot have undefined format` lines per
  10-minute session (≈12 per frame; 64 MB of log in total) and `64` `getResourceSize … format 0 native 9`
  (`native 9` = `VK_FORMAT_R8_UNORM`, the reactive mask).
- `sl::Resource::desc` is never set, although the SDK header says the description (`VkImageCreateInfo`) is mandatory
  on Vulkan; the R8 reactive mask is tagged `kBufferTypeBiasCurrentColorHint`, not `kBufferTypeReactiveMaskHint`.
- Once per session: `Streamline presentCommon() was not observed … internal bookkeeping and garbage collection will
  not run`.

These explain the log flood and a resource-tracking leak candidate; they are not known to cause the missing FX.
Tracked as `T-EXIT-DLSS-2` in [tasks.md](../../dev/tasks.md).

## 4. Root cause of the missing FX: the #181 cutscene unquantize (found 2026-11-23)

Reproduced locally on Linux with the reporter's save (second entry, the manual save at the grass field), walking forward
into the cutscene with the maintainer's settings applied through `XCDE_INI` (`upscaler = fsr3`, native AA, `fps = 60`,
`occlusion = auto`, `taa = 0`, `game_blur = 0`, real time):

- With the patch active (the default at more than 30 FPS, `cutscenes = same`): the Monado's ring lens and blade
  emission are absent — at "Another new symbol appeared on the Monado!" the ring renders as flat blue trim with the
  background visible through it. This matches the reporter's stills and the maintainer's own capture.
- `XCDE_CUTSCENE_UNQUANTIZE=0` with everything else unchanged: the ring lens glows with its runes, the blade emits and
  blooms — the look of the walkthrough reference.
- Deterministic baseline (`fps = 30`, patch inactive): glow present. `occlusion = on` under the deterministic run:
  glow still present, so occlusion culling is not involved.
- Binary-patch A/B on a copy of the v17.0.1 build (no rebuild): replacing the call at the **third** patch site
  (`0x700050c374`, the snap-track time rounding, in `f_700050c150`) with "return false" while leaving the other two
  sites unquantized brings the glow back. Gate 3 (`_cutscene_track_unquantize`) is therefore the regression.

Mechanism: tracks with the snap flag are authored on the exact 30 Hz grid, and the sequence evaluator matches their
key/trigger times against that grid. Feeding it continuous time (gate 3 bypassed) misses those exact times, so the
cutscene track that turns the Monado's glow on is never triggered. The other two gates (cutscene clock, camera
interpolation) do not affect it. This also explains the reporter's results across upscalers: the gate depends on the
frame rate, not on DLSS/FSR 3/bilinear or frame generation.

- Workaround for players: launcher "Cutscenes: 30 fps" (`cutscenes = 30`) or `XCDE_CUTSCENE_UNQUANTIZE=0`.
- Fix (applied in `games/xcde/patches.py`): the third bypass is removed, so gates 1 and 2 keep the visible #181
  smoothness while snap tracks stay on the 30 Hz grid. **Pending:** regenerate the lifted code from the main checkout
  and re-verify #181 (60 FPS cutscene pose smoothness, T224's frame-pair measurement) on the regenerated build before
  shipping.
