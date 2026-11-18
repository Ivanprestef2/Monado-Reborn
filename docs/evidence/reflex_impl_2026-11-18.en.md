# Evidence: NVIDIA Reflex (T272), opt-in implementation (2026-11-18)

> Spanish version: [`reflex_impl_2026-11-18.md`](reflex_impl_2026-11-18.md)

**Question:** does the Reflex integration added in this session work (Streamline runtime shared with DLSS,
per-frame token, markers, *sleep*, `reflex` setting, DLLs)? **Result:** it compiles, links and starts in the real
game: Streamline loads Reflex + PCL, `slReflexGetState` reports low latency available and the frame loop runs
(`[reflex] frame loop active`). Latency measurement and one startup anomaly with Streamline loaded remain (below).
Design and limits: [`../reflex_research.md`](../reflex_research.md).

## Machine and build

- Windows 10.0.19044; **NVIDIA GeForce GTX 1050 Ti** (Pascal, no RTX), driver **582.78**, Vulkan **1.4.312**,
  8 logical CPUs.
- Branch `feat/nvidia-reflex`, created from `main` (`474f616`) with the T272 integration. No game data or
  generated code was modified.
- The `build/` objects had been cleared: the 320 generated `.c` files and the 65 runtime objects were rebuilt with
  the same flags as `scripts/build_boot.sh` / `scripts/build_rt_gfx.sh` (zig cc/c++), and linked with the command
  from `scripts/link_gfx.sh` (385 objects), final executable of this session. This session's environment blocks the
  project's bash/ninja scripts, so the build used an equivalent Python driver (logs in `build/`, ignored).

## Method

Short hidden runs (`LARECOMP_HIDDEN=1`, screenshot and exit at frame 240), without touching keyboard/mouse:

- **A - Reflex only:** `xcde.ini` with `upscaler = fsr`, `reflex = on`; `XCDE_INI=<repo>\build\reflex_probe\reflex_only.ini`.
- **B - baseline:** the same file with `reflex = off` (Streamline is not loaded).
- **C - DLSS without RTX:** `upscaler = auto`, `reflex = off`; Streamline loads for DLSS, which is unsupported.

## Results

| Check | Result |
|---|---|
| Runtime and game compile | 320 + 65 units, **0 failures**; 385-object link OK |
| Run A: features loaded | `loaded (requirements: dlss 0, reflex 1, pcl 1; 2 feature(s))` |
| Run A: state | `[reflex] Reflex disponible (baja latencia) en la GPU activa` (device and surface) |
| Run A: frame loop | `[reflex] frame loop active (frame 1)` |
| Run A: DLLs | `sl.reflex.dll`, `sl.pcl.dll` and `NvLowLatencyVk.dll` loaded with verified NVIDIA signatures |
| Run A: execution | 240 frames, `build/window.png` saved, exit **0** |
| Run B (no Streamline) | `[reflex] Reflex no solicitado`; 240 frames, exit 0, no stall warnings |
| Run C (DLSS unsupported) | `[dlss] DLSS no compatible...`; shows the same startup stalls as A |
| `test_settings_roundtrip` | passes (includes the `reflex` key) |
| `tests/unit/test_reflex_contracts.py` | 4/4 OK |
| `tests/unit/test_install_dlss.py` | 5/5 OK |
| `tools/validate_port.py` | every source compiles; 5 tests do not link here because `libwinpthread.dll.a` is missing |

## Open finding: startup with Streamline loaded

With Streamline loaded (runs A and C) the game's watchdog reports **3 stalls of 5-8 s** at frames 7-14, during
pipeline construction; frame 8 reaches t=11.9 s (max 8162 ms). Without Streamline (run B) the same phase finishes
at t=5.5 s (max 2003 ms) with no warnings. Isolation:

- not `slReflexSleep`: with `XCDE_REFLEX_NOSLEEP=1` the stalls remain;
- not the swapchain proxy: with the surface/hooks disabled (temporary diagnostic) they remain;
- not the unrequested DLSS/NGX plugins: with a Reflex-only plugin directory (temporary diagnostic) they remain;
- it reproduces when requesting **DLSS only** on this GPU (run C), without Reflex.

