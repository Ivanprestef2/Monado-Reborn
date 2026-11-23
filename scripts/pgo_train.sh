#!/usr/bin/env bash
# Play the instrumented (MONADO_PGO=gen) build with your own data folder: settings (xcde.ini), saves, the launcher's
# environment variables (xcde_env.ini) and the game files it points at. Counts go to build/pgo/raw/ (docs/guides/PGO.md).
#   scripts/pgo_train.sh [data folder, default ~/.local/share/monado]
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
data=${1:-${XDG_DATA_HOME:-$HOME/.local/share}/monado}
exe=$repo/build/pgo/gen/monado
[[ -x $exe ]] || { echo "no instrumented build at $exe (docs/guides/PGO.md, step 1)" >&2; exit 1; }
# the native FSR 3 / FSR 4 Lite components load from beside the executable: borrow the normal build's
for lib in libmonado_fsr3.so libmonado_fsr4.so; do
  [[ -e $repo/build/pgo/gen/$lib || ! -e $repo/build/linux/$lib ]] || ln -s ../../linux/$lib "$repo/build/pgo/gen/$lib"
done
[[ -f $data/xcde.ini ]] || echo "warning: no xcde.ini in $data: default settings" >&2
game=$(cat "$data/game-root.txt" 2>/dev/null || true)
[[ -n $game && -d $game/exefs ]] || game=$data/targets
[[ -d $game/exefs ]] || game=$repo/targets
# the launcher passes these NAME=VALUE lines to the game
if [[ -f $data/xcde_env.ini ]]; then
  while IFS= read -r line; do
    [[ $line =~ ^[A-Za-z_][A-Za-z0-9_]*= ]] && export "${line?}"
  done < "$data/xcde_env.ini"
fi
unset LARECOMP_HIDDEN XCDE_DETERMINISTIC XCDE_INPUT_REPLAY XCDE_INI LARECOMP_SAVE_DIR
export LARECOMP_DATA_ROOT=$data
log=$repo/build/pgo/train.log
echo "data: $data  game: $game  log: $log"
cd "$data"
# (the whole log goes to $log; the terminal shows where it read data, settings and saves from)
"$exe" "$game" 2>&1 | tee "$log" | grep --line-buffered -E "^\[paths\]|\[save\] read .*bfssystem|xcde.ini" || true
ls -la "$repo"/build/pgo/raw/
