# Development changelog

New developer documentation is written in English. This file is the place for session prose that would otherwise be
appended to [`tasks.md`](tasks.md): what was done, what was measured and what is left, one section per work session.
Keep the ledger in `tasks.md` (task id + status) and the runtime edit log in [`../UPSTREAM.md`](../UPSTREAM.md)
(article X) separate from this narrative.

Older session notes live in the archived handoffs under [`archive/`](archive/) and in the historical session sections
of `tasks.md`; do not move them, they are the record.

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
  `tools/package/make_appimage.py` puts it at a release AppDir's root, where RecompLauncher reads it from the AppImage.
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

## RecompLauncher setup review fixes (T279, 2026-11-23)

- `tools/package/make_appimage.py`: `recomplauncher.json` is copied only for `--release` images, inside the same
  branch that copies the first-start builder. A personal image (game code, no builder) no longer advertises an Easy
  Setup step whose `AppRun --prepare` has nothing to run.
- `packaging/linux/AppRun`: `--prepare` checks that the builder is present and executable before `exec`; otherwise it
  prints why and exits 126 instead of the shell's raw 126/127. `recomplauncher.json` maps 126/127 to the same message.
- `tools/gamebuild/gamebuild.py`: on a rebuild, the `prepare.done` stamp (`build.json`) is unlinked after the
  up-to-date check and before `validate`/build, while its unit signatures are kept in memory and passed to
  `compile_units`, so objects are still reused. A failed or interrupted rebuild therefore never looks finished, and a
  successful one writes the stamp last as before. The up-to-date stamp read is the only operation in the
  `try/except`; both precompile call sites go through `precompile_after_build`, which turns an `OSError`/`ValueError`
  into the "Shaders compile while you play" fallback line and a diagnostic instead of failing the setup.
- `precompile_step` reads the game's `done` line as 1.0 (the bar used to move 1.0 → 0.94 when the poll caught it) and
  labels `seed` without moving the bar; `pipelines` then carries it to 1.0.
- Tests: `tests/unit/test_recomplauncher_support.py` (`--prepare` 126; failed rebuild leaves no `build.json`;
  up-to-date and end-of-build precompile `OSError` still exit 0; previous unit signatures reach `compile_units`;
  `done`/`seed` lines), the new `tests/unit/test_make_appimage.py` (a personal tree has no manifest or builder, a
  release tree has both) and `tests/unit/test_make_portable.py` (the mapped builder exit codes).
- Verified on Linux: 179 unit tests pass, 10 skipped; `package_guard.py --git`, `check_docs.py`, `check_paths.py`,
  `sh -n packaging/linux/AppRun`; the manifest validates against the live schema with `ajv-cli` (draft-07,
  non-strict). Not run here: a full RecompLauncher setup/Play (needs a dump) and the Windows package.