That is, loading the Streamline runtime (interposer/plugins) slows the port's pipeline prewarm. The test runs exit
with `_exit(0)` (screenshot), so they **do not save** `cache/vk_pipelines.bin`: each run rebuilds ~290 programs. A
normal close saves the cache, and the project's precompile step can start warm; measure a normal launch before
considering `reflex = on` by default. No latency gain is claimed: the Reflex verification HUD / FrameView is
missing.

### Normal-launch measurement (20:40)

Normal visible launches (FIFO window, `fsr3`, `run_speed = 2.00`, same settings) were repeated and killed after
20 s (warm cache) or 45 s (pipeline cache file deleted, driver cache warm), comparing `reflex = on` and `off`:

| Launch | Frame 8 | Prewarm | `[stall] no frame` | Rate at the end |
|---|---|---|---|---|
| warm, Reflex on | t=6.1 s | 1471 ms (2112 builds) | 0 | 579 frames/18.1 s (53.1 fps) |
| warm, Reflex off | t=6.0 s | 1478 ms (2110 builds) | 0 | 596 frames/18.1 s (56.3 fps) |
| cold, Reflex on | t=6.7 s | 1802 ms (2111 builds) | 0 | 1972 frames/42.9 s (58.4 fps) |
| cold, Reflex off | t=6.7 s | 1836 ms (2112 builds) | 0 | 1968 frames/42.8 s (58.7 fps) |

"Cold" deleted `cache/vk_pipelines.bin` (the driver cache and `variants.bin` remained); the user's warm cache was
restored afterwards. **The 3 stalls of 5-8 s from the hidden runs did not reproduce**: normal startup is the same
with Reflex on and off (< 0.1 s to frame 8, within noise; the final FPS depends on the scene). The startup anomaly
is therefore a hidden-harness/forced-exit or very-cold-driver-cache artifact, not a Reflex regression in normal
play. A first-ever launch on a machine with no driver cache was not tested.

## Close (20:00): startup crash with Reflex and fix

The first real launcher start with `reflex = on` (`upscaler = fsr3`, `run_speed = 2.00`, `party_switch = 0`,
V-Sync, visible window) **crashed** at frame 8:

- `crash/crash_20261118_200033.txt`: `c0000005` at `f_7000593120+0xb0`, **read of 0xda8**, last presented frame 8;
  the same startup stalls came first (report in `crash/stall_1.txt`).
- Stack: `fill_state` → `party_game_tick` → `run_speed_leader` → `field_group` → `guest_call` →
  `f_7000593120+b0`. Guest disassembly: `x20 = *(obj + 0x680)` then `LD64(x20 + 0xda8)` with no null check;
  while the field object has just been found and `+0x680` is still 0, the read lands on `0xda8`.
- It is a **pre-existing run-speed race**: the party scanner finds the field object during loading, before the game
  links `+0x680`; the Streamline startup stall (above) widens the window. It is not a Reflex API or proxy call.
- Fix in `runtime/hle/party.c` (`field_group`): read `*pm` and `+0x680` and defer the guest call until the link
  exists (`[party] field manager not linked yet: run-speed lookup deferred`, max 3 messages). Once the link exists,
  behaviour is unchanged.

Validation: runtime rebuilt and linked; the same user settings pass in hidden and visible 240-frame runs (no
crash). In those runs the guard did not actually fire (the race is timing-dependent), but the condition checked is
exactly the read that faulted in the dump. The startup cost with Streamline remains open (previous section); this
fix stops it from becoming a crash with `run_speed` active.

## X-button close (20:14): teardown crash and fix

The user played a real session (2409 presented frames) and closed with the window's **X** button; the game wrote
`crash/crash_20261118_201438.txt`:

- execute **`c0000005`** at `0x7ff87e2aba50`, last frame 2409; the failing thread is 0x2018 (host), not a game
  thread. The log ends at `window closed`.
