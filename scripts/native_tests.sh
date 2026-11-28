#!/usr/bin/env bash
# Native (C) runtime tests, Windows host: fibers (T032b) and condvar timeout semantics (T031). No game data needed.
set -e
cd "$(dirname "$0")/.."
M=/c/msys64/ucrt64
Z=".venv/Scripts/python -m ziglang cc -I$M/include"
mkdir -p build/test build/rt
$Z -c runtime/platform/windows.c -o build/rt/platform_windows.o -O1 -w -Iruntime
$Z -O1 -w -Iruntime -o build/test/test_fibers.exe tests/native/test_fibers.c runtime/hle/fibers.c
$Z -O1 -w -Iruntime -o build/test/test_condvar.exe tests/native/test_condvar.c runtime/ipc.c build/rt/platform_windows.o runtime/log.c runtime/diag/diag.c runtime/diag/sampler.c $M/lib/libwinpthread.dll.a -lwinmm -ldbghelp
build/test/test_fibers.exe
build/test/test_condvar.exe
# [XCDE] T275: the timestamped log prefix (one per line, partial lines continue; timestamped unit test).
$Z -std=gnu11 -O1 -w -Iruntime -o build/test/test_log_timestamp.exe tests/native/test_log_timestamp.c runtime/log.c
build/test/test_log_timestamp.exe
$Z -O1 -w -Iruntime/nvn/maxwell -o build/test/test_translate_tex.exe tests/native/test_translate_tex.c runtime/nvn/maxwell/translate.c
build/test/test_translate_tex.exe
$Z -O1 -w -Iruntime -o build/test/test_movie_buf.exe tests/native/test_movie_buf.c
build/test/test_movie_buf.exe
$Z -O1 -w -Iruntime/nvn/maxwell -o build/test/test_translate_ldc.exe tests/native/test_translate_ldc.c runtime/nvn/maxwell/translate.c
build/test/test_translate_ldc.exe
$Z -O1 -w -Iruntime/nvn/maxwell -o build/test/test_translate_xfb.exe tests/native/test_translate_xfb.c runtime/nvn/maxwell/translate.c
build/test/test_translate_xfb.exe
$Z -O1 -w -Iruntime/nvn -o build/test/test_tegra_block_height.exe tests/native/test_tegra_block_height.c runtime/nvn/tegra.c runtime/nvn/formats.c $M/lib/libwinpthread.dll.a
build/test/test_tegra_block_height.exe
$Z -O1 -w -Iruntime/nvn -o build/test/test_tegra_copy.exe tests/native/test_tegra_copy.c runtime/nvn/tegra.c runtime/nvn/formats.c $M/lib/libwinpthread.dll.a
build/test/test_tegra_copy.exe
$Z -O1 -w -o build/test/test_time_math.exe tests/native/test_time_math.c
build/test/test_time_math.exe
$Z -O1 -w -fno-strict-aliasing -Iruntime -o build/test/test_host_fma.exe tests/native/test_host_fma.c  # [XCDE] FMA3 vs library fma
build/test/test_host_fma.exe
$Z -std=c11 -Wall -Wextra -Werror -o build/test/test_port_core.exe tests/native/test_port_core.c
build/test/test_port_core.exe build/test/port-core.ini
$Z -std=c11 -Wall -Wextra -Werror -o build/test/test_gpu_probe.exe tests/native/test_gpu_probe.c
build/test/test_gpu_probe.exe
$Z -std=c11 -Wall -Wextra -Werror -o build/test/test_settings_roundtrip.exe tests/native/test_settings_roundtrip.c runtime/host/settings_ini.c runtime/hle/cheats_options.c runtime/hle/graphics.c runtime/hle/foliage_distance.c
build/test/test_settings_roundtrip.exe build/test/settings-roundtrip.ini
# [XCDE] window size presets shared by the launcher and the F1 menu (runtime/window_size.h).
$Z -std=c11 -Wall -Wextra -Werror -o build/test/test_window_size.exe tests/native/test_window_size.c
build/test/test_window_size.exe
# [XCDE] audio output channel policy and 5.1 / stereo mixing (runtime/audio_options.h).
$Z -std=c11 -Wall -Wextra -Werror -o build/test/test_audio_options.exe tests/native/test_audio_options.c
build/test/test_audio_options.exe
# [XCDE] custom FPS bounds/parsing and RTSS / MangoHud early limiter state machine (runtime/fps_limiter.h).
$Z -std=c11 -Wall -Wextra -Werror -o build/test/test_fps_limiter.exe tests/native/test_fps_limiter.c
build/test/test_fps_limiter.exe
# [XCDE] T277: GitHub #218 - a non-ASCII folder through the UTF-8 path wrappers (utf8_path.h).
$Z -std=c11 -Wall -Wextra -Werror -o build/test/test_utf8_paths.exe tests/native/test_utf8_paths.c
build/test/test_utf8_paths.exe build/test
$Z -std=c11 -O1 -w -Iruntime -o build/test/test_graphics_options.exe tests/native/test_graphics_options.c runtime/hle/graphics.c  # [XCDE]
build/test/test_graphics_options.exe
$Z -std=c11 -O1 -w -Iruntime -o build/test/test_prop_lod.exe tests/native/test_prop_lod.c runtime/hle/graphics.c
build/test/test_prop_lod.exe
$Z -std=c11 -Wall -Wextra -Werror -o build/test/test_display_options.exe tests/native/test_display_options.c
build/test/test_display_options.exe

