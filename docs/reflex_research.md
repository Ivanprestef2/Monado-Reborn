# NVIDIA Reflex in Monado-Reborn: feasibility and opt-in implementation

Date: 2026-11-18. Question: can this port add **NVIDIA Reflex** (low latency) on top of the Streamline
integration it already has, and what would it take? Sections 1-3 are the feasibility analysis; section 8 records
the opt-in implementation added on 2026-11-18 (no game files changed). The experiment behind the analysis is
reproducible with `tools/reflex_probe.cpp`; the raw run is
[`docs/evidence/reflex_2026-11-18_probe.txt`](evidence/reflex_2026-11-18_probe.txt) and the summary is
[`docs/evidence/reflex_2026-11-18.md`](evidence/reflex_2026-11-18.md) (Spanish) /
[`reflex_2026-11-18.en.md`](evidence/reflex_2026-11-18.en.md) (English).

**Short answer: yes, the runtime path is proven end to end on the available PC (Vulkan extension, Streamline
feature, sleep and latency reports), and it is now implemented as an opt-in (`reflex = off` by default), but the
latency gain is situational and must still be measured with the Reflex verification HUD before it is enabled by
default.**

| Question | Answer |
|---|---|
| Does the driver support it? | Yes on the available PC: GeForce GTX 1050 Ti, driver 582.78, Vulkan 1.4.312 exposes `VK_NV_low_latency` and `VK_NV_low_latency2` |
| Does the Streamline runtime support it here? | Yes: `slIsFeatureSupported(kFeatureReflex)` returns `eOk` and `lowLatencyAvailable = 1`, with only Reflex + PCL features loaded, and also when DLSS is requested but unsupported |
| What is already in the repo? | Streamline 2.14.16 manual hooking, swapchain proxies, `NvLowLatencyVk.dll`, surface hook, signature check |
| What was missing (now implemented, opt-in) | `sl.reflex.dll` / `sl.pcl.dll` deployment, a Streamline runtime not gated on DLSS, a shared frame token, markers and sleep in the frame loop, settings/UI — all added in section 9 |
| How big is it? | Medium: the implementation is ~1 week of the ~1-1.5 weeks estimate; the Reflex API is small, the work is the frame/thread ownership around the async render worker |
| Will it reduce latency in the default 60 FPS V-Sync mode? | Unknown; NVIDIA's own matrix says classic V-Sync without VRR/FG often gains nothing, but this port queues presents up to 2 frames ahead, so there is room. Measure with the Reflex verification HUD before claiming anything |
| Why it matters on the available PC | It is a GTX 1050 Ti: DLSS cannot run there, so a `DLSS`/`Automatic` start loads Streamline (~1.1-1.3 s) and then discards it. Reflex is the NVIDIA feature this class of GPU can actually use |

## 1. What Reflex needs

Reflex Low Latency is NVIDIA-only: GeForce 900 series and newer, driver 456.38+ (Streamline 2.14.16
`ProgrammingGuideReflex.md`); it is a distinct plugin (`sl.reflex.dll`) plus the PC Latency Stats plugin
(`sl.pcl.dll`) that owns the markers. On Vulkan the integration is:

- the device extension **`VK_NV_low_latency`** (reported by `slGetFeatureRequirements` for `kFeatureReflex`;
  the plugin also knows `VK_NV_low_latency2`), no extra queues or features;
- a swapchain that Streamline can see (its proxies), like the DLSS path already does;
- one `sl::FrameToken` per frame and, every frame, **`slPCLSetMarker`** for
  `eSimulationStart/End`, `eRenderSubmitStart/End`, `ePresentStart/End`, and **`slReflexSleep`** where the
  application should wait before starting the next frame's CPU work;
- `slReflexSetOptions` at least once (even when off: `mode = eOff`), `slReflexGetState` for
  `lowLatencyAvailable` and the per-frame latency report;
- the vendor module **`NvLowLatencyVk.dll`**, which Streamline loads and signature-checks itself. It is already
  deployed today.

NVIDIA's support matrix (same guide) matters for expectations: Reflex reduces latency when the GPU is the
bottleneck, with VRR, or with frame generation; with **classic V-Sync, no VRR and no FG it may not reduce it
at all**, and it never helps a CPU-bound frame. This port's default is 60 FPS with FIFO (`xcde.ini vsync = 1`),
so the gain depends on how far ahead the CPU runs; the async render worker queues up to two presents ahead
(`exec.c` `rq_push_result`, `rq_presents >= 2`), which is exactly the kind of slack Reflex can remove, but only
measurement will tell.

