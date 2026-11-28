# Development changelog

New developer documentation is written in English. This file is the place for session prose that would otherwise be
appended to [`tasks.md`](tasks.md): what was done, what was measured and what is left, one section per work session.
Keep the ledger in `tasks.md` (task id + status) and the runtime edit log in [`../UPSTREAM.md`](../UPSTREAM.md)
(article X) separate from this narrative.

Older session notes live in the archived handoffs under [`archive/`](archive/) and in the historical session sections
of `tasks.md`; do not move them, they are the record.

## 2026-11-28 — runtime reliability and build defaults (T283)

- Stage save recovery and preserve the committed journal through failed restores. Allocate guest thread
  IDs and TLS under the kernel lock. Release IPC service resources after the last session or domain reference.
- Report RomFS I/O failures to the guest. Build sibling links in one pass. Keep working audio streams through
  failed device changes and report settings-write failures in the overlay.
- Associate native Vulkan present waits with acquired images. Drain the FSR 3 present queue before reusing its
  semaphore ring. Correct AppImage skip-launcher checks and pass an explicit dump to the launcher.
- Default to `-O3` for CMake builds, legacy builds, and packaged game-code builds. Make CMake reconfigure for ABI
  changes. The first-start builder fingerprints input contents, locks its output folder, and stages its link.
- Restore the SIMD differential harness and run it in Linux CI. Build SDL3 in CI so the Linux platform tests
  are required. The published PGO profile is reused with the new default.

## 2026-11-20 — documentation cleanup (Phase 1)

- Reorganized `docs/` into `adr/`, `contracts/`, `dev/`, `evidence/`, `guides/`, `history/`, `reference/` and
  `user/es/`; added the [`../README.md`](../README.md) index and the [`../adr/README.md`](../adr/README.md) index.
- English is canonical for developer documentation; the Spanish player guide is [`../user/es/README.md`](../user/es/README.md).
  The Spanish originals of `PERFORMANCE_UI`, `VULKAN_DLSS_FPS`, ADR 0011 and `UPSTREAM` are archived in
  [`../history/es/`](../history/es/).
- Archived the 2026-10-25 handoff (`archive/HANDOFF-2026-10-25.md`), the 2026-10-21 shader-pipeline handoff and the
  session 14 work log; the current status is in `tasks.md` and `../UPSTREAM.md`.
- Added `tools/check_docs.py` (documentation link/path checker), wired into `scripts/ci.sh` and CI.

## 2026-11-20 — code and tooling cleanup (Phase 2)

- Grouped `tools/` by purpose: `build/`, `gamebuild/`, `package/`, `lifter/tests/`, `tours/`, `diag/`, `media/`,
  `research/` and `setup/` (with a README in the groups that need one). The two repo-wide checkers stay at
  `tools/check_docs.py` and `tools/check_paths.py`.
- Deleted dead files: the tracked `*.bak` copies, the orphan swkbd HLE unit, the unused Vulkan 1.1 compatibility shim
  and its orphan test, the replaced menu stub, the uncompiled Metal sources, the superseded `make_dist`/`drive`
  scripts, the desktop-path image helpers and the legacy FSR 4 Lite preparation script (superseded by
  `tools/package/fsr4_prepare.py` + `tools/build/build_fsr4prep.py`). The exact list is in the commit message; the
  generated `docs/hle_audit.md` was regenerated.
- Pruned `third_party/imgui/` to the 17 files the overlay and launcher compile (removed examples, docs, misc, demo and
  unused backends: 225 files, ~3.8 MB) and added `third_party/README.md` with upstream, pin, licence and bump
  instructions. Added the missing FSR 4 Lite capture tooling row to `NOTICE.md`.
- Added `tools/check_paths.py` (repo path literals in scripts, workflows, Python tooling and `CMakeLists.txt`) with
  `tests/unit/test_check_paths.py`, wired into `scripts/ci.sh` and CI; added `docs/dev/repo-layout.md` (current tree);
  moved the GPU compatibility matrix to `docs/reference/gpu-compatibility-matrix.md`.
- Deferred: `tools/lifter/tests/test_simd.py` is still broken (it references a `test_simd_harness.c` that was never
  committed) and neither it nor `test_lift.py` can run here (no Unicorn/Capstone); Windows-only build paths were not
  executed locally.

## 2026-11-20 — cleanup follow-ups and decisions (Phase 3)

- Proposed [ADR 0013](../adr/0013-variant-seed-publication.md): a narrow, explicit Article I exception for
  `data/variants_seed.bin` (numeric pipeline-state keys only, reproducible with `tools/research/merge_variants.py`;
  no game content). The ADR is **Proposed**: accepting it needs a one-line Article I amendment plus the changelog
  entry that Article I requires.
- Removed the orphan Vulkan 1.1 compatibility header (`vk_compat`; its implementation and test were deleted in Phase 2) and the
  stale `host_menu_stub.o` cleanup line from `scripts/build_rt_gfx.sh`.
- Verified on the cleaned tree: `bash scripts/ci.sh` (113 unit tests, package guard), `tools/check_docs.py`,
  `tools/check_paths.py`, `tools/diag/hle_audit.py --check`, `package_guard --git`, a runtime-only CMake build and a
  release-mode `monado_launcher` build.
- Left open as decisions (not cleanup): consolidating the four hand-synced build paths, shipping `LICENSE`/`NOTICE`
  and the GPL notice for the FSR 4 Lite capture tooling in release packages, pruning merged remote branches and
  normalizing tag names, and pinning GitHub Actions / unifying the CI Python version.

## 2026-11-21 — fix menu/map/load backdrop corruption on dynamic resolution resize (#172)

- Cross-referencing the reporter's `last_game.log` from GitHub #172 showed `res_mode = Dynamic` stepping internal
  resolution back up (`0.80 -> 1.00`) as soon as a paused screen (Main Menu, Area Map, Load Game) dropped GPU time,
  triggering `tex_backend` reallocation of persistent 16:9 captured backdrops without preserving pixels.
- Preserved GPU-owned scalable color textures across all shared mip levels via `vkCmdBlitImage` before deferring old
  image destruction (`runtime/nvn/vulkan/textures.c`, `runtime/nvn/vulkan/tex_resize.h`), and invalidated upload state
  for CPU-backed scalable textures (`XCDE_RESIZE_PRESERVE=0` opt-out).
- Hardened new uncompressed color render-target creation in `create_image` with a zero-clear (`XCDE_CLEAR_NEW_COLOR_RT=0`
  opt-out) and added `XCDE_POISON_NEW_IMAGES=1` magenta initialization for catching unwritten-RT reads on non-RDNA3 GPUs.
