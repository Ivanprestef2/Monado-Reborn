#!/usr/bin/env bash
# Build the headless boot executable (M3a) from targets/ (dump) with the project venv's zig cc.
#   1. python tools/build/gen_all.py            -> generated/{main,sdk}
#   2. this script                        -> build/obj/*.o, build/rt/*.o, build/xcde_boot.exe
#   3. build/xcde_boot.exe targets 15     -> boot log on stderr
set -e
cd "$(dirname "$0")/.."
Z=".venv/Scripts/python -m ziglang cc"; export Z
CF="-O3 -march=x86_64_v2 -ffp-contract=off -fno-strict-aliasing -fvisibility=hidden -Iruntime -w"; export CF
mkdir -p build/obj/main build/obj/sdk build/rt
# drop objects whose chunk no longer exists (the chunk layout shifts when the generator output changes)
for o in build/obj/*/*.o; do m=$(basename "$(dirname "$o")"); b=$(basename "$o" .o); [ -f "generated/$m/$b.c" ] || rm -f "$o"; done
cc1() {
  local source=$1 module object
  module=$(basename "$(dirname "$source")")
  object="build/obj/$module/$(basename "$source" .c).o"
  if [[ $object -nt $source && $object -nt scripts/build_boot.sh &&
        $object -nt runtime/cpu.h && $object -nt runtime/simd.h && $object -nt runtime/simd_fp.h ]]; then
    return
  fi
  $Z -c "$source" -o "$object.$BASHPID.tmp" $CF &&
    mv -f "$object.$BASHPID.tmp" "$object" || { rm -f "$object.$BASHPID.tmp"; echo "FAIL $source" >&2; return 1; }
}
export -f cc1
ls generated/main/*.c generated/sdk/*.c | xargs -P "${JOBS:-16}" -I{} bash -c 'cc1 {}'
for f in loader dispatch kernel ipc fnprof log platform/windows services/fs services/romfs_virtual services/game_files services/am services/acc \
         services/time services/hid_set services/misc hle/heaps hle/pl hle/vi hle/nvn_boot hle/fibers hle/framepace hle/foliage_distance hle/foliage_pool hle/graphics hle/cheats_options hle/cheats_combat hle/cheats_rewards hle/cheats_enemy_health hle/title_exit hle/system_exit hle/desktop_exit_caption hle/expanded_saves hle/expanded_save_scan hle/quickstep_native hle/quickstep_picker hle/quickstep_save hle/quickstep_slot hle/quickstep_stats hle/quickstep_stats_ui hle/quickstep_ui_memory host/stubs host/settings_boot host/gamelib_static host/main_boot diag/diag diag/sampler diag/watchdog diag/crashdump; do
  $Z -c runtime/$f.c -o build/rt/$(echo $f | tr / _).o $CF -DGAMELIB_ID='"dev"'
done
# (the window-less boot exe is a dev leftover from M3; XCDE_SKIP_BOOT_LINK=1 builds only the game objects - scripts/build_all.sh)
[ -n "$XCDE_SKIP_BOOT_LINK" ] || scripts/link_boot.sh