## 2. What the repository already has

- `runtime/nvn/vulkan/dlss.cpp` loads Streamline in **manual hooking mode**
  (`eUseManualHooking | eDisableCLStateTracking | eUseFrameBasedResourceTagging`), verifies the Authenticode
  signature of `sl.interposer.dll`, merges `slGetFeatureRequirements`, creates the device with Streamline's
  extensions/features/queues, calls `slSetVulkanInfo`, creates the Win32 surface through Streamline's proxy and
  routes `vkCreateSwapchainKHR`, `vkAcquireNextImageKHR`, `vkQueuePresentKHR` and `vkDeviceWaitIdle` through its
  proxies (`device.c` `sc_proxies`). That is the whole plumbing Reflex needs.
- `runtime/nvn/vulkan/device.c` `ui_window_init` calls `dlss_init_early()` before creating the Vulkan instance,
  then `dlss_device_created()` after `vkCreateDevice`, then `dlss_create_surface()` and `sc_proxies()`.
- `build/` already contains `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll` and
  `NvLowLatencyVk.dll`. `sdk/sl/bin/x64` also contains `sl.reflex.dll`, `sl.pcl.dll` and the Reflex licence, but
  `cmake/copy_upscaler_dlls.cmake`, `scripts/copy_upscaler_dlls.sh` and `tools/install_dlss.py` do not deploy
  those two, so Streamline cannot load the feature today even if it were requested.

What blocks Reflex today is not a missing API but the shape of the integration:

1. **The runtime is gated on DLSS.** `dlss_init_early()` returns early unless the upscaler is `DLSS` or
   `Automatic`; `dlss_validate_adapter()` and `dlss_device_created()` clear the shared `sl_ok` flag when DLSS
   itself is unsupported (a GTX 1050 Ti, for example); `dlss_create_surface()` and `sc_proxies()` only run when
   DLSS is usable. Reflex has to survive a machine where DLSS is not.
2. **One frame token per Streamline frame.** `dlss_evaluate()` calls `slGetNewFrameToken` once per evaluation.
   If Reflex also calls it at the frame boundary, Streamline sees two frames per game frame. The token must be
   issued once and shared.
3. **The frame is split across threads.** The guest/limiter thread ends the frame, the render worker
   (`exec.c` `rq_worker`) presents it later; the present is capped at two in flight. Markers and DLSS evaluation
   must use the right frame's token even when the worker runs during the next simulation.

## 3. The experiment (2026-11-18)

`tools/reflex_probe.cpp` (not built by CMake or CI; command in its header) loads Streamline 2.14.16 production
DLLs from `sdk/sl/bin/x64` with `kFeatureReflex` + `kFeaturePCL`, creates a hidden-window Vulkan device and a
FIFO swapchain exactly through Streamline's manual-hooking proxies (same order as `device.c`: instance, device,
`slSetVulkanInfo`, proxy surface, swapchain), then runs 240 frames of markers plus `slReflexSleep`.

Raw enumeration of the installed driver (throwaway probe, same session):

```
NVIDIA GeForce GTX 1050 Ti | vendor 0x10de | Vulkan 1.4.312 | driver 582.78
VK_KHR_present_id, VK_KHR_present_wait, VK_KHR_timeline_semaphore
VK_NV_low_latency, VK_NV_low_latency2
```

Streamline-level results (full log in the evidence file):

| Check | Result |
|---|---|
| `slInit` with Reflex + PCL | `eOk`; 1.1-1.3 s (all SDK plugins in the directory are loaded and probed) |
| Reflex requirements | 0 instance ext, **1 device ext: `VK_NV_low_latency`**, 0 queues, 0 features |
| `slIsFeatureSupported(kFeatureReflex)` | `eOk` (`0`) |
| `slReflexGetState` | `lowLatencyAvailable = 1`, `latencyReportAvailable = 1`, flash driver-controlled |
| `slReflexSetOptions(eLowLatency)` | `eOk` |
| 240 frames, markers + `slReflexSleep` | 0 sleep errors, 0 marker errors |
| Latency reports | valid timestamps and `gpuFrameTimeUs` around 6900 us |
| Same with DLSS also requested (`PROBE_DLSS=1`, DLSS unsupported on this GPU) | Reflex stays available, markers/sleep keep working |