- Scoped `tmp_lod_bias()` to CPU-uploaded mipmapped material textures in `exec.c` (`XCDE_TMP_LOD_BIAS_ALL=1` escape hatch)
  and extended `SamplerVariants` in `textures.c` by quality bucket so live upscaler/quality switches take effect cleanly.
- Added `tests/native/test_tex_resize.c` (wired into `scripts/native_tests.sh`) and source contract assertions in
  `tests/unit/test_vk_source_contracts.py`.

## 2026-11-21 — fix 3D menu model jitter under temporal upscalers (#168)

- Traced frame 2900 of `tests/routes/menu_all.txt` (Change Equipment): the game tonemaps the jittered 3D stage and
  character into an offscreen target (`pass 198`), draws stale field HUD into a fresh presented target (`passes 199-200`),
  overwrites the presented target with the blurred field backdrop and dim overlay (`pass 201 draws 0-1`), composites the
  offscreen 3D scene target onto the presented target (`pass 201 draw 2`), and then draws the 2D menu UI (`pass 202`).
- Because `tmp_scene_frame` was already set by the earlier 3D draws, `T220`/`T213` took `tmp_hud_snapshot()` on
  `pass 199` (pitch black), causing FSR 3 / FSR 4 Lite / DLSS to upscale a blank image and `tmp_hud.frag` to replace
  the entire 3D character and room with the raw un-dejittered render-size image (`fin`).
- Added offscreen-to-presented scene provenance tracking (`TexBackend.tmp_scene_frame`, `tmp_presented_scene_frame`,
  and pure-C11 helpers in `runtime/nvn/vulkan/tex_resize.h`, with `XCDE_TMP_SCENE_PROVENANCE=0` fallback) so the
  pre-HUD snapshot is taken right after `pass 201 draw 2` composites the 3D scene into the presented image.
- Verified on `tests/routes/menu_all.txt` (frames 2900-2905, FSR 3.1 Performance): frame-to-frame error on Shulk's
  boots dropped from `1.69-2.60` (`±1 px` shift per frame) to `0.105-0.169` (`(0, 0)` shift), character sharpness gradient
  rose from `1.732` to `1.886`, and Title (400), Field (2500), and Main Menu (2700) remained intact.

## 2026-11-21 — timestamps in the game log (T275)

- `build/last_game.log` now dates the runtime's lines: `runtime/log.c` adds `[YYYY-MM-DD HH:MM:SS.mmm]` (local
  time) to the `LOG`/`FATAL` macros, the watchdog header, crash/backtrace output and the direct diagnostics that wrote
  to stderr directly (the multi-line thread dumps of a stall stay under their timestamped header).
  The calendar text is cached per thread per second, so the per-line work is a vDSO wall-clock read plus three
  millisecond digits; one `vsnprintf` and one `fwrite` per line (about 110 ns over `fprintf` in a 200,000-line
  microbenchmark, far below the write itself). A partial line keeps one prefix and multi-line messages get one per
  line. `XCDE_LOG_TIME=0` restores the un-prefixed output.
- `runtime/platform.h`/`platform/linux.c`/`platform/windows.c`: `plat_wall_ns()` (UTC since the Unix epoch) and the
  `rt_logf` declaration; the new unit is in `RT_UNITS`, `scripts/build_*` and the link lists.
- `tests/native/test_log_timestamp.c` (wired into `scripts/native_tests.sh` and `tools/diag/validate_port.py`) checks
  first-line, partial-line and multi-line prefixes; the focused native tests that link a runtime unit using `LOG` got
  a one-line `rt_logf` stub.

## 2026-11-21 — camera follows jumps (T276b)

- The field camera's vertical look target stayed at ground level while the character jumped (`camera + 0x5300`, look
  point `+0x5400`), so with the 8x high jump the character left the frame. The new `camera_follow_jump` option raises
  both target Y values with the controlled leader's Y from the pad read and lets the game's own camera update
  (`f_70000510a0`) compute the position, pitch, collisions and smoothing; horizontal targets and the look offset stay
  native. Menus, cutscenes and event cameras are skipped (`player_in_control` + `cutscene_playing`).
- Found with an object probe and `XCDE_WATCH_BT` backtraces (`f_70000510a0` → `f_70000547b0` → `f_700072c070`); the
  renderer's view matrix confirmed the camera's world Y rising 23.99 → 28.55 while the leader went 22.99 → 27.91
  (FSR 3 probe). Option in F1 → Cheats → Exploration and the launcher, `camera_follow_jump = 0/1` in `xcde.ini`;
  `test_settings_roundtrip.c` covers the round-trip and `XCDE_CAM_TRACE=1` logs the followed values.

## 2026-11-22 — the game image never reached the swapchain after the post.c split (#210)

- Symptom: on Linux (KDE/Wayland, NVIDIA 580.196.07) the game ran — audio, 60 fps, `last_game.log` full of presents —
  but nothing was shown: the taskbar listed the window and it never mapped. `LARECOMP_WINDOW_SHOT` captures had the
  performance overlay drawn over a black game image, while the same capture with `LARECOMP_VKQ=0` was correct (the
  loading screen), which localised the defect to the deferred recording path.
- Cause: PR #210 moved the present / SMAA / FSR 1 passes out of `runtime/nvn/vulkan/exec.c` into a new
  `runtime/nvn/vulkan/post.c` without the `vkq.h` include. On Linux the ordered recording worker (T-LINUX-THREE) is on
  by default, and `exec.c`, `device.c`, `temporal.c` and `textures.c` route every `vkCmd*` through it; the new unit
  called the driver directly from the render thread while the worker replayed into the same command buffer. The present
  draw raced the worker (the game image was never composed, the Wayland surface never got its first buffer, so the
  window never mapped), and when both threads recorded at once the command stream collapsed (`free(): invalid size`
  inside `vkBeginCommandBuffer`, which then hit most launches).
- Fix: `post.c` includes `vkq.h` after `vk.h` (one line plus the reason) — the ordering it had inside `exec.c` — and a
  build-time `#error` keeps the invariant if the passes are split into another unit again.
- Verified (GTX 1050 Ti, driver 580.196.07, `8eb62ed` + this change): before, frame 500 captured overlay-over-black and
  launches aborted at the first present; after, fullscreen 1920x1080 with `upscaler = fsr3` renders the loading screen
  and runs 60 fps for 32 s (frame 1784) without a crash, and the title screen (frame 1800) is correct.
  `LARECOMP_VKQ=0` renders correctly on the unpatched build too: that A/B is the reproduction.

## 2026-11-23 — crash on exit with DLSS (GitHub #216)

