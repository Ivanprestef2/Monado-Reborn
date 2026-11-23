#!/usr/bin/env bash
# Native (C) runtime tests on Linux. No game data needed.
set -e
cd "$(dirname "$0")/.."

CC="${CC:-clang}"
OUT="build/test-linux"
mkdir -p "$OUT"

INC="-Iruntime -Iruntime/nvn -Iruntime/nvn/maxwell -Ithird_party"
if [ -n "${VMA_INCLUDE_DIR:-}" ]; then
  INC="$INC -I$VMA_INCLUDE_DIR"
fi

run_test() {
  local name="$1"; shift
  echo "=== $name ==="
  "$CC" -O1 $INC "$@" -o "$OUT/$name"
  if [ "$name" = "test_port_core" ]; then
    "$OUT/$name" "$OUT/port-core.ini"
  elif [ "$name" = "test_settings_roundtrip" ]; then
    "$OUT/$name" "$OUT/settings-roundtrip.ini"
  else
    "$OUT/$name"
  fi
}

run_test test_cliff_batch -std=c11 -Wall -Wextra -Werror tests/native/test_cliff_batch.c
run_test test_display_options -std=c11 -Wall -Wextra -Werror tests/native/test_display_options.c
run_test test_foliage_distance -std=c11 -Wall -Wextra -Werror tests/native/test_foliage_distance.c runtime/hle/foliage_distance.c -lm
run_test test_footstep_repeat -std=gnu11 -D_GNU_SOURCE -w -ffunction-sections -fdata-sections -Wl,--gc-sections tests/native/test_footstep_repeat.c -lpthread -lm
run_test test_gpu_probe -std=c11 -Wall -Wextra -Werror tests/native/test_gpu_probe.c
run_test test_graphics_options -std=c11 -Wall -Wextra -Werror tests/native/test_graphics_options.c runtime/hle/graphics.c -lm
run_test test_host_fma -std=gnu11 -w -fno-strict-aliasing tests/native/test_host_fma.c -lm
run_test test_log_timestamp -std=gnu11 -Wall -Wextra -Werror tests/native/test_log_timestamp.c runtime/log.c
run_test test_movie_buf -std=gnu11 -Wall -Wextra -Werror tests/native/test_movie_buf.c
run_test test_output_scale -std=c11 -Wall -Wextra -Werror tests/native/test_output_scale.c runtime/nvn/output_scale.c -lm
run_test test_port_core -std=c11 -Wall -Wextra -Werror tests/native/test_port_core.c -lm
run_test test_prop_lod -std=c11 -Wall -Wextra -Werror tests/native/test_prop_lod.c runtime/hle/graphics.c -lm
run_test test_render_policy -std=c11 -Wall -Wextra -Werror tests/native/test_render_policy.c
run_test test_settings_roundtrip -std=gnu11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200112L tests/native/test_settings_roundtrip.c runtime/host/settings_ini.c runtime/hle/cheats_options.c runtime/hle/graphics.c runtime/hle/foliage_distance.c -lpthread -lm
run_test test_stick_click -std=c11 -Wall -Wextra -Werror tests/native/test_stick_click.c
run_test test_tegra_block_height -std=gnu11 -w tests/native/test_tegra_block_height.c runtime/nvn/tegra.c runtime/nvn/formats.c -lpthread
run_test test_tegra_copy -std=gnu11 -w tests/native/test_tegra_copy.c runtime/nvn/tegra.c runtime/nvn/formats.c -lpthread
run_test test_tex_resize -std=c11 -Wall -Wextra -Werror tests/native/test_tex_resize.c
run_test test_time_math -std=gnu11 -Wall -Wextra -Werror tests/native/test_time_math.c
run_test test_translate_ldc -std=gnu11 -w tests/native/test_translate_ldc.c runtime/nvn/maxwell/translate.c -lm
run_test test_translate_tex -std=gnu11 -w tests/native/test_translate_tex.c runtime/nvn/maxwell/translate.c -lm
run_test test_translate_xfb -std=gnu11 -w tests/native/test_translate_xfb.c runtime/nvn/maxwell/translate.c -lm
run_test test_ui_scale -std=c11 -Wall -Wextra -Werror tests/native/test_ui_scale.c -lm
run_test test_window_size -std=c11 -Wall -Wextra -Werror tests/native/test_window_size.c
# [XCDE] T277: GitHub #218 - a non-ASCII folder through the UTF-8 path wrappers (utf8_path.h).
run_test test_utf8_paths -std=c11 -Wall -Wextra -Werror tests/native/test_utf8_paths.c

if pkg-config --exists shaderc 2>/dev/null || echo '#include <shaderc/shaderc.h>' | "$CC" -x c -E - >/dev/null 2>&1; then
  SHADERC_LIBS="$(pkg-config --libs shaderc 2>/dev/null || echo -lshaderc_shared)"
  run_test test_translate_cfg -std=gnu11 -w tests/native/test_translate_cfg.c runtime/nvn/maxwell/translate.c $SHADERC_LIBS -lm
else
  echo "=== test_translate_cfg: SKIPPED (shaderc headers missing) ==="
fi

if echo '#include <vk_mem_alloc.h>' | "$CC" $INC -x c -E - >/dev/null 2>&1 && pkg-config --exists sdl3 2>/dev/null; then
  SDL_CFLAGS="$(pkg-config --cflags sdl3)"
  run_test test_texture_index -std=gnu11 -w $SDL_CFLAGS -ffunction-sections -fdata-sections -Wl,--gc-sections tests/native/test_texture_index.c
else
  echo "=== test_texture_index: SKIPPED (vk_mem_alloc.h or sdl3 missing) ==="
fi

if pkg-config --exists sdl3 2>/dev/null; then
  SDL_CFLAGS="$(pkg-config --cflags sdl3)"
  SDL_LIBS="$(pkg-config --libs sdl3)"
  run_test test_linux_write_watch -std=gnu11 -Wall -Wextra -Werror $SDL_CFLAGS tests/native/test_linux_write_watch.c runtime/platform/linux.c $SDL_LIBS -lpthread -ldl -lm
  run_test test_condvar -std=gnu11 -D_GNU_SOURCE -w $SDL_CFLAGS tests/native/test_condvar.c runtime/platform/linux.c runtime/log.c runtime/diag/diag.c runtime/diag/sampler.c $SDL_LIBS -lpthread -ldl -lm
else
  echo "=== test_linux_write_watch / test_condvar: SKIPPED (sdl3 missing) ==="
fi