Not measured, and deliberately so: the probe's window is hidden and its FIFO swapchain is not paced by a real
display, so `slReflexSleep` averaged 0.04 ms (max 0.18 ms). That is the expected behaviour of a low-latency
implementation with nothing to wait for, **not** a latency measurement. The probe proves the API path, the
feature availability and the reports; the gain in the game has to be measured with the Reflex verification HUD /
FrameView (section 6).

## 4. How it would be integrated

### 4.1 Runtime split

Keep one Streamline runtime for DLSS and Reflex, and stop identifying it with DLSS:

- `dlss_init_early()` becomes `streamline_init_early()`: it runs when **either** DLSS is wanted (as today) **or**
  `reflex != off`; `featuresToLoad` = DLSS (if wanted) + Reflex + PCL. It tolerates a missing `sl.reflex.dll`
  (request DLSS only, report Reflex unavailable) instead of failing the whole `slInit`.
- Capability flags stop sharing one variable: `sl_rt_ok` (runtime loaded, `slSetVulkanInfo` done),
  `dlss_ok`, `reflex_ok` (`slIsFeatureSupported` + `lowLatencyAvailable`), `sl_surface_ready`.
- `dlss_validate_adapter()` and `dlss_device_created()` only invalidate DLSS; the surface and the swapchain
  proxies are created whenever `sl_rt_ok`, because Reflex needs them without DLSS.
- Requirement merging takes the union over all requested features (`VK_NV_low_latency` comes from
  `slGetFeatureRequirements`, never hardcoded).

### 4.2 Shared frame token

One token per game frame, in a small ring (Streamline allows up to 6 frames in flight, `MAX_FRAMES_IN_FLIGHT`):

- the limiter thread calls `sl_frame_token(game_frame)` at the frame boundary (it gets and stores the token);
- `nvnQueuePresentTexture` (objects.c) tags the present job with `game_frame`;
- the worker uses that frame's token for the present markers, and `dlss_evaluate()` receives it instead of
  calling `slGetNewFrameToken` itself;
- the ring size covers the two queued presents plus margin, and a token that would be recycled while the worker
  still uses it is treated as a bug (assert in debug builds, log once in release).

This is the part that deserves a spike of its own: it touches the async render worker, DLSS evaluation and the
present path at once.

### 4.3 Call placement

| Point (file / function) | Thread | Calls |
|---|---|---|
| frame boundary, `framepace.c` `xcde_limiter_spin` (once per frame, before the T157 nap) | limiter/guest | `slReflexSleep(token)` only: the game is still waiting for the next frame here |
| limiter returns, `framepace.c` `xcde_limiter_done` | limiter/guest | `slPCLSetMarker(eSimulationStart, token)`: the next simulation really starts here |
| `objects.c` `nvnQueuePresentTexture` | guest | `slPCLSetMarker(eSimulationEnd)`, `slPCLSetMarker(eRenderSubmitStart)`; record `game_frame` in the present job |
| `device.c` `vk_frame_end`, after `vk_submit_extra` and before `sc_present` | worker (or guest if sync) | `slPCLSetMarker(eRenderSubmitEnd)`, `slPCLSetMarker(ePresentStart)` |
| `device.c` `vk_frame_end`, after `sc_present` returns | worker (or guest if sync) | `slPCLSetMarker(ePresentEnd)` |
| repeat presents at 30 FPS (`repeat_presents`) | worker | no markers for the redraw of the same image |
| F1 menu / ini change | UI | `slReflexSetOptions(mode)` (live), `eOff` when disabled |
| shutdown, before device/instance destruction | main | `slShutdown()` (already done for DLSS) |

Threading note: the PCL marker API is documented thread-safe, but Streamline's `slSetConstants` (DLSS) is not.
Do not wrap `slReflexSleep` and DLSS evaluation in one global lock: the sleep waits for the driver and the
worker's present must be able to proceed. If the two threads must be serialised, serialise only the short calls
and leave the sleep outside the lock.

`sl::ReflexOptions::frameLimitUs` is not used in v1: the game has its own fixed-step limiter and replacing its
pacing with the driver's is a separate experiment.

Sketch of the call flow (not compiled; the names are the proposal, not existing code):