- Symptom: the #216 report ("missing particles on the Monado") also carried the crash from that evening. Both sessions
  in its ZIP (`last_game.prev.log`, `last_game.log`) end the same way: the player closes the window (`window closed`)
  and the process faults with `0xc0000005` at the same address, `host lr … (dlss_evaluate)`. It is a crash on **exit**,
  not "after the battle": 2 of the 2 logged sessions (one of them only 3 minutes long).
- Evidence (minidump parsed): the faulting RIP lies inside **no loaded module** (the address hole between
  `nvspcap64.dll` and `libopus-0.dll`), i.e. code that had just been unloaded; the crashing thread's stack points back
  into `dlss_evaluate` and below it into `nvoglv64.dll` (the NVIDIA ICD). The guest dump has the game's threads still
  running (`nnMain` "running", 41 threads, 40 blocked).
- Cause: `exit(0)` runs the `atexit` handlers in reverse order of registration. `quick_exit_handler` in `device.c` —
  the hard exit that drains the recording queue, waits for the GPU and calls `plat_terminate()` — is registered first,
  so it ran **last**, while `dlss_init_early()` registered `atexit(dlss_shutdown)` later, so `slShutdown()` ran
  **first**: it unloaded `sl.*`/NGX while the render thread was still inside `dlss_evaluate`, and that thread's next
  Streamline call jumped into the unloaded plugin.
- Fix: no exit handler unloads the vendor SDKs. `dlss.cpp` keeps `dlss_shutdown` (and, on Linux, `dlss_shutdown_linux`,
  which had the same LIFO problem with the NGX component) but no longer registers it; the window-close path ends the
  process through `quick_exit_handler` + `plat_terminate()`, which skips the DLL teardown entirely — that is the reason
  the handler exists. A truly orderly SDK shutdown would need every renderer thread stopped first: after `slShutdown`
  the Streamline swapchain proxies the renderer holds are gone too, and the exit path cannot stop the game's threads.
- Tests: `tests/unit/test_vk_source_contracts.py` (`test_vendor_upscaler_sdks_are_not_unloaded_by_an_exit_handler`) pins
  the invariant (no `atexit(dlss_shutdown…)` in `dlss.cpp`; the `quick_exit_handler` note; `plat_terminate()`), and the
  runtime-only build (`-DMONADO_RUNTIME_ONLY=ON`) compiles the edited units. **Windows in-game verification pending**:
  exit the game 5–10 times with DLSS on and check `build/crash/` stays empty (`LARECOMP_VK_TRACE=1` logs the exit).
- Open leads from the same report (not changed here; Windows-only, unverifiable on this machine). The Streamline
  integration never calls the common plugin's `presentCommon()` (one warning per session: "internal bookkeeping and
  garbage collection will not run"), tags its resources without `sl::Resource::desc` (the SDK header says the
  description is mandatory on Vulkan) and hands the R8 reactive mask to `kBufferTypeBiasCurrentColorHint`. The session
  logs 425,692 `isFormatSupported Cannot have undefined format` errors (12 per frame, 64 MB in 10 minutes; the Streamline
  spam also buries the useful lines) and 64 `getResourceSize … format 0 native 9` (`native 9` = `VK_FORMAT_R8_UNORM`,
  the reactive mask). Worth fixing next: fill `desc` with the `VkImageCreateInfo` and tag the mask as
  `kBufferTypeReactiveMaskHint`, then re-run the same session and check the log says 0 errors.
- The report's first symptom (the Monado's ether/particles missing in in-engine cutscenes) still needs a save and a
  reference image; the cheapest A/Bs for the reporter are `cutscenes = 30` (or `XCDE_CUTSCENE_UNQUANTIZE=0`, the #181
  change shipped the day before), `occlusion = off`, a 16:9 window, `frame_gen = 0` and `XCDE_XFB_SHADOW=0`.

