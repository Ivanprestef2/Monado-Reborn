# Optimisation level and profile-guided build

The generated game code and runtime compile at `-O3` by default. The release workflow uses the same level on Windows
and Linux, and the packaged builder uses it when it compiles the player's game code.

| Option | Values | What it does |
|---|---|---|
| `MONADO_OPT` | `O0`, `O1`, `O2`, `O3` (default) | optimisation level of the game code, runtime, and launcher |
| `MONADO_PGO` | empty (off), `gen`, a `.profdata` file | `gen` instruments the build. A profile file enables profile-guided optimisation at the selected optimisation level. |
| `MONADO_OUT_DIR` | a folder | where the executables go, so several variants can sit side by side for A/B runs |

The idea comes from TotK Recomp (`docs/pgo.md` there): a profile lays hot paths out straight, inlines hot calls and
compiles code that never ran for size. They measured 12% faster frames in a trained fight and 6% in an untrained one.
Another recomp (dod3) measured PGO as 8% slower, so measure before trusting it ([RECOMP_KNOWLEDGE.md](../reference/RECOMP_KNOWLEDGE.md)).

Measured here (2026-11-17, RX 6900 XT, deterministic mode, the new-game route shifted past the boot: boot, title
screen and the opening in-engine cutscene, 72,000 frames; mean CPU time per frame):

| Build | Boot | Title | Cutscene | Whole run |
|---|---|---|---|---|
| -O1 (before) | 2.82 ms | 3.07 ms | - | - |
| -O2 | 2.78 ms | 3.03 ms | 5.33 ms | 3.82 ms |
| -O2 + profile | 2.63 ms | 2.87 ms | 4.96 ms | 3.59 ms |

That first profile came from the scripted route alone. The published one adds about 15 minutes of real play on
several saves (fields, towns, battles, menus, loading), which took the game functions with counts from 9,678 to 12,673.
With it, loading a save into the field (deterministic mode, mean CPU time per frame, 2 runs each):

| Build | Boot | Title and menus | Field | Whole run |
|---|---|---|---|---|
| -O1 (before) | 2.89 ms | 3.26 ms | 13.26 ms | 6.12 ms |
| -O2 + profile | 2.73 ms | 3.11 ms | 11.24 ms | 5.42 ms |

The field (-15%) was part of the training too, so this is still a best case: TotK Recomp's gain halved on scenes
outside its training. A scripted battle from a save next to an enemy would measure that; it is not done yet.
Code that never ran in the training counts as cold (optimised for size), so retrain with wide coverage, not one scene.

To train with your own data folder (settings, saves, the launcher's environment): `scripts/pgo_train.sh`.

## Train a profile (Linux, system clang)

1. Build the instrumented executable into its own folder:

   ```bash
   cmake -S . -B build/cmake/pgo-gen -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
         -DMONADO_PGO=gen -DMONADO_OUT_DIR=$PWD/build/pgo/gen -DVMA_INCLUDE_DIR=<folder with vk_mem_alloc.h>
   cmake --build build/cmake/pgo-gen --target monado_game_exe
   ```

2. Play it: `build/pgo/gen/monado targets`. It runs slower than a normal build. Counts go to
   `build/pgo/raw/default_<id>.profraw` once a minute and at exit; every run of the same executable adds to that file.
   Cover what players spend their time on: open fields and towns, battles (arts, chain attacks, Monado arts), menus,
   cutscenes, loading into new areas, day and night, weather. Keep one or two scenes out of the training so you can
   measure on code the profile has not seen.
3. Merge: `python tools/build/pgo.py merge` writes `build/pgo/monado.profdata` and `build/pgo/monado.json`, a record of the
   lifter and the runtime headers (`cpu.h`, `simd.h`, `simd_fp.h`) it was trained on.
4. Publish: `python tools/build/pgo.py publish` writes `data/pgo/monado.proftext.gz` (about 0.8 MB) and `data/pgo/monado.json`.
   Commit both: release builds use them.

Train on Linux: the Windows build uses zig cc, which ships no profile runtime for `gen`. One profile serves both
platforms: the game code is the same C on both, so its functions match (checked: the same 3 of 1,302 functions, tiny
inline NaN helpers with no counts, mismatch on Linux and Windows alike). Only the runtime's platform layer
(`windows.c`) never ran in a Linux training, and it compiles without a profile as before.

## Release packages

Players build the game code on their own PC (`tools/gamebuild/gamebuild.py`, bundled zig 0.17.1), so the profile has to reach
that build:

- The committed copy is LLVM's **text** format. A compiler only reads an indexed profile written by its own LLVM
  version or an older one: zig 0.17.1's clang 22 rejects one from clang 23 ("unsupported instrumentation profile
  format version"). Text reads anywhere (checked with LLVM 15 and 21).
- The release workflow's `pgo` job runs `tools/build/pgo.py index --tool llvm-profdata-19`: LLVM 19 is the oldest compiler
  in the release (clang-19 builds the Linux runtime), and zig's clang 22 reads its output too.
- Both platform jobs build the runtime with `-DMONADO_PGO=build/pgo/monado.profdata` and stage it for the player's
  build (`tools/gamebuild/gamebuild_stage.py --pgo`, `link.json` `"pgo"`); `tools/gamebuild/gamebuild.py` then compiles every game file
  with `-fprofile-use`.
- The guest flags are `-O3` (`tools/build/game_id.py`), and `GAMELIB_ID` includes the profile, so a new profile makes every
  player's launcher rebuild the game code once.
- Every compile that uses the profile also gets `-DMONADO_PGO_ID=<its digest>` (CMake and `tools/gamebuild/gamebuild.py`).
  zig's compiler cache keys on the command line and the included files, not on the profile's contents: without the
  digest, a new profile at the same path got the old objects back, from CI's restored zig cache and from the zig-cache
  a player's build keeps between releases.
- The profile holds function names (`f_<guest address>`), control-flow hashes and counts: no game code. TotK Recomp
  ships its profiles the same way.

When you bump zig or the Linux CI compiler, keep the `pgo` job's `llvm-profdata` no newer than the oldest of them.

## Build with it

```bash
cmake -S . -B build/cmake/pgo-use -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DMONADO_PGO=build/pgo/monado.profdata -DMONADO_OUT_DIR=$PWD/build/pgo/use -DVMA_INCLUDE_DIR=<...>
cmake --build build/cmake/pgo-use --target monado_game_exe
```

Every compile depends on the profile, so a new profile rebuilds everything.

## When the profile goes stale

clang matches each function by name and a hash of its control flow, and silently drops the counts of a function whose
code changed. The helpers of `cpu.h`, `simd.h` and `simd_fp.h` are inlined into every lifted function, so a change to
them, or to the lifter (`tools/lifter`), invalidates the whole profile. `python tools/build/pgo.py check` says whether that
happened. Game-code patches only touch the patched functions and can wait for the next training.

## Measure

`tools/diag/benchmark_native.py --reference <exe> --candidate <exe> --ini <ini> --frames N --warmup W` alternates the two
executables with the same settings and route in deterministic mode (one guest thread at a time, so the time per frame
is the CPU cost of the frame) and reports the median FPS change.