```c
// framepace.c, once per game frame while the limiter waits at the frame boundary
if (reflex_enabled()) {
    sl::FrameToken *t = sl_frame_token(game_frame);   // gets it once, keeps it in the ring
    reflex_sleep(t);                                  // outside any lock: the worker must keep presenting
}
// xcde_limiter_done (the limiter returned): the next simulation starts here
reflex_marker(sl_frame_token(game_frame), eSimulationStart);
// objects.c, nvnQueuePresentTexture (guest)
reflex_marker(sl_frame_token(game_frame), eSimulationEnd);
reflex_marker(sl_frame_token(game_frame), eRenderSubmitStart);
queue_present(texture, game_frame);                   // the job carries its game frame
// device.c, vk_frame_end (worker)
sl::FrameToken *t = sl_frame_token(present_job_frame);
reflex_marker(t, eRenderSubmitEnd);
reflex_marker(t, ePresentStart);
sc_present(queue, &pi);
reflex_marker(t, ePresentEnd);
```

### 4.4 Settings, UI, deployment

- `xcde.ini`: `reflex = on | off | boost` (the NVIDIA default is On; the first build ships `off` / opt-in and the
  default flips only after the validation in section 6). The setting is read/written by
  `runtime/host/settings_ini.c`, mirrored in `runtime/settings.h`, the launcher
  (`tools/launcher/launcher.cpp`, `Cfg`) and the F1 menu **Gráficos > Velocidad** next to V-Sync, following the
  NVIDIA UI guideline labels ("Reflex: Off / On / On + Boost"). The control is disabled with a tooltip when the
  selected GPU is not NVIDIA, the DLLs are missing or `reflex_available()` is false.
- `runtime/nvn/vulkan/reflex.cpp` (or the extended Streamline module) exposes
  `reflex_init/available/status/set_mode/frame_start/marker/sleep/shutdown`, with a short `[reflex]` status in
  the log (`available`, `unsupported: ...`, `sleep errors N`).
- Deployment: add `sl.reflex.dll` and `sl.pcl.dll` to `cmake/copy_upscaler_dlls.cmake`,
  `scripts/copy_upscaler_dlls.sh` and `tools/install_dlss.py` (`REQUIRED`, so a fresh install has them), and the
  launcher's DLL check. `tools/make_portable.py` already copies every `build/*.dll`, so portable packages get
  them automatically; add `reflex.license.txt` (NVIDIA SDK licence, currently in `sdk/sl/bin/x64/`) to the
  packaged licences and to the `NOTICE.md` table.
- `tools/validate_port.py` compiles `nvn/vulkan/dlss.cpp` as part of its UI build check; add the new file there
  if it is split out.

### 4.5 Tests before the feature

- `tests/native/test_settings_roundtrip.c`: the new key round-trips.
- `tests/unit/test_install_dlss.py`: it derives its cases from `REQUIRED`/`OPTIONAL`, so extend the expected
  set explicitly rather than silently accepting the new default.
- A source-contract test (the style of `tests/unit/test_vk_source_contracts.py`) for: `VK_NV_low_latency` is
  taken from the requirements; the sleep is called once per frame; the token is issued once per game frame;
  deterministic runs do not call Reflex; a missing plugin does not disable DLSS.
- The probe stays a manual GPU check (`tools/reflex_probe.cpp`), not a CI test: it needs an NVIDIA driver and
  the vendor DLLs.

## 5. Risks and open questions

1. **Frame attribution across the async worker** (highest). Two presents may be queued when the next
   simulation starts; the wrong token makes the report (and possibly DLSS options/tagging) attach to the wrong
   frame. Mitigation: the per-frame token ring and a frame id in the present job; prove it with the verification
   HUD showing monotonic, non-zero timestamps.
2. **Threading between Reflex and DLSS.** Markers/sleep on the limiter thread, DLSS evaluation on the worker.
   The documented thread safety does not cover a global lock around the sleep; a wrong lock deadlocks the
   present. Mitigation: no shared mutex across the sleep; keep SL calls short and measure with the worker on
   and off.
3. **Real benefit in the default mode.** 60 FPS + V-Sync + no VRR is the case where NVIDIA's matrix says no
   gain. The port's two-present frame lag is the argument for a gain; this must be measured in 60 V-Sync,
   120/unlimited (mailbox) and 30 V-Sync repeat-present configurations.