- the minidump lists `sl.dlss.dll` as an **unloaded module** (base `0x7ff87e260000`, size `0x69000`); the faulting
  address is `sl.dlss.dll+0x4ba50`. `sl.reflex.dll`, `sl.pcl.dll` and `sl.common.dll` are also unloaded, while
  `sl.interposer.dll` is still loaded.
- cause: `slShutdown()` (called from `dlss_shutdown`'s `atexit`) unloads the plugins while an NGX/DLSS worker thread
  can still be executing `sl.dlss` code; on return, the thread jumps into unloaded code. It is not a game or session
  failure: only the orderly-close path. The X button runs `ui_run` → `SDL_EVENT_QUIT` → `exit(0)` → `atexit`; the
  in-game menu can exit another way.

Fix: `dlss_shutdown` **no longer calls `slShutdown()`** (or `vkDeviceWaitIdle`) when the process exits; it leaves
the runtime mapped and Windows reclaims it. That is safe because the process is exiting and no Vulkan/Streamline
calls follow.

Validation: rebuilt and linked (`build/xcde.exe`, 20:22); a direct `WM_CLOSE` run produced no new crash report
(0 reports) and the process terminated. Confirmed by the user's normal launch and X close: no report.

## Reflex verification HUD: not available on this setup

The SDK zip (`streamline-sdk-v2.14.16.zip`, downloaded by the user) contains `utils/reflex/`:
`ReflexTestEnable.exe`, `ReflexTest.exe`, `PrintPCL.exe` and `FVSDKSetup.exe`. `utils/reflex` was extracted to
`build/reflex_probe/reflex_utils/` (ignored). `ReflexTestEnable.exe 1` answers **"Please run with elevated/admin
privileges"**; run elevated it returns 0. `build/xcde.exe` was added to the NVIDIA Control Panel profile and both
windowed and fullscreen were tried, but the HUD **does not appear in the desktop capture**.

To avoid depending on the overlay, `XCDE_REFLEX_STATS=1` was added; it logs `slReflexGetState` every 120 frames.
Result of a 12 s session (windowed, `fsr3`, Reflex on):

```
[reflex] stats frame=241 sleep_ok=241 fail=0 pcl_markers=1440 low=1 report=1 msg=49692
[reflex] report frame=236 input=0 sim=... submit=... present=... driver=... os=... gpu=...
           gpu_us=17011 active_us=2669
```

- `sleep_ok=241 fail=0`: `slReflexSleep` was called and returned `eOk` on **every** frame (the HUD's `App Called
  Sleep` would be 1).
- `pcl_markers=1440`: 6 markers per frame (simulation, submit and present, start/end) accepted by the plugin
  (`eOk`); the HUD's PCL would not be 0.
- `report=1`, `low=1`: a latency report is available and low latency is supported; driver/OS/GPU timestamps are
  non-zero and `gpu_us ≈ 16.4-17.0 ms` at 60 FPS.
- `input=0` because input sampling is not marked (an input marker would be needed; optional).
- The `present` fields in this sample have start=end (zero-duration section on a loading frame); the other
  sections have start<end.

FrameView SDK (`FVSDKSetup.exe`) was installed and the PC restarted; the `FvSvc` service was stopped and was
started; test mode was re-enabled, the game was added to the NVIDIA profile, windowed/fullscreen and V-Sync on/off
were tried, an exclusive-fullscreen attempt was made (reverted), and `ReflexTest.exe curve` was launched. **The HUD
never rendered** in this configuration (Vulkan + Streamline manual hooking, unsigned executable), even though the
underlying values are verified by `XCDE_REFLEX_STATS`. The visual HUD/FrameView remains pending for a signed/D3D
build or the NVIDIA App; it does not block the integration or the markers.

## Limits

- The automated runs are title-screen runs (240 frames), not gameplay; the user's real session (2409 frames) was
  not measured with the HUD or FrameView.
- PCL / input latency was not measured; no RTX to test DLSS + Reflex together.
- Full logs remain in `build/reflex_probe/` (ignored); the sanitized excerpt is
  [`reflex_impl_2026-11-18_run.txt`](reflex_impl_2026-11-18_run.txt).