# [XCDE] T259/T260: quality policy and default optimizations, no game data.
$Z -std=c11 -Wall -Wextra -Werror -o build/test/test_render_policy.exe tests/native/test_render_policy.c
build/test/test_render_policy.exe
$Z -std=c11 -Iruntime -o build/test/test_output_scale.exe tests/native/test_output_scale.c runtime/nvn/output_scale.c
build/test/test_output_scale.exe
$Z -O1 -ffunction-sections -fdata-sections -Wl,--gc-sections -Iruntime -Ithird_party -o build/test/test_texture_index.exe tests/native/test_texture_index.c
build/test/test_texture_index.exe
$Z -O1 -ffunction-sections -fdata-sections -Wl,--gc-sections -Iruntime -o build/test/test_romfs_cache.exe tests/native/test_romfs_cache.c $M/lib/libwinpthread.dll.a
build/test/test_romfs_cache.exe build/test
# [XCDE] T037: atomic save writes and commits (short pwrite, ENOSPC / fsync failures, journal kept on a failed commit).
$Z -O1 -w -Iruntime -o build/test/test_fs_commit.exe tests/native/test_fs_commit.c $M/lib/libwinpthread.dll.a
build/test/test_fs_commit.exe

# [XCDE] T266: middle-button stick click as a pulse, dropped with ZL held.
$Z -std=c11 -Wall -Wextra -Werror -o build/test/test_stick_click.exe tests/native/test_stick_click.c
build/test/test_stick_click.exe

# [XCDE] GitHub #121: HUD scale anchors and groups (runtime/ui_scale.h).
$Z -std=c11 -Wall -Wextra -Werror -o build/test/test_ui_scale.exe tests/native/test_ui_scale.c
build/test/test_ui_scale.exe

# [XCDE] GitHub #172: scalable resize preservation, color RT clear, and material-only LOD bias policy.
$Z -std=c11 -Wall -Wextra -Werror -Iruntime -o build/test/test_tex_resize.exe tests/native/test_tex_resize.c
build/test/test_tex_resize.exe

# [XCDE] Cliff batching, foliage distance, graphics options, translate CFG.
$Z -std=c11 -Wall -Wextra -Werror -Iruntime -Iruntime/nvn -o build/test/test_cliff_batch.exe tests/native/test_cliff_batch.c
build/test/test_cliff_batch.exe
$Z -std=c11 -Wall -Wextra -Werror -Iruntime -o build/test/test_foliage_distance.exe tests/native/test_foliage_distance.c runtime/hle/foliage_distance.c
build/test/test_foliage_distance.exe
$Z -O1 -w -Iruntime/nvn/maxwell -o build/test/test_translate_cfg.exe tests/native/test_translate_cfg.c runtime/nvn/maxwell/translate.c $M/lib/libshaderc_shared.dll.a
build/test/test_translate_cfg.exe