4. **FSR frame generation.** Presents are produced inside AMD's replacement swapchain; with the Streamline
   proxies installed it forwards its swapchain calls to them (`temporal_fg.inc` `fgsc_proc`), so Streamline sees
   more presents than game frames and the host cannot mark the generated presents as out-of-band. Reflex + FSR FG
   is therefore unproven; v1 should either report it as unsupported or be tested separately before it is
   advertised.
5. **Startup cost / stall with Streamline loaded.** `slInit` takes ~1.1-1.3 s on this machine (it scans every SDK
   plugin in the directory). More important, the hidden-run validation found that **loading Streamline at all**
   (Reflex, or DLSS-only on an unsupported GPU) makes the port's pipeline prewarm phase 2-3x slower: the game's
   watchdog reports three 5-8 s stalls at frames 7-14, while the same run without Streamline reaches that point in
   5.5 s. It is **not** the Reflex sleep (`XCDE_REFLEX_NOSLEEP=1`), the swapchain proxy (diagnostic without the
   surface hooks) or the unrequested DLSS/NGX plugins (a Reflex-only plugin directory): it reproduces with
   DLSS-only. See `docs/evidence/reflex_impl_2026-11-18.md` / `.en.md`. The hidden runs exit with `_exit(0)`, so they never
   save `cache/vk_pipelines.bin` and rebuild ~290 programs every time; a normal quit saves it, and the project's
   precompile step can warm it, so the cost in normal play must be measured before deciding whether Reflex can be
   default-on. If it remains, options are to defer Streamline loading until the device/swapchain actually needs it
   (hard with manual hooking) or to scan a feature-specific plugin directory.
   **Normal-launch follow-up:** visible launches with the user's settings were measured on/off with a warm cache and
   with `cache/vk_pipelines.bin` deleted (driver cache warm), killing at 20/45 s. Frame 8 took 6.0-6.7 s, the
   prewarm holds were ~1.5-1.8 s and there were **no** `[stall] no frame` reports in any of the four runs; the 5-8 s
   stalls were a hidden-harness/forced-exit or very-cold-driver-cache artifact, not a Reflex regression in normal
   play. A first-ever run with no driver shader cache was not tested.
   A real launcher run with `reflex = on` (1920x1080, V-Sync, `upscaler = fsr3`, `run_speed = 2.00`) crashed at
   frame 8 during this stall, in the run-speed path: `fill_state` -> `party_game_tick` -> `run_speed_leader` ->
   `field_group` -> guest `f_7000593120`, which reads `(*(pm)+0x680)+0xda8` without a null check. While the field
   object is still being constructed `+0x680` is 0, so the guest read faults (`c0000005`, read of `0xda8`); the
   longer Streamline startup widens that window. This is a pre-existing race in the run-speed lookup, not a
   Streamline/Reflex API error. It is fixed by a guard in `field_group` that defers the lookup until the link
   exists (`runtime/hle/party.c`); repeat runs with the same config pass, and the guard is exactly the read that
   faulted. See `docs/evidence/reflex_impl_2026-11-18.md` / `.en.md`.
6. **Non-NVIDIA and hybrid.** `VK_NV_low_latency` does not exist on AMD/Intel; the option must be hidden or
   disabled and no extension may be added. On the Optimus/switchable-graphics path the selected adapter decides.
   `slIsFeatureSupported` remains the final check, not the vendor name.
7. **Drivers.** Reflex needs 456.38+; the extension being present is checked by Streamline, but an old driver
   must fail gracefully. The pinned Streamline 2.14.16 and the signature check stay.
8. **Deterministic/headless runs.** `diag_deterministic()` runs and the hidden-window headless path must not
   call Reflex (no presents, virtual time). Guard like DLSS does.

## 6. Validation plan (only after implementation)

1. Regressions with Reflex off: routes identical, Reflex is not requested (`featuresToLoad` has no
   `kFeatureReflex`), no `[reflex]` status, markers or sleep, and the DLSS path is byte-for-byte the current one.
2. NVIDIA machine: `ReflexTestEnable.exe 1` (Reflex verification HUD) shows `App_Called_Sleep = 1`, all
   timestamps non-zero and monotonic, and `PrintPCL`/FrameView reports PCL not 0.0; check Off / On / On + Boost.