- Update (same evening, the reporter's own A/Bs): with FSR 3.1 and with the bilinear upscaler the Monado FX are
  still missing, and those two sessions exit **cleanly** — so the presentation stack (DLSS, FSR 3, frame
  generation, jitter/HUD snapshot) is out, and the exit crash reproduces only when DLSS is enabled, which is the
  control for the fix above. The triage note records both runs.

- The missing FX is the **#181 cutscene unquantize's third gate** (2026-11-23, found with the reporter's save): at more
  than 30 FPS the snap-track time bypass misses the exact 30 Hz key times and the cutscene track that enables the
  Monado's ether glow / ring lens never triggers. `XCDE_CUTSCENE_UNQUANTIZE=0` and a binary-patched build with only that
  gate requantized both restore it; occlusion was ruled out. Fix: the third bypass is removed from
  `games/xcde/patches.py` (the clock and camera gates keep the #181 smoothness); regeneration and the #181 re-measure
  are pending. Workaround meanwhile: `cutscenes = 30` in the launcher.

Triage of both symptoms, the minidump findings and the A/B table:
[summary.md](../evidence/2026-11-23-issue-216/summary.md).

## 2026-11-23 — launcher under non-ASCII install paths (GitHub #218)

- Symptom: on Windows, with the package under a folder like `C:\Users\João Santos\Monado`, the launcher starts
  nothing — "Could not start the game code build (error 267)", "curl.exe could not be started (error 267)" — and its
  settings file is not read (defaults silently).
- Cause: the launcher's paths are UTF-8 (`SDL_GetBasePath`, the environment), but they went to the ANSI Win32 calls
  (`CreateProcessA`'s `lpCurrentDirectory`, `CreateFileA`, `MoveFileExA`, `SetEnvironmentVariableA`,
  `FindFirstFileA`, `ShellExecuteA`) and to the CRT (`fopen`, `remove`, `stat`). Windows reads those bytes in the system
  code page: `João` (`C3 A3` in UTF-8) is `JoÃ£o` in CP1252, a folder that does not exist, so `CreateProcessA` failed
  with `ERROR_DIRECTORY` (267) before the child started.
- Fix: new `runtime/host/utf8_path.h` (UTF-8 → UTF-16, then `_wfopen`/`_wremove`/`_wrename`/`MoveFileExW`; plain libc
  elsewhere). `atomic_file.h` and `ini_writer.h` save through it, so the game's settings/caches get the same handling.
  The launcher starts all six children with `CreateProcessW` (wide executable, command line, working folder), opens its
  logs and progress files with the wrappers, reads/writes `PATH` and the Diagnostics environment as UTF-16, lists mods
  and builds the report with the wide APIs, opens folders with `ShellExecuteW`, and uses `SDL_GetPathInfo` instead of
  `stat`. `curl` writes to the relative `build/update_check.json` (its working folder is the root already) so a child
  that decodes argv in the system code page cannot mangle the path.
- Tests: `tests/native/test_utf8_paths.c` (Linux and Windows; wired into `scripts/native_tests_linux.sh`,
  `scripts/native_tests.sh` and `tools/diag/validate_port.py`) makes a `João-😀` folder, opens/renames/replaces/removes
  files there, round-trips the shared INI writer and — on Windows — starts `cmd.exe` in it with `CreateProcessA` (the
  reporter's 267) and with `CreateProcessW` (must run). `tests/unit/test_launcher_utf8_paths.py` pins the source
  contracts (no ANSI Win32 call left, wide children, UTF-8 file helpers, shared writers).
- Verified under Wine 11 (Proton-CachyOS, code page 1252) with the real Windows launcher built by zig + the project's
  ucrt64 SDK: the fixed build writes `build/launcher.log` and starts stand-ins for `curl.exe` and
  `monado-gamebuild.exe` under `Z:\…\João-😀\…`; the same test with `launcher.cpp` from `HEAD` starts neither and
  writes no log (`CreateProcessA` 267). Route, outputs and limits:
  [summary.md](../evidence/2026-11-23-issue-218/summary.md).
- Still open: the game's own paths are ANSI on Windows (`plat_exe_path` = `GetModuleFileNameA`, `getcwd`, `stat`,
  `fopen` in `runtime/paths.c` and the ~90 file calls elsewhere), so a package under a non-ASCII folder is expected to
  stop at the game's data-root check. Tracked as `T-UTF8-RUNTIME`; a run of the shipped package under
  `C:\Users\João\…` is still worth doing. The Windows CI job (windows-latest) built the launcher with the ucrt64
  SDK and ran `tests/native/test_utf8_paths.c` green (run 45093578036: `test_utf8_paths_build: PASS`,
  `test_utf8_paths: PASS`).

## 2026-11-23 — the game under non-ASCII paths (GitHub #218, second half)

- Measured, not assumed: with the #220 launcher fix the game could start under `Z:\…\João-😀\…`, but stopped at
  `[fs] no romfs folder at targets/romfs` → `FATAL: cannot open targets/romfs.bin`. mingw's `opendir()` makes the path
  absolute with `_tfullpath` → `GetFullPathNameA` (ANSI, MAX_PATH); in a non-ASCII working folder the search runs in
  "JoÃ£o" and returns no entries, while relative `fopen`/`fstat`/`read` (resolved by the OS against the wide working
  folder) kept working. Proof: a relative folder with 2 files listed **0 entries** from inside `João-😀`, 5 from an
  ASCII folder, and `FindFirstFileW("targets\\romfs\\*")` works in the João folder.
- Fix: `runtime/platform/dir_utf8.h` (new) — `xcde_opendir/readdir/closedir/telldir/seekdir` over
  `FindFirstFileW`, names as UTF-8, `d_type` from the Windows attributes; the whole folder is read at open (the game
  asks for an entry count before reading). `services/fs.c`, `services/romfs_virtual.c`, `nvn/vulkan/precompile.c` and
  `paths.c` use it; on Linux it is `opendir`/`readdir`.
- The other absolute-path sites: `runtime/host/utf8_path.h` grew `_wopen`, `wstat`, `_wmkdir`, `_wrmdir`, `_wchdir`,
  `_wgetcwd`, `GetEnvironmentVariableW` and `LoadLibraryW` wrappers. `runtime/loader.c`, `services/fs.c`,
  `services/romfs_fd_cache.h` and `services/romfs_virtual.c` open, stat and read through them (a `--game <absolute
  dump>` path with accents works), `gamelib.c` loads the game library with `LoadLibraryW` and reads `LARECOMP_*`
  paths as UTF-8, `settings_ini.c` reads `XCDE_INI`/`xcde.ini` the same way, `fsr4.c` loads APS components and model
  paths the same way, and `platform/windows.c` gained `plat_argv_utf8` (argv parsed from the wide command line) and
  writes `lasrecomp.log` next to the exe with `GetModuleFileNameW`/`_wfreopen`.
- T135 relaunch (`runtime/host/main_boot.c`) and the crash minidump helper (`runtime/diag/crashdump.c`) start their
  child with `CreateProcessW` and the wide exe/command line; `enter_install_dir` uses `_wchdir`. `XCDE_TEST_RESERVE_FAIL=1`
  forces the relaunch once (the collision is random) so the route can be re-tested.
- Tests: `tests/native/test_utf8_paths.c` now also chdirs into the `João-😀` folder, enumerates `.` through the shim
  and (on Windows) reports the CRT's 0 entries next to it, and reads a UTF-16 environment value; new
  `tests/unit/test_runtime_utf8_paths.py` pins the source contracts.
- Verified with the Wine test bed (`docs/evidence/2026-11-23-issue-218/summary.md`): the game boots and renders
  thousands of frames from `João-😀\Built` with a relative `targets/` (the run after the last change: 2,055 frames at
  30 fps); `XCDE_TEST_RESERVE_FAIL=1` relaunches into the game; `XCDE_TEST_CRASH_AT=150` writes
  `crash/crash_*.txt` **and a 296 KB `.dmp`** (minidump helper exit 0); a detached launch (no standard handles) writes
  `build/lasrecomp.log` in the João folder; `--game` with the absolute `João-😀\…\targets` path loads the game and
  renders (3,463 frames). Linux native tests and CI are green; the Windows CI job compiles the runtime and runs the
  native test on windows-latest.
- Review follow-ups (same PR): `xcde_opendir` sets `errno` like the CRT it replaces (`ENOENT`/`EACCES`/`ENOTDIR`/
  `ENOMEM`/`EIO`, and `services/fs.c`'s `OpenDirectory` reports it to the game) and gives up on a folder whose
  enumeration fails mid-way; the precompiler's own `stat`/`fopen`, `XCDE_PRECOMPILE_LIST` and `XCDE_VARIANTS_SEED`
  use the UTF-8 wrappers (an absolute non-ASCII `--precompile` root works); `plat_argv_utf8` keeps the complete CRT
  argv if one argument cannot be converted instead of dropping it; the save-tree `mkdir`/`rmdir`/`rename`/`remove`
  calls and the path-valued diagnostic variables (`XCDE_DUMP_MEM`/`_EACH`, `XCDE_DUMP_HEAP`, `XCDE_FRAME_HASH`,
  `XCDE_INPUT_REPLAY`/`RECORD`, `XCDE_USAGE`, `XCDE_HLE_COVERAGE`, `XCDE_SAMPLE_OUT`, `XCDE_TOUR`) use them too.
- Leftovers (none on the start/play path): `services/kbicons.c` still opens its cache / mod icon files with the
  narrow CRT (relative ASCII names in practice), `platform/win_compat.h`'s `realpath` shim (`_fullpath`) is not used
  by the game, and developer-only debug switches that take a path (`XCDE_GLSL_DIR`, `XCDE_REC_OUT`) are still narrow.
  The reported install, dump and save paths are covered.

## RecompLauncher support (T279, 2026-11-23)

- `recomplauncher.json`: the player's copy is the named input `game` (the top-level `game` field and `{game}` are
  deprecated in RecompLauncher's format); one file covers the Windows zip and the Linux AppImage (`start.program` and
  `prepare.program` list both, with one argument list); `prepare.timeout` (not in the schema) is gone, `prepare.done`
  is `gamebuild/build.json`; `system` (Windows 10, glibc 2.39, Vulkan 1.2) and `exit_codes` (2: no copy) added.
  Validated against https://recomplauncher.com/schema/recomplauncher.json (the old file failed it on
  `prepare.timeout`, and release packages added `prepare.version`).
- `tools/package/make_portable.py` copies the manifest unchanged (RecompLauncher runs the setup step again after
  every new release; the builder's fingerprint makes that a no-op when nothing changed).
  `tools/package/make_appimage.py` puts it at the AppDir root, where RecompLauncher reads it from the AppImage.
- `packaging/linux/AppRun`: `--game <dump>` plays without the `targets` link, `--prepare` runs the bundled builder.
  RecompLauncher passes `MONADO_DATA_ROOT={install}`, so the Linux install keeps the Windows layout.
- `tools/gamebuild/gamebuild.py`: `--prepare` is accepted (and ignored), and with `RECOMPLAUNCHER=1` every progress
  update is also printed as `progress <0..1> <text>`; "Already built" when the build is up to date.
- `runtime/nvn/vulkan/device.c`: `RECOMPLAUNCHER_MODE=bigpicture` opens the window fullscreen without changing the
  saved setting. `tools/launcher/launcher.cpp`: no update check at start under RecompLauncher.
- Shaders in the setup step: `prepare.options` has a "Prepare the shaders now" tick (on by default) that adds
  `--precompile`. `tools/gamebuild/gamebuild.py` then runs `<game> --precompile <dump>` from the data root after the
  game code (built or already up to date), with the game from `MONADO_GAME_EXE` (AppRun) or `build/xcde.exe` beside
  the Windows builder. The bar is 70% build, 30% shaders; a precompile whose `cache/precompile_done.txt` stamp matches the
  executable is skipped (the launcher's own check), and a failed one is logged in `build/last_precompile.log` without
  failing the setup. A frozen builder gives the game back the original `LD_LIBRARY_PATH` and resets its Windows DLL
  folder first.
- Tests: `tests/unit/test_recomplauncher_support.py`, `tests/unit/test_make_portable.py`; the unit suite, the package
  guard and the documentation/path checks pass on Linux. Not done here: a full setup and Play from RecompLauncher
  (needs a dump), and a Windows build of the two C/C++ edits (CI compiles them).

## .nup packages as the dump (T280, 2026-11-23)

- **Constitution art. II.1 changed** (needs a changelog entry with the reason): from "only exefs/ and romfs/ already
  extracted" to "already decrypted: extracted to folders, or in a `.nup` package that stores them uncompressed, read
  in place and never converted". Reason: a `.nup` is a plain, already-decrypted container of the same exefs and
  romfs (RecompLauncher reads it by build ID without keys), so accepting it keeps the article's intent (the player's
  own dump, no keys, nothing decrypted by Monado) while sparing players an extraction step and 15 GB of loose files.
  Encrypted containers (`.nsp`, `.xci`, `.nca`) are still refused.
- `runtime/services/game_files.c` and its header (new): the dump as a folder or a package; `gf_init`, `gf_source`/`gf_read`/
  `gf_pread`/`gf_open` for a game path, and the package tree (`gf_pkg_*`) for listings. A compressed or incomplete
  package is refused with the reason.
- Readers moved onto it: `runtime/loader.c` (NSOs), `runtime/host/main_boot.c` (`gf_init`, `main.npdm`),
  `runtime/services/romfs_virtual.c` (`scan_package`, files read at their offset in the package),
  `runtime/services/fs.c`, `runtime/hle/movie.c` (FFmpeg custom I/O), `runtime/hle/quickstep_native.c`,
  `runtime/services/kbicons.c`, `runtime/nvn/vulkan/precompile.c` (`list_package`). A folder dump reads exactly as
  before.
- Builds: `services/game_files` added to CMake, `scripts/build_{boot,linux,rt_gfx}.sh`, `scripts/link_{boot,gfx}.sh`
  and `tools/diag/validate_port.py`.
- Tools: `tools/extract/nup.py` (new), `check_dump.py` (a package's exefs and romfs manifest), `manifest.py`
  (`manifest_of`), `gamebuild.py` (`exefs_folder`, a fingerprint by module size and build ID, exit code 2 for a
  package that can't be used), the frozen builder bundles `nup`.
- AppRun `--game` takes a `.nup`; `recomplauncher.json` offers a folder or a `.nup` file.
- Tests: `tests/native/test_nup_package.c` (in `scripts/native_tests_linux.sh`), `tests/unit/test_nup_package.py`;
  `test_runtime_utf8_paths.py` follows the reads into `game_files.c`. Checked on a real XCDE package (base game):
  parsed, 18,544 files, refused by build ID. Not done: the Windows native test (the Windows `pread` lives in
  `runtime/platform/windows.c`).
- 64-bit offsets on Windows: the Windows build has a 32-bit `off_t` and its stat size is 32-bit (`_wstat64i32`), so
  the first real run did not see a 15 GB package as one (no shader files found) and reads past 2 GB would have been
  cut. `gf_pread_at` (ReadFile with a 64-bit OVERLAPPED offset; `pread` elsewhere) and `GetFileSizeEx` replace them for
  package reads, its size and its detection; `romfs_virtual.c` and `movie.c` read through it.
- Played from a real 2.0.0 package (14.6 GB, base game plus the v262144 update) on Windows with the release runtime
  (RTX 5080): the setup step built the game code (126 s) and precompiled 9,823 programs from the package's 8,124
  shader files (0 failed); the game reached the title screen at 30 FPS. A cutscene from a package was not reached.

## Shader precompile speed and progress (T281, 2026-11-23)

- Why it was slow (measured on the low-end dev PC: i7-3770K, GTX 1050 Ti, 16 GB; an LD_PRELOAD timer around
  `shaderc_compile_into_spv`, `vkCreateGraphicsPipelines` and `uncompress` on an unmodified build, plus a 312-file
  subset of the dump): the cost is the CPU compilers. A fragment shader costs 82 ms of shaderc on average (29 ms
  median, p90 273 ms) because fragments run spirv-opt by default – 7.5x a vertex shader (5.3 ms) and ~80 % of the
  per-stage CPU; NVIDIA's own compile of each new module adds 27–55 ms. Maxwell → GLSL translation itself is trivial
  (0.2–1 ms per stage) but was serialized behind one global lock; with warm caches that lock was the whole critical
  path. The scan reads 8.1 GB and inflates 14.0 GB, and the 25 >64 MB map/model files were processed last and one at
  a time: a ~29 s serial tail of the 52 s phase.
- `runtime/nvn/maxwell/translate.c` + `shader.h`: the translator is re-entrant now – the `RSC2`/`img_coord` scratch
  rings and the guest-heap range (`maxwell_gmem_base/size`) are thread-local. `runtime/nvn/vulkan/exec.c`:
  `xlat_lock`/`xlat_unlock` skip the global lock for threads running their own precompile jobs (`tl_inline_jobs`); the
  game's single translator thread still takes it.
- `runtime/nvn/vulkan/precompile.c`: the offline precompile uses all logical CPUs but one (was the in-game
  `xcde_shader_workers`, cpus-4 = 4 here), with only 4 build workers during the scan and the rest joining after it
  (7 workers starved the zlib inflate: 32.4 s vs 21.8 s scan); the scan sorts the biggest files first and allows up
  to N >64 MB files at once (N by RAM: 4 at 16 GB, 2 at 8 GB) instead of the serial tail; progress reports the four
  passes (scan, build, seed, pipelines) weighted by the previous run's measured pass times
  (`cache/precompile_times.txt`), so the launcher's bar moves through the seed and pipeline passes and can show the
  time left (it used to reach 100 % at the end of the build, stand still, then jump back for the pipelines pass); the
  seed pass skips the jobs the program pass already built (465 of 1,170 on the subset; 5,484 of 9,045 for the whole
  game); jobs on the crash skip list are reported as "skipped", not "failed".
- `tools/launcher/launcher.cpp`: reads the new progress format and shows "pass N of 4 · name · done/total · ~min
  left"; `pc_stop` deletes `cache/precompile_inflight.bin` because a stop the user asked for is not a driver crash
  (the next run used to move those in-flight jobs to the permanent skip list – the 92 entries in the dev cache were
  ~23 cancels × 4 workers); the explicit "Precompile shaders" button clears the skip list and a stale in-flight
  record so the failed jobs are retried. `tools/gamebuild/gamebuild.py` reads the same progress format.
- Measured, old vs new build back to back: warm rerun 8.37 → 1.55 s (5.4x), warm with driver libraries and whole
  pipelines 24.8 → 3.6 s (6.9x), cold SPIR-V 32.1 → 27.8 s (-13 %, shaderc-bound with 7 threads on 4 cores), full-dump
  scan 40.6 → 21.1 s (-48 %). The SPIR-V cache after a cold subset run is identical old vs new (2,133 entries, same
  bytes): the parallel translation and the seed dedupe change no output.
- Tests: `tests/unit/test_recomplauncher_support.py` (the new parser, the cancel/skip-list source contracts), the
  full unit suite (196) and `scripts/native_tests_linux.sh`. Left open: the fragment-spirv-opt default needs an
  in-game FPS A/B (86 % of a fragment compile is spirv-opt); a build-time fingerprint of the translator inputs to
  skip the rerun after builds that cannot change any shader; and the whole-pipeline pass policy on low-end PCs.

## 2026-11-24 — incremental shader precompile (T282)

- The user asked to "do incremental precompile": after T281 a rebuild that cannot change the shaders still precompiled
  everything again, because `cache/precompile_done.txt` named the executable's size and time (T211). Measured on the
  low-end PC, full dump, warm cache: a full re-run took 367.8 s (scan 32.7, build 316.3, seed 1.0, pipelines 16.6) and
  393.3 s back to back - every time the game executable changed for any reason.
- `tools/build/shader_sig.py` (new) fingerprints the sources that decide the precompiled artifacts: the Maxwell-to-GLSL
  translator (`runtime/nvn/maxwell/`, recursively), the GLSL-to-SPIR-V compiler (`runtime/nvn/vulkan/shaders.c`, `shaders.h`, `vk.h`),
  the precompile's own program/key guesses (`runtime/nvn/vulkan/precompile.c`), the shader content hash it duplicates
  (`runtime/nvn/objects.c`) and the runtime acceptance of the precompiled SPIR-V (`runtime/cache_validation.h`). CMake
  generates `build/inc/shader_sig.h` from it with a custom command (only `precompile_sig.c` includes it, so a
  translator change recompiles that one unit); both shell build scripts generate it, and `tools/diag/validate_port.py`
  compiles the unit with `-DXCDE_SHADER_SIG="validation"` instead (it never generates the header). exec.c's build path
  is covered by `runtime/nvn/vulkan/precompile_rev.h` (`XCDE_PRECOMPILE_ARTIFACTS_REV`), which is mixed into the
  signature.
- `runtime/nvn/vulkan/precompile_sig.c` (new; native test `tests/native/test_precompile_sig.c`) computes the run
  signature: the build fingerprint, the artifacts revision, the environment switches (presence and value: the SPIR-V
  optimisation `LARECOMP_VK_OPT`/`XCDE_FS_OPT`, the translator switches that change the emitted GLSL and the
  precompile's `XCDE_PRECOMPILE_LIBS`/`PIPELINES` pass policy), every file under `<dump>/romfs` by relative name, size
  and mtime (for a `.nup` package: its 64-bit size and mtime - Windows' struct stat is 32-bit, the T280 trap) and the
  shipped variant seed. A root must be a dump folder with a readable `romfs/` or a `.nup` package file; one unreadable
  file inside romfs is skipped like the shader scan skips it. Deliberately not covered: `cache/variants.bin` (it grows
  while playing and must not invalidate the cache) and the graphics driver, including the Vulkan route / selected
  adapter from xcde.ini (after a driver update, a GPU switch or a route change press "Precompile shaders").
- `runtime/nvn/vulkan/precompile.c`: the done stamp gained a `sig <hex>` line and lost the `exe <size> <mtime>` one;
  new `--precompile-status <dump>` mode (exit 0 current, 1 needs preparing, 2 game files unreadable) that touches no
  cache, no scan and no Vulkan device; `--precompile` exits at once when the stamp's signature matches the sources
  (`XCDE_PRECOMPILE_FORCE=1` runs the passes anyway). `runtime/host/main_boot.c` dispatches the new mode before any
  platform/GPU init.
- `tools/launcher/launcher.cpp`: `pc_done()` runs `<game> --precompile-status targets` (and writes its one line to
  `build/last_precompile_status.log`) instead of parsing the stamp; the answer is memoized because the UI asks every
  frame, and dropped when the builder creates the executable, a precompile ends, another dump is selected, the settings
  or the environment rows are edited, or the executable itself changed (it asks again; the runtime then decides by
  fingerprint, so an unrelated rebuild still keeps the cache). The status child gets the same `xcde_env.ini` environment
  as the precompile (`apply_env()`), because those options are signature inputs, and it is asked before the window is
  shown. The explicit "Precompile shaders" button passes `XCDE_PRECOMPILE_FORCE=1` to its child instead of deleting the
  done stamp (a spawn that fails cannot lose a valid cache; the runtime clears the crash/in-flight lists itself), and
  exit code 2 ("game files unreadable") does not start a doomed run. `tools/gamebuild/gamebuild.py`:
  `precompile_status` runs the same command from the data root with a timeout and keeps 0/1/2 apart before
  RecompLauncher's setup step.
- The measurements also exposed an older reason slow reruns never got faster: the cache never converged (consecutive
  full runs compiled 19,878 / 7,685 / 6,959 SPIR-V stages). The found shaders' named-input masks
  (`found[].int_in`/`named`) were never initialized after the `realloc`: every container that does not name its inputs
  left heap garbage there, so the build made random integer-input variants of those shaders (17,251-19,610 "named
  variants" per run; the real count is 87). And since T281 the build workers started during the scan, copying the
  masks while the scanners were still merging them. Both fixed: the fields are initialized when a shader is added and
  the build workers start after the scan. A forced warm full run is now 54.8 s / 57.6 s (scan 20.2/21.8, build
  13.3/14.2, seed 1.0/1.2, pipelines 19.3/19.7) with 8 and then 0 SPIR-V compiled - deterministic.
- Measured after the fix: `--precompile-status` on the full dump 78-108 ms (18,544-file walk, including process start);
  `--precompile` with a current cache 87/95 ms ("nothing to do") instead of a 55-393 s run; touching the executable (a
  rebuild that changes no shader source) keeps the cache current; `XCDE_FS_OPT=0` and a changed game-file mtime report
  "needs preparing" in ~50 ms; the subset functional runs (312 files, 1,464 programs) show the same: cold 1,464
  programs in 41.6 s (2,301 SPIR-V compiled), then status 57 ms, early exit 52 ms, forced warm re-run 1.9 s (0 SPIR-V
  compiled). Two forced full runs after the fix compiled 8 and then 0 stages: the SPIR-V cache is deterministic now.
  The game booted to the title with the new binary at 30 FPS, 619 frames, 0 compile failures.
- Tests: `tests/unit/test_precompile_incremental.py` (fingerprint and the runtime/launcher/builder/build contracts),
  `tests/unit/test_recomplauncher_support.py` (the builder asks the status and does not start a current precompile),
  `tests/unit/test_launcher_utf8_paths.py` (the new wide-API child process), the native suite (the new signature test),
  `python -m unittest discover -s tests/unit`, `tools/check_docs.py`, `tools/check_paths.py`, `package_guard.py --git`
  and a runtime-only CMake configure that builds the generated header and the new unit.
- Not verified here: the Windows build (the new code uses the existing wide-API patterns; CMake, both shell scripts and
  `validate_port.py` were updated) and a real RecompLauncher setup run.

## 2026-11-24 — PR review fixes (T281/T282)

Review follow-ups on the two commits above (the Windows runtime is still not executed here):

- Precompile: clamp `nscanners` to the 64-entry thread array (a >128-logical-CPU Linux host overflowed the stack; the
  CPUs count is not bounded); bound the >64 MB scan budget by bytes (`RAM/8`) as well as by file count; require
  `cache/spirv.idx` for "current" (a truncated pack alone must not claim the cache is there); remove the done stamp only
  after `gf_init` validated the root; `XCDE_PRECOMPILE_FORCE=1` now also clears the crash skip list and the in-flight
  record, so the launcher button can force a retry without deleting anything itself; `job_built()` is recorded only for
  variants that actually built (the seed pass no longer skips a failed variant); an exact `sig ` line match; a
  `--precompile` with a plain file as root exits 2 and leaves the stamp alone (the signature validates the root).
- Signature: 64-bit root metadata on Windows through the wide API (the T280 `.nup` case: a 15 GB package through
  `_wstat64i32`); environment switches are mixed by presence *and* value (a set-but-empty `XCDE_FS_OPT=` is not unset);
  the translator's GLSL switches and the precompile's pass switches are covered; `runtime/nvn/objects.c` and
  `runtime/cache_validation.h` join the build fingerprint, the translator folder is hashed recursively, and
  `precompile_rev.h` covers exec.c's build path; an unreadable entry inside romfs is skipped like the scan skips it, a
  directory recursion cap turns a symlink loop into "unreadable", and a folder without `romfs/` or a package without
  the `NUP` magic is "unreadable", not "stale".
- Translator re-entrancy: the remaining function-scope statics reachable from `maxwell_translate` (the lazy `getenv`
  caches and the two diagnostic counters) are `_Thread_local`; `g_wide_box_possible` is `_Atomic` (the render thread
  writes it on a resize while precompile workers read it).
- Build/launcher: a selective `scripts/build_rt_gfx.sh <unit>` rebuild always regenerates the fingerprint header and
  recompiles `precompile_sig` (a translator edit + relink could otherwise carry a stale fingerprint and skip the
  re-precompile); `validate_port.py` builds `test_precompile_sig` once (it was listed twice) and warning-strict; the
  status probe is skipped while a precompile runs, answered before the window is shown, and `--shot … precompile` forces
  a run so the screenshot still shows the bar; the Windows `pc_stop` retries the in-flight delete (TerminateProcess is
  asynchronous) and the memo is dropped when a stop happens.
- Tests: native signature test now builds a unique temp root (a killed run + PID reuse cannot wedge it), checks the
  isolated removal, a same-size mtime-only edit, a plain-file root, a folder without romfs, set-empty vs unset options,
  a translator switch and a pass switch; new `tests/native/test_translate_threads.c` translates the two scratch-ring
  users on 8 threads and compares every GLSL source with the serial output (Linux native suite and Windows
  validate_port); unit tests updated for the force/memo/exit-code contracts and the new fingerprint inputs.
  `scripts/ci.sh`: 220 unit tests (10 skipped); native suite and a runtime-only CMake build of the changed units pass.

## 2026-11-24 — post-merge PR #231 review fixes

- Windows cache validation now uses a 64-bit file-size query, so a valid `cache/spirv.pack` larger than 2 GiB does not force a
  full precompile after every build.
- The translator's remaining lazy diagnostics caches are thread-local. The parallel translation test now holds workers at a
  start gate, compares their concurrent first translations with a serial baseline generated only after they join, and checks
  thread-creation failures.
- A precompile writes a current signature only after the scan and all build passes complete without unexpected failures.
  Source reads and xbc1 decompression share RAM-bounded admission budgets; scanner count is RAM-aware and every worker phase
  joins only threads that started.
- A forced rerun validates the root and invalidates the old stamp before deleting crash-retry records, and performs that
  deletion only after Vulkan initialization succeeds. Loose-SPIR-V mode has its own completion marker; a persistence-disabled
  run cannot stamp the cache current.
- The build fingerprint now includes the game-file reader and the GLSL replacement headers (bloom and GTAO). The runtime
  signature includes the corresponding replacement/patch switches, GPL/layout policy and SPIR-V cache-mode options; the
  artifacts revision was bumped for the GTAO change already on main.
- The launcher no longer reports an unknown status probe as prepared, retries verification explicitly, and restores/unsets
  Diagnostics environment rows when they are removed. RecompLauncher's builder skips the unbounded full run after a failed
  status probe and treats a full-run spawn failure as non-fatal.
- Validation: 242 unit tests (10 skipped), the Linux native suite, focused Clang/GCC signature tests, a cold-start translator
  TSAN run, targeted Clang compilation of `precompile.c`, and ASan/UBSan cache-mode/scanner harnesses (including a sparse
  3 GiB pack) pass. The Windows branch was source-reviewed but not executed locally.

## 2026-11-24 — PR #247 review follow-up

- Only required scan/base-program failures block a current stamp. Extra/named guesses, seed records and whole pipelines
  remain best-effort; their failures are reported without forcing a full rerun on every launch. `n_built` counts base
  programs again, and incompatible variant-record sizes are skipped before creating jobs.
- `--precompile-status` returns 3 when SPIR-V persistence is disabled. The launcher and RecompLauncher builder show that
  precompilation does not apply and skip the repeated full pass; the explicit Precompile button is disabled and
  `pc_start` refuses to spawn the same no-stamp run.
- The scan budget has a separate large-file slot counter, so small files no longer delay a `>64 MiB` container. Unreadable
  shader containers and directories make the signature/scan incomplete instead of being silently skipped. Only entries
  proven irrelevant are skipped (a known non-directory type and `ENOENT`/`ENOTDIR`, e.g. a dangling mod file); `EACCES`,
  other errors and `DT_UNKNOWN` stay fatal so an inaccessible subtree cannot be missed.
- A shared `xcde_utf8_file_info()` helper gives the game-file reader, signature walk, precompile listing and cache validator
  64-bit metadata on Windows. The native UTF-8 path test now creates a sparse 3 GiB file and verifies its reported size.
- `XCDE_GLSL_DIR` hashes the override `.glsl` bytes as well as the directory path, so in-place debug-shader edits invalidate
  the precompile signature. exec.c now accepts an override only when it is a regular file and seek/tell/read succeed (a
  directory can be `fopen`ed on Linux and used to look like an empty shader); non-regular or unreadable entries are
  skipped with a warning, and regular files of any size are hashed.
- POSIX `xcde_opendir` now checks `readdir`'s error state, so a partial directory listing cannot make the signature or the
  precompile scan look complete. The signature walk and precompile listing share one path limit.
- Diagnostics environment edits invalidate the memoized precompile status. The explicit Precompile button checks the
  applied environment directly for `LARECOMP_VK_NO_SPIRV_CACHE`, so it needs no synchronous status walk and re-enables as
  soon as the variable is removed.
- A precompile run that finds no shader files but recorded scan failures (a missing/corrupt package `romfs`, for example)
  now exits 2 as unreadable instead of 1 as stale, so the launcher and builder do not retry a doomed run.
- Variant records are validated before they can become jobs and again as the last line of defence: `ncolor` is limited to
  the eight fixed colour slots, and sample counts, attribute bindings/streams, topology, logic-op and the shader-key
  fields are range-checked. A correctly sized but corrupt `cache/variants.bin` or seed can no longer reach
  `pipeline_whole()`'s `cba[8]`/`write_mask[8]`; the game's own prewarm uses the same validator.
- The effective `xcde.ini` GPU pipeline policy (`gpu_profile` compatibility, `gpu_gpl` on/off) is loaded before
  `LARECOMP_HIDDEN` makes `settings_load()` ignore the file, and is part of the cache signature. The precompile now warms
  the same libraries/whole pipelines the game will ask for, and changing the policy invalidates the cache. PLAY and the
  explicit precompile save pending settings before the status probe/child starts: the probe and the child read
  `xcde.ini`, so without the save they could approve (or warm) the previous policy while the game starts with the new one.
- Diagnostics environment edits and "Clear shader cache" also drop the footer's previous status line. The clear button
  is disabled while a precompile or game-code build is running, so it cannot delete cache files underneath it, and it
  removes the loose `cache/spirv/*.spv` files too (previously the non-empty directory was left in place).
- A status probe that takes a second or more is no longer retried automatically from the launcher's render path; the
  retry deadline is measured after the probe, and only PLAY or another explicit action retries a slow UNKNOWN.
- "Clear shader cache" removes `cache/precompile_done.txt` and `cache/precompile_loose_done.txt` (the old code removed the
  never-written `cache/precompile_sig.txt`) and drops the memoized status, so the next frame cannot keep reporting CURRENT.
- Validation: 258 unit tests, the Linux native suite, focused Clang/GCC signature and sparse-file tests,
  targeted Clang syntax checks for `precompile.c`/`exec.c`, and the Linux launcher syntax check pass. Windows execution is
  still pending; the sparse-file test is included in the Windows validation suite.

## 2026-11-26 — save writes cannot silently tear (T037)

- `runtime/services/fs.c` now validates `IFile.Write` offset/size for every file and keeps writing through short
  `pwrite()` results (retrying `EINTR`); a zero-length write or any remaining error is reported to the guest. `Flush`
  reports `fsync()` failures, `fsync_tree()` propagates the first stat/open/flush error, and `Commit` keeps the journal
  and stays dirty when the flush fails. A journal that cannot be created also refuses the change instead of writing an
  unprotected save. `copy_tree()` treats read errors and partial writes as failures.
- Added `tests/native/test_fs_commit.c` (short writes, ENOSPC, failed fsync, journal kept on a failed commit), wired into
  `scripts/native_tests_linux.sh`, `scripts/native_tests.sh` and the Windows validation suite.