3. A/B latency in three configurations: 60 FPS V-Sync, 120/unlimited (mailbox), 30 FPS V-Sync (repeat presents).
   Report average PCL, 1%/0.1% lows, FPS change (NVIDIA's QA bar is under 4%), and the same routes as the
   existing capture tests (field, battle, cutscene, menu).
4. Robustness: window resize/minimise/restore, resolution and upscaler changes, FSR FG on/off, alt-tab,
   shutdown, and the "DLL missing" / non-NVIDIA fallbacks.
5. Evidence: one document with the machine, build hash, PCL numbers and the verification HUD screenshot, in the
   style of the existing evidence files.

## 7. Effort

| Stage | Work | Estimate |
|---|---|---|
| 0 | Split the Streamline runtime from DLSS gating; deploy `sl.reflex.dll` + `sl.pcl.dll`; no Reflex yet (DLSS behaviour must stay identical) | 0.5-1 day |
| 1 | Reflex feature, capability state, shared token ring, markers, sleep, status log, env switch for A/B | 2-3 days |
| 2 | Settings, launcher, F1 menu, ini round-trip, i18n, packaging/licences | 1 day |
| 3 | Validation and tuning (three display configurations, FG, hybrid/negative cases) + evidence | 2-3 days |
| | **Total** | **~1-1.5 weeks** |

The old estimate in `graphics_options_research.md` ("medium", cost 0) is confirmed. The Reflex API itself is
small; the cost is in the frame/thread ownership and in testing the display configurations that actually gain.

## 8. Implementation status (2026-11-18, opt-in)

Implemented on branch `feat/nvidia-reflex` (branched from `main` at 474f616). No game data or generated code changed.

| Area | Change |
|---|---|
| Streamline runtime | `runtime/nvn/vulkan/dlss.cpp`: loads the feature set the settings ask for (DLSS and/or Reflex + PCL), takes the union of their requirements, and keeps `sl_rt_ok` / `dlss_ok` / `reflex_ok` separate. An unsupported DLSS (a GTX without RTX) no longer tears down Reflex; a missing plugin only drops its own feature. The proxy surface and swapchain hooks are created as soon as a feature is usable. |
| Frame token | One `sl::FrameToken` per game frame in an 8-slot ring (`token_for`), issued at the frame boundary and shared by the PCL markers, `slReflexSleep` and `dlss_evaluate` (which keeps its old per-evaluation token only when Reflex is off). The present path carries the game frame through the async render worker. |
| Frame loop | `framepace.c` `xcde_limiter_spin`: once per frame `slReflexSleep` (before the existing T157 nap); `xcde_limiter_done`: `eSimulationStart` when the limiter returns; `objects.c` `nvnQueuePresentTexture`: `eSimulationEnd` + `eRenderSubmitStart`; `device.c` `vk_frame_end`: `eRenderSubmitEnd` + `ePresentStart` around `vkQueuePresentKHR`, `ePresentEnd` after. Repeat presents at 30 FPS do not emit a second set. |
| API | `runtime/nvn/vulkan/reflex.h` (new) + `reflex_*` exports in `dlss.cpp` (no-op stubs on Linux). Status text for the menu/log (`[reflex] ...`). |
| Settings/UI | `xcde.ini` `reflex = off / on / boost` (default **off**, opt-in), `runtime/settings.h`, `host/settings_ini.c`, F1 **Gráficos > Velocidad**, launcher **Velocidad**; `XCDE_REFLEX=0/1/2` for A/B. Deterministic runs disable it. |
| Deployment | `sl.reflex.dll`, `sl.pcl.dll` and `NvLowLatencyVk.dll` added to `cmake/copy_upscaler_dlls.cmake`, `scripts/copy_upscaler_dlls.sh`, `tools/install_dlss.py`; `NOTICE.md` updated. `tools/make_portable.py` already copies every `build/*.dll`. |
| Tests | `tests/native/test_settings_roundtrip.c` (key round-trip), `tests/unit/test_reflex_contracts.py` (source contracts), `tests/unit/test_install_dlss.py` (DLLs). |

Validation state (2026-11-18):

- The full runtime and all 320 generated game translation units were rebuilt with the project's own flags (the
  object cache had been cleared) and linked into `build/xcde.exe` (385 objects, 0 failures), outside the project's
  bash/ninja scripts (blocked in this environment).
- Hidden runs of the real game (`LARECOMP_HIDDEN=1`, 240 frames, screenshot, exit):
  - Reflex only (`upscaler = fsr`, `reflex = on`): `loaded (requirements: dlss 0, reflex 1, pcl 1; 2 feature(s))`,
    `Reflex disponible (baja latencia)`, `[reflex] frame loop active (frame 1)`, exit 0;
  - Reflex off: `Reflex no solicitado`, exit 0, no watchdog stalls;
  - DLSS requested on this non-RTX GPU: DLSS unsupported, and the same startup stalls as Reflex-only (see risk 5).
- Native settings round-trip passes with the new key; `tests/unit/test_reflex_contracts.py` (4/4) and
  `tests/unit/test_install_dlss.py` (5/5) pass; `tools/validate_port.py` compiles every runtime/UI source (5 native
  tests are not linkable here because this environment's MSYS2 lacks `libwinpthread.dll.a`).
- **Not measured:** input-to-photon latency / PCL with the Reflex verification HUD or FrameView (the SDK's
  `ReflexTestEnable.exe` HUD tool is extracted but needs an elevated console), gameplay (the runs stop at the title
  screen), and DLSS + Reflex together (no RTX on this PC). Until then the feature stays `off` by default and this
  document does not claim a latency gain. `XCDE_REFLEX_NOSLEEP=1` and `XCDE_REFLEX=0/1/2` are the A/B switches used
  above; the first is diagnostic only. `XCDE_REFLEX_STATS=1` logs `slReflexGetState` every 120 frames: a 12 s run
  reported `sleep_ok=241 fail=0` (every frame), `pcl_markers=1440` (6/frame), `report=1 low=1` and non-zero
  driver/OS/GPU timestamps (`gpu_us ≈ 16.4-17.0 ms`), so the sleep/marker/PCL report path is verified even though
  the driver HUD overlay has not been made visible on this machine (the NVIDIA App Alt+R overlay is not installed;
  the Reflex HUD tool needs FrameView or an elevated test setup).
- **Startup crash (fixed):** the first real launcher run with `reflex = on` crashed at frame 8 in the run-speed path
  (`f_7000593120+0xb0`, read of `0xda8`) because the longer Streamline startup let the party scanner find the field
  object before its `+0x680` link existed. `field_group` (`runtime/hle/party.c`) now defers the guest call until the
  link is present; the same launcher/hidden configs run without the crash. This is a pre-existing timing race in the
  run-speed feature, exposed by the startup stall, not a Reflex API failure.
- **Teardown crash (fixed):** closing the window with the X button while Reflex was on produced `c0000005` after
  2409 frames. The minidump shows the address inside **unloaded** `sl.dlss.dll` (`+0x4ba50`): Streamline's
  `slShutdown` unloads the plugins while an NGX/DLSS worker thread is still executing them. `dlss_shutdown` no
  longer calls `slShutdown` at process exit; the runtime stays mapped until Windows reclaims it. A direct
  `WM_CLOSE` run produced no new crash report.

To try it on an NVIDIA machine: set `reflex = on` in `xcde.ini` (launcher or F1) and start the game; the log shows
`[reflex] Reflex disponible ...` when the feature came up, `[reflex] frame loop active` once the limiter drives it,
or the reason in the F1 menu. Enabling it in F1 on a run that started with `reflex = off` needs a restart (the
runtime was not loaded).

## 9. Non-goals for this effort

- **DLSS Frame Generation.** It is the main NVIDIA Reflex consumer, but it needs a recent RTX GPU and a
  replace-the-swapchain integration; this project ships AMD FSR frame generation and does not request `sl.dlss_g`.
- **Replacing the game's frame limiter** with `ReflexOptions::frameLimitUs`.
- **Promising a latency number** before the verification HUD/FrameView measurement. The only claim this
  document supports is that the runtime path works on the available PC.
- **Linux.** Streamline/Reflex is the Windows path; the Linux build keeps its native FSR components.

## Reproduction

```powershell
# from the repository root, with the SDK DLLs in sdk/sl/bin/x64 (git-ignored)
.venv\Lib\site-packages\ziglang\zig.exe c++ -target x86_64-windows-gnu -mcpu=x86_64_v2 -std=c++20 `
    -I third_party/streamline/include -idirafter C:/msys64/ucrt64/include `
    -o build/reflex_probe.exe tools/reflex_probe.cpp C:/msys64/ucrt64/lib/libvulkan-1.dll.a -luser32 -lgdi32
build\reflex_probe.exe            # Reflex + PCL
$env:PROBE_DLSS=1; build\reflex_probe.exe   # also request DLSS (must not take Reflex down)
```
