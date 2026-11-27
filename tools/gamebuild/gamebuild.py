"""Build the game code library the runtime loads (release packages only).

Source builds compile the whole game into the executable. A release package ships
this builder instead: on first start it lifts the guest code from the player's own
`exefs/` into C (`tools/lifter`), compiles it with the bundled `zig cc` and links
`lasgame.so` / `lasgame.dll` in the writable data folder. Nothing derived from the
game is distributed, and the runtime refuses a library built by another generator
(GAMELIB_ID, tools/build/game_id.py).

Layout expected in the resources folder (`--resources`, default: `gamebuild/`
beside the executable, or `../share/monado/gamebuild` in an AppImage):

    link.json              compiler, guest flags, extra link inputs
    gamelib-id.txt         GAMELIB_ID the runtime was built with
    zig[.exe]              bundled compiler
    entry/gamelib_entry.c  the library's exported entry point
    include/               cpu.h, simd.h, simd_fp.h (the guest ABI)
    games/                 profile.json, patches.py, hle_symbols.txt

Usage: monado-gamebuild --root <dump folder or .nup> [--out DIR] [--resources DIR] [--jobs N]
                        [--force] [--dry-run] [--precompile]
       gamebuild.py --write-costs <gamebuild.log> games/xcde/compile_cost.json   (after a release's lifter change)
Exit codes: 0 ok, 1 usage, 2 bad game files, 3 generation failed, 4 compile failed,
5 link failed. Progress is written for the launcher: `<out>/progress.txt`, and also printed as
`progress <0..1> <text>` lines when RecompLauncher runs it (RECOMPLAUNCHER=1, recomplauncher.json).
"""
import argparse
import faulthandler
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import traceback
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
for _sub in ('tools/lifter', 'tools/extract'):
    _path = ROOT / _sub
    if _path.is_dir() and str(_path) not in sys.path:
        sys.path.insert(0, str(_path))

import gen  # noqa: E402  (tools/lifter/gen.py)
import nup  # noqa: E402  (tools/extract/nup.py, T280)

# GitHub #86: zig could sit idle for good on some Windows PCs and the log showed nothing past the lifting. Every step
# is now logged with a timestamp and flushed at once, each compiler command is written before it runs, the final link
# runs with -v into the log, and a stuck compiler ends in an error instead of a build that never finishes.
COMPILE_TIMEOUT = 20 * 60  # one chunk (they take seconds)
LINK_TIMEOUT = 45 * 60     # the library (minutes on a slow PC, including zig's one-time C runtime build)
_log_lock = threading.Lock()
_log_start = time.time()


def note(log, text):
    with _log_lock:
        log.write(f'[{time.strftime("%H:%M:%S")} +{time.time() - _log_start:.0f}s] {text}\n')
        log.flush()


def quoted(command):
    return ' '.join(f'"{part}"' if ' ' in str(part) else str(part) for part in command)


def run_tool(command, env, log, what, timeout, stream=False):
    """Run the compiler. stream: its output goes straight into the log (visible while it runs)."""
    note(log, f'{what}: {quoted(command)}')
    started = time.time()
    try:
        if stream:
            with _log_lock:
                log.flush()
                result = subprocess.run(command, text=True, stdout=log, stderr=subprocess.STDOUT, env=env,
                                        timeout=timeout, stdin=subprocess.DEVNULL)
        else:
            result = subprocess.run(command, text=True, capture_output=True, env=env, timeout=timeout,
                                    stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        note(log, f'{what}: no result after {timeout // 60} minutes: stopped (the compiler hung)')
        raise SystemExit(f'gamebuild: {what} hung for {timeout // 60} minutes; see the log')
    note(log, f'{what}: exit {result.returncode} after {time.time() - started:.1f}s')
    return result


def control_flow_guard():
    """Whether Windows enforces Control Flow Guard here (GitHub #104: on one PC it did despite the frozen builder's
    cleared CFG flag; the shipped unicorn copes with it, older ones died in VCRUNTIME140's longjmp)."""
    try:
        import ctypes
        flags = ctypes.c_uint32(0)
        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        if not kernel32.GetProcessMitigationPolicy(ctypes.c_void_p(kernel32.GetCurrentProcess()), 7,  # CFG policy
                                                   ctypes.byref(flags), ctypes.sizeof(flags)):
            return f'unknown (error {ctypes.get_last_error()})'
        return ('on' + (', strict' if flags.value & 4 else '')) if flags.value & 1 else 'off'
    except Exception as error:  # (diagnostic only)
        return f'unknown ({error})'


def describe_compiler(resources, out, link, log):
    """The compiler's version and settings (its cache folder above all) at the top of the log."""
    compiler = resources / link['compiler']
    env = compiler_env(resources, out, link)
    note(log, f'platform {sys.platform}, {os.cpu_count()} CPUs, compiler {compiler}')
    if os.name == 'nt':
        note(log, f'Control Flow Guard in this process: {control_flow_guard()}')
    note(log, f'ZIG_LIB_DIR={env.get("ZIG_LIB_DIR", "(unset)")} ZIG_GLOBAL_CACHE_DIR={env["ZIG_GLOBAL_CACHE_DIR"]} '
              f'link: {"prebuilt C runtime" if link.get("lld") else "zig cc"}')
    for args in (['version'], ['env']):
        try:
            result = run_tool([str(compiler)] + args, env, log, f'zig {args[0]}', 120)
            with _log_lock:
                log.write((result.stdout or '') + (result.stderr or ''))
                log.flush()
        except SystemExit:
            pass  # (already logged; the build itself decides)

# Peak memory of one compiler job: 340 MB measured (zig cc -O2 with the profile, the slowest chunk), doubled.
JOB_MEMORY = 768 << 20


def available_memory():
    """Bytes of memory free for new processes, or None when the system does not say."""
    try:
        if sys.platform == 'win32':
            import ctypes

            class Status(ctypes.Structure):
                _fields_ = [('length', ctypes.c_uint32), ('load', ctypes.c_uint32), ('total_phys', ctypes.c_uint64),
                            ('avail_phys', ctypes.c_uint64), ('total_page', ctypes.c_uint64),
                            ('avail_page', ctypes.c_uint64), ('total_virtual', ctypes.c_uint64),
                            ('avail_virtual', ctypes.c_uint64), ('avail_extended', ctypes.c_uint64)]
            status = Status()
            status.length = ctypes.sizeof(Status)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
                return status.avail_phys
            return None
        with open('/proc/meminfo', encoding='ascii') as meminfo:
            for line in meminfo:
                if line.startswith('MemAvailable:'):
                    return int(line.split()[1]) * 1024
    except (OSError, ValueError, AttributeError):
        pass
    return None


def default_jobs():
    """One compiler per CPU, as many as the free memory holds (at least 2). It used to stop at 8: a 16-CPU PC
    compiled at half speed."""
    cpus = os.cpu_count() or 4
    memory = available_memory()
    if memory is None:
        return min(8, cpus)
    return max(2, min(cpus, memory // JOB_MEMORY))


def default_data_root():
    override = os.environ.get('LARECOMP_DATA_ROOT') or os.environ.get('MONADO_DATA_ROOT')
    if override:
        return Path(override)
    if sys.platform == 'win32':
        base = os.environ.get('LOCALAPPDATA') or os.environ.get('APPDATA') or str(Path.home())
        return Path(base) / 'Monado'
    if sys.platform == 'darwin':
        return Path.home() / 'Library' / 'Application Support' / 'Monado'
    xdg = os.environ.get('XDG_DATA_HOME')
    return (Path(xdg) if xdg else Path.home() / '.local' / 'share') / 'monado'


def resources_dir(explicit):
    here = Path(sys.executable if getattr(sys, 'frozen', False) else __file__).resolve().parent
    for candidate in (explicit, os.environ.get('MONADO_GAMEBUILD_DIR'),
                      here, here / 'gamebuild', here.parent / 'share' / 'monado' / 'gamebuild',
                      here.parent / 'gamebuild'):
        if candidate and (Path(candidate) / 'link.json').is_file():
            return Path(candidate)
    raise SystemExit('gamebuild: no game-build resources found (link.json); '
                     'pass --resources <package gamebuild folder>')


def progress_file(out):
    return out / 'progress.txt'


# The launcher's bar and time estimate: the share of the whole build each step takes. Lifting runs on one core, the
# compilation on `jobs` of them. Reference PC (8 cores / 16 threads): lifting 84 s; compiling 158 s with 8 jobs, 149 s
# with 9 and 104 s with 16, i.e. COMPILE_RATIO times the lifting divided by the effective parallelism, where a job past
# half the CPUs (the second thread of a core) adds about half a core. The link is ~10 s on a first build (zig builds
# its C runtime once), the last LINK_SHARE of the bar.
COMPILE_RATIO = 15.0
LINK_SHARE = 0.05
_shares = {'generate': 0.5}


def set_shares(jobs, cpus=None):
    cores = max(1, (cpus or os.cpu_count() or 2) // 2)
    jobs = max(1, jobs)
    effective = min(jobs, cores) + 0.5 * max(0, jobs - cores)
    _shares['generate'] = (1 - LINK_SHARE) / (1 + COMPILE_RATIO / effective)


def overall(phase, fraction):
    """The whole build's progress (0..1) from the progress of one step."""
    generate = _shares['generate']
    if phase == 'generate':
        return generate * fraction
    if phase == 'compile':
        return generate + (1 - LINK_SHARE - generate) * fraction
    return 1 - LINK_SHARE + LINK_SHARE * fraction


def launcher_step_text(phase, done, total, text):
    """What RecompLauncher shows under its setup step (Monado's own launcher words it the same way)."""
    if phase == 'generate':
        return f'Reading the game code: {done} of {total}'
    if phase == 'compile':
        return f'Compiling the game code: {done} of {total} files'
    return text


def print_progress(value, text):
    """RecompLauncher (recomplauncher.json) runs this builder as its setup step, with RECOMPLAUNCHER=1, and reads
    `progress <0..1> <text>` lines from stdout. Monado's own launcher reads progress files instead, so its runs stay
    quiet."""
    if os.environ.get('RECOMPLAUNCHER') != '1':
        return
    print(f'progress {value:.4f} {text}', flush=True)


# With --precompile, the shader precompile follows the game code in one setup step: the bar's first part is the
# build, the rest the shaders (a few minutes each on the reference PC). Without it, the build is the whole bar.
PRECOMPILE_BUILD_SHARE = 0.7
_launcher_build_share = [1.0]


def print_launcher_progress(phase, done, total, text, fraction):
    value = overall(phase, fraction) * _launcher_build_share[0]
    print_progress(value, launcher_step_text(phase, done, total, text))


# ---- the shader precompile (--precompile, RecompLauncher's setup step)
# Monado's own launcher runs `<game> --precompile <dump>` before the first start (tools/launcher/launcher.cpp,
# pc_start): runtime/nvn/vulkan/precompile.c translates every shader in the romfs into cache/, rewriting
# cache/precompile_progress.txt as it goes and writing cache/precompile_done.txt at the end. RecompLauncher starts the
# game directly, so its setup step runs the same precompile here, from the data root.

def game_executable():
    """The game program that precompiles: MONADO_GAME_EXE (AppRun sets it), else build/xcde.exe beside this builder in
    the Windows package (build/gamebuild/). None when neither is there."""
    named = os.environ.get('MONADO_GAME_EXE')
    if named and Path(named).is_file():
        return Path(named)
    if not getattr(sys, 'frozen', False):
        return None
    sibling = Path(sys.executable).resolve().parent.parent / 'xcde.exe'
    return sibling if sibling.is_file() else None


# Exit codes of the runtime's `--precompile-status <dump>` mode (runtime/nvn/vulkan/precompile.c).
PC_STATUS_CURRENT, PC_STATUS_STALE, PC_STATUS_UNREADABLE, PC_STATUS_DISABLED = 0, 1, 2, 3
PC_STATUS_TIMEOUT_S = 60  # ([XCDE] T282 review: the walk measured 78-108 ms; this only bounds a hung/locked child)


def precompile_status(data_root, exe, dump):
    """The runtime's answer for cache/precompile_done.txt: 0 current, 1 needs preparing, 2 the game files cannot be read,
    3 SPIR-V persistence is disabled (precompilation is not applicable); -1 when it cannot be asked (no runnable
    executable, timeout, unexpected exit). [XCDE] T282"""
    reset_dll_directory()
    flags = subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0
    try:
        done = subprocess.run([str(exe), '--precompile-status', str(dump)], cwd=data_root, env=child_environment(),
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=flags,
                              timeout=PC_STATUS_TIMEOUT_S)
    except (OSError, subprocess.TimeoutExpired):
        return -1
    return done.returncode if done.returncode in (0, 1, 2, 3) else -1


def precompile_is_current(data_root, exe, dump):
    """Whether the runtime considers the precompiled shaders current for this build and these game files:
    `<game> --precompile-status <dump>` (runtime/nvn/vulkan/precompile_sig.c) compares the translator/SPIR-V compiler
    fingerprint, the environment switches, the precompile's pass switches and the game files with
    cache/precompile_done.txt, without scanning or opening a Vulkan device. Exit code 0 = current. ([XCDE] T282: before,
    this read an `exe <size> <mtime>` line, so every new build precompiled everything again even when it could not change
    the shaders.)"""
    return precompile_status(data_root, exe, dump) == PC_STATUS_CURRENT


# [XCDE] T281: the precompile writes "<phase> <overall done> <overall total> <this pass done> <this pass total>" over
# its four passes (scan, programs, seed variants, whole pipelines); the overall fraction is already weighted by the
# measured cost of each pass (runtime/nvn/vulkan/precompile.c), so the bar here is the runtime's own, not a model.
PRECOMPILE_PASSES = {'scan': 'scanning the game files', 'build': 'building the shader programs',
                     'seed': 'known variants (seed)', 'pipelines': 'whole optimized pipelines'}


def precompile_step(line):
    """(share of the precompile done, text) from a cache/precompile_progress.txt line. None when unreadable, and for
    the closing `done` line ("Shaders ready" follows it)."""
    parts = line.split()
    if len(parts) != 5 or parts[0] == 'done':
        return None
    try:
        overall_done, overall_total, pass_done, pass_total = (int(x) for x in parts[1:])
    except ValueError:
        return None
    if overall_total <= 0:
        return None
    share = min(1.0, max(0.0, overall_done / overall_total))
    name = PRECOMPILE_PASSES.get(parts[0], parts[0])
    return share, f'Preparing the shaders: {name}, {pass_done} of {pass_total}'


def child_environment():
    """The environment for the game when this builder starts it. A frozen builder (PyInstaller) points
    LD_LIBRARY_PATH at its own libraries and keeps the original in LD_LIBRARY_PATH_ORIG: the game needs the original
    (AppRun's usr/lib)."""
    env = dict(os.environ)
    if 'LD_LIBRARY_PATH_ORIG' in env:
        env['LD_LIBRARY_PATH'] = env.pop('LD_LIBRARY_PATH_ORIG')
    elif getattr(sys, 'frozen', False):
        env.pop('LD_LIBRARY_PATH', None)
    return env


def reset_dll_directory():
    """A frozen builder on Windows adds its own folder to the DLL search (SetDllDirectory), and the programs it starts
    inherit that: the game must load its DLLs from build/ only."""
    if sys.platform != 'win32' or not getattr(sys, 'frozen', False):
        return
    import ctypes
    ctypes.windll.kernel32.SetDllDirectoryW(None)


def precompile_shaders(dump):
    """The shader precompile, after the game code. It never fails the setup: without it the game compiles each shader
    when it first needs it, as it always could (the launcher's "Omitir")."""
    share = _launcher_build_share[0]
    exe = game_executable()
    if exe is None:
        print('gamebuild: no game executable to precompile the shaders with (MONADO_GAME_EXE); skipped', flush=True)
        print_progress(1, 'Shaders compile while you play')
        return
    data_root = default_data_root()
    status = precompile_status(data_root, exe, dump)
    if status == PC_STATUS_CURRENT:
        print_progress(1, 'Shaders already prepared')
        return
    if status == PC_STATUS_DISABLED:
        print('gamebuild: SPIR-V persistence is disabled (LARECOMP_VK_NO_SPIRV_CACHE); the shader precompile was '
              'skipped because it does not apply; the game compiles shaders as it reaches them', flush=True)
        print_progress(1, 'Shaders compile while you play')
        return
    if status == -1:  # (timeout, spawn failure or an unexpected exit: a full pass cannot be bounded or trusted)
        print('gamebuild: could not verify the shader cache (the status probe timed out, could not start, or exited '
              'unexpectedly); the shader precompile was skipped', flush=True)
        print_progress(1, 'Shaders compile while you play')
        return
    if status == PC_STATUS_UNREADABLE:  # [XCDE] T282 review: the run would exit 2 at once and print only a generic error
        print('gamebuild: the game files cannot be read (the shader precompile was skipped); check the input dump',
              flush=True)
        print_progress(1, 'Shaders compile while you play')
        return
    print_progress(share, 'Preparing the shaders')
    (data_root / 'cache').mkdir(parents=True, exist_ok=True)
    (data_root / 'build').mkdir(parents=True, exist_ok=True)
    progress = data_root / 'cache' / 'precompile_progress.txt'
    progress.unlink(missing_ok=True)
    reset_dll_directory()
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if sys.platform == 'win32' else 0
    last = None
    with (data_root / 'build' / 'last_precompile.log').open('w', encoding='utf-8', errors='replace') as log:
        try:
            process = subprocess.Popen([str(exe), '--precompile', str(dump)], cwd=data_root, stdout=log,
                                       stderr=subprocess.STDOUT, env=child_environment(), creationflags=flags)
        except OSError as error:
            print(f'gamebuild: could not start the shader precompile ({error}); the game compiles shaders as it reaches them',
                  flush=True)
            print_progress(1, 'Shaders compile while you play')
            return
        while process.poll() is None:
            time.sleep(0.5)
            try:
                step = precompile_step(progress.read_text(encoding='utf-8', errors='replace'))
            except OSError:
                continue  # (not written yet, or being replaced)
            if step and step != last:
                last = step
                print_progress(share + (1 - share) * step[0], step[1])
    if process.returncode != 0:
        print(f'gamebuild: the shader precompile stopped with code {process.returncode} (build/last_precompile.log); '
              'the game compiles shaders as it reaches them', flush=True)
        print_progress(1, 'Shaders compile while you play')
        return
    print_progress(1, 'Shaders ready')


def report(out, phase, done, total, text='', fraction=None):
    """`<phase> <done> <total> <text>`, then `overall <0..1>`: the launcher's bar (time-weighted, so it moves at a
    steady pace and gives a time estimate). Older launchers read the first line only."""
    path = progress_file(out)
    if fraction is None:
        fraction = done / total if total else 1.0
    print_launcher_progress(phase, done, total, text, fraction)
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix('.tmp')
        temporary.write_text(f'{phase} {done} {total} {text}\noverall {overall(phase, fraction):.4f}\n',
                             encoding='utf-8')
        os.replace(temporary, path)
    except OSError:
        pass  # a progress file the launcher cannot read must never fail the build


def fingerprint(dump, resources, link):
    """Everything a rebuild depends on: the generator/ABI id, the dump and the link inputs."""
    import hashlib
    h = hashlib.sha256()
    h.update((resources / 'gamelib-id.txt').read_text(encoding='utf-8').strip().encode())
    h.update(json.dumps(link, sort_keys=True).encode())
    package = nup_package(dump)
    for name, relative in (('main', 'exefs/main'), ('sdk', 'exefs/sdk')):
        if package:  # (a package copied or rebuilt with the same modules needs no new build: their size and build id)
            head = package.read(relative, 0x60)
            h.update(f'{name} nup {package.sizes[package.find(relative)]} {head[0x40:0x60].hex()}\n'.encode())
            continue
        path = dump / relative
        st = path.stat()
        h.update(f'{name} {st.st_size} {int(st.st_mtime)}\n'.encode())
    for extra in link.get('link_files', []):
        path = resources / extra
        if path.is_file():
            h.update(path.name.encode())
            h.update(str(path.stat().st_size).encode())
    crt = resources / 'crt'
    if crt.is_dir():
        for path in sorted(crt.iterdir()):
            h.update(f'{path.name} {path.stat().st_size}\n'.encode())
    return h.hexdigest()


def load_profile(resources):
    return json.loads((resources / 'games/profile.json').read_text(encoding='utf-8'))


# ---- a .nup package as the dump (T280): its exefs and romfs are already decrypted and read in place
def nup_package(dump):
    """The package the dump is (tools/extract/nup.py), or None for a folder with exefs/ and romfs/."""
    return nup.NupPackage(dump) if dump.is_file() and nup.is_package(dump) else None


def exefs_folder(dump, out, modules):
    """The folder the lifter reads the modules from: the dump's exefs/, or for a package a copy of the modules it lifts
    in <out>/nup-exefs/ (the lifter reads files; written again only when the package's module differs)."""
    package = nup_package(dump)
    if not package:
        return dump / 'exefs'
    folder = out / 'nup-exefs'
    folder.mkdir(parents=True, exist_ok=True)
    for module in modules:
        data = package.read('exefs/' + module['name'])
        target = folder / module['name']
        if not target.is_file() or target.read_bytes() != data:
            target.write_bytes(data)
    return folder


def validate(dump, resources, strict=False):
    import check_dump
    # GitHub #132: the module build-ids prove the version; a romfs from another region differs only in localized data
    # (bdat/menu sizes), so it is built with a warning instead of being refused. --strict-dump asks for the reference
    # dump exactly (the source-build check_dump default is also strict-capable with --strict).
    result = check_dump.check_dump(str(dump), load_profile(resources), allow_romfs_variant=not strict)
    for warning in result.warnings:
        print(f'gamebuild: warning: {warning.strip()}', flush=True)
    if not result.ok:
        raise SystemExit(f'gamebuild: {dump}: ' + '; '.join(result.messages))


def generate_modules(dump, resources, out, jobs, log):
    """Lift every module the profile marks `lift` into <out>/generated/<module>/*.c."""
    profile = load_profile(resources)
    modules = [m for m in profile['modules'] if m.get('lift')]
    generated = out / 'generated'
    generated.mkdir(parents=True, exist_ok=True)
    exefs = exefs_folder(dump, out, modules)  # [T280] the dump's exefs/, or the modules from its .nup package
    # Lifting time follows the module's size (main and sdk are 6.6 and 5.4 MB): the bar moves by bytes, not modules.
    sizes = [(exefs / m['name']).stat().st_size for m in modules]
    total_size = sum(sizes) or 1
    for index, module in enumerate(modules):
        report(out, 'generate', index, len(modules), f'Reading {module["name"]}', sum(sizes[:index]) / total_size)
        argv = [module['name'], str(exefs / module['name']), str(module['base']),
                str(generated / module['name']),
                '--simd', 'c',
                '--patches', str(resources / 'games/patches.py'),
                '--hle', str(resources / 'games/hle_symbols.txt')]
        log.write(f'\n=== generate {module["name"]} ===\n')
        log.flush()
        saved_argv, saved_stderr = sys.argv, sys.stderr
        try:
            sys.argv = ['gen.py'] + argv
            sys.stderr = log
            gen.main(argv)
        except SystemExit as error:
            if error.code:
                raise SystemExit(f'gamebuild: lifting {module["name"]} failed: {error.code}')
        except Exception as error:  # noqa: BLE001 - reported to the player, not raised
            log.write(traceback.format_exc())
            log.flush()
            raise SystemExit(f'gamebuild: lifting {module["name"]} failed: {error}')
        finally:
            sys.argv, sys.stderr = saved_argv, saved_stderr
    report(out, 'generate', len(modules), len(modules), 'Game code read')
    return modules


def compiler_env(resources, out, link):
    """The bundled zig finds its trimmed lib/ folder here (the binary alone would not)."""
    cache = str(out / 'zig-cache')
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=cache, ZIG_LOCAL_CACHE_DIR=cache)
    if link.get('lib_dir'):
        env['ZIG_LIB_DIR'] = str(resources / link['lib_dir'])
    return env


_PGO_FLAGS = {}


_LOCAL_ISA_FLAGS = None


def local_isa_flags():
    """Safe scalar/128-bit host ISA extensions for locally-built lasgame (never redistributed).
    Enables hardware FMA (removing runtime simd_host_fma checks in runtime/simd.h) and BMI/LZCNT without
    enabling 256-bit AVX2 auto-vectorization (which regressed Zen+). Disable with MONADO_GAMEBUILD_NATIVE=0."""
    global _LOCAL_ISA_FLAGS
    if _LOCAL_ISA_FLAGS is not None:
        return _LOCAL_ISA_FLAGS
    if os.environ.get('MONADO_GAMEBUILD_NATIVE') == '0':
        _LOCAL_ISA_FLAGS = []
        return _LOCAL_ISA_FLAGS
    flags = []
    if sys.platform.startswith('linux'):
        try:
            cpuinfo = Path('/proc/cpuinfo').read_text(encoding='utf-8', errors='ignore')
            for line in cpuinfo.splitlines():
                if line.startswith('flags'):
                    feats = set(line.split(':', 1)[1].split())
                    if 'fma' in feats:
                        flags.append('-mfma')
                    if 'bmi1' in feats:
                        flags.append('-mbmi')
                    if 'bmi2' in feats:
                        flags.append('-mbmi2')
                    if 'abm' in feats or 'lzcnt' in feats:
                        flags.append('-mlzcnt')
                    break
        except OSError:
            pass
    if os.environ.get('MONADO_GAMEBUILD_THINLTO') == '1':
        flags.append('-flto=thin')
    _LOCAL_ISA_FLAGS = flags
    return _LOCAL_ISA_FLAGS


def pgo_flags(resources, link):
    """The staged profile (docs/guides/PGO.md): unprofiled / changed functions compile without one, so no warnings.
    MONADO_PGO_ID puts the profile's digest on the command line: zig's cache (zig-cache in the output folder, kept
    between builds) keys on the arguments and included files, not on the profile, and would hand back objects built
    with the previous release's profile."""
    if not link.get('pgo'):
        return []
    path = resources / link['pgo']
    if path not in _PGO_FLAGS:
        import hashlib
        digest = hashlib.sha256(path.read_bytes()).hexdigest()[:16]
        _PGO_FLAGS[path] = [f'-fprofile-use={path}', '-Wno-profile-instr-unprofiled', '-Wno-profile-instr-out-of-date',
                            '-Wno-backend-plugin', '-mllvm', '-static-func-full-module-prefix=false',
                            f'-DMONADO_PGO_ID={digest}']
    return _PGO_FLAGS[path]


# A chunk missing from compile_cost.json: about 1.3 s per MB of C (the median chunk of the reference build).
SECONDS_PER_BYTE = 1.3e-6


def compile_costs(resources):
    """{chunk: seconds} from the reference build, or {} (an old or missing file only changes the order)."""
    try:
        costs = json.loads((resources / 'games/compile_cost.json').read_text(encoding='utf-8'))['seconds']
        return {key: float(value) for key, value in costs.items()}
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return {}


def write_compile_costs(log_path, dest):
    """games/xcde/compile_cost.json from a build's gamebuild.log (chunk names and seconds only, nothing of the game)."""
    import re
    seconds = {}
    for match in re.finditer(r'\] compile (\S+\.c): exit 0 after ([0-9.]+)s', Path(log_path).read_text(errors='replace')):
        seconds[match.group(1)] = round(float(match.group(2)), 1)
    if not seconds:
        raise SystemExit(f'gamebuild: no compile times in {log_path}')
    Path(dest).write_text(json.dumps({
        'note': 'Seconds per chunk from one reference build (tools/gamebuild/gamebuild.py --write-costs). The builder starts '
                'the slowest first; a stale file only changes that order.',
        'seconds': dict(sorted(seconds.items())),
    }, indent=1) + '\n', encoding='utf-8')
    print(f'gamebuild: {len(seconds)} chunk times -> {dest}')


def compile_units(resources, out, link, jobs, log):
    """Compile every generated chunk with the bundled compiler, in parallel."""
    compiler = resources / link['compiler']
    generated = out / 'generated'
    units = sorted(generated.glob('*/*.c'))
    if not units:
        raise SystemExit('gamebuild: no generated sources')
    include = [str(resources / item) for item in link.get('include', ['include'])]
    objects = out / 'obj'
    objects.mkdir(parents=True, exist_ok=True)
    stamp = out / 'build.json'
    previous = {}
    if stamp.is_file():
        try:
            previous = json.loads(stamp.read_text(encoding='utf-8')).get('units', {})
        except (OSError, ValueError):
            previous = {}
    # The slowest chunks start first, so the end of the build is many small chunks in parallel instead of one that
    # happened to come last. Before this, sdk_090 (67 s on its own) started 80 s into the compilation and the build
    # waited for it alone. Its source size says nothing about it (a 400 KB chunk; others twice its size take 2 s), so
    # the order comes from the times of a reference build (games/compile_cost.json). Chunks it doesn't list are
    # estimated from their size. The bar moves by those expected seconds. The link keeps the sorted order.
    sizes = {source: source.stat().st_size for source in units}
    hints = compile_costs(resources)
    costs = {source: hints.get(source.relative_to(generated).as_posix(), sizes[source] * SECONDS_PER_BYTE)
             for source in units}
    total_cost = sum(costs.values()) or 1
    done = [0, 0]  # chunks, expected seconds
    progress_lock = threading.Lock()
    failures = []

    def finished(source, key):
        with progress_lock:
            done[0] += 1
            done[1] += costs[source]
            report(out, 'compile', done[0], len(units), f'Compiling {key}', done[1] / total_cost)

    def build(source):
        target = objects / source.parent.name / (source.stem + '.o')
        target.parent.mkdir(parents=True, exist_ok=True)
        key = source.relative_to(generated).as_posix()
        signature = f'{sizes[source]} {int(source.stat().st_mtime)}'
        if previous.get(key) == signature and target.is_file():
            finished(source, key)
            return key, signature, target
        command = ([str(compiler)] + list(link.get('compile', ['cc'])) + list(link.get('guest_flags', [])) +
                   local_isa_flags() + pgo_flags(resources, link) +
                   [f'-I{item}' for item in include] + ['-c', str(source), '-o', str(target)])
        result = run_tool(command, compiler_env(resources, out, link), log, f'compile {key}', COMPILE_TIMEOUT)
        if result.returncode != 0:
            failures.append(f'{key}:\n{result.stdout}{result.stderr}')
        finished(source, key)
        return key, signature, target

    slowest_first = sorted(units, key=lambda source: -costs[source])
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        built = dict(zip(slowest_first, pool.map(build, slowest_first)))
    results = [built[source] for source in units]
    if failures:
        log.write('\n'.join(failures))
        log.flush()
        raise SystemExit(f'gamebuild: {len(failures)} chunk(s) failed to compile; see {out / "gamebuild.log"}')
    return results


def link_library(resources, out, link, units, log):
    """Link the compiled chunks and the entry point into the game code library."""
    compiler = resources / link['compiler']
    entry = out / 'obj' / 'gamelib_entry.o'
    command = [str(compiler)] + list(link.get('compile', ['cc'])) + list(link.get('guest_flags', [])) + [
        f'-I{resources / item}' for item in link.get('include', ['include'])] + [
        '-DGAMELIB_ID="{}"'.format((resources / 'gamelib-id.txt').read_text(encoding='utf-8').strip()),
        '-c', str(resources / 'entry/gamelib_entry.c'), '-o', str(entry)]
    env = compiler_env(resources, out, link)
    result = run_tool(command, env, log, 'compile the entry point', COMPILE_TIMEOUT)
    if result.returncode != 0:
        log.write(result.stdout + result.stderr)
        log.flush()
        raise SystemExit('gamebuild: linking the entry point failed')
    library = out / os.path.basename(link['library'])
    inputs = ([str(entry)] + [str(target) for _, _, target in units] +
              [str(resources / extra) for extra in link.get('link_files', [])])
    if link.get('lld'):
        args = []
        for arg in link['lld']:
            if arg == '{inputs}':
                args += inputs
            else:
                args.append(arg.replace('{crt}', str(resources / 'crt')).replace('{dir}', str(out))
                            .replace('{entry}', entry.stem).replace('{name}', library.stem))
        response = out / 'link.rsp'
        response.write_text(''.join(f'"{arg}"\n' for arg in args), encoding='utf-8')
        command = [str(compiler), 'lld-link', f'@{response}']
    else:
        command = [str(compiler)] + list(link.get('link', ['cc', '-shared'])) + inputs + ['-v', '-o', str(library)]
    result = run_tool(command, env, log, 'link', LINK_TIMEOUT, stream=True)
    if result.returncode != 0:
        raise SystemExit('gamebuild: linking the game code library failed')
    # What the runtime reads before loading it (runtime/gamelib.c), and the launcher's own record.
    library.with_name(library.name + '.id').write_text(
        (resources / 'gamelib-id.txt').read_text(encoding='utf-8').strip() + '\n', encoding='utf-8')
    return library


def main(argv=None):
    parser = argparse.ArgumentParser(description='Build the game code library from a local game dump.')
    parser.add_argument('--root', help='folder holding exefs/ and romfs/, or a .nup package')
    parser.add_argument('--out', help='build folder (default: <data root>/gamebuild)')
    parser.add_argument('--resources', help='shipped gamebuild resources')
    parser.add_argument('--jobs', type=int,
                        help='parallel compilations (default: one per CPU, as many as the free memory allows)')
    parser.add_argument('--force', action='store_true', help='rebuild even when nothing changed')
    parser.add_argument('--dry-run', action='store_true', help='print the steps instead of building')
    parser.add_argument('--strict-dump', action='store_true',
                        help='refuse a romfs that differs from the reference dump (other regions are accepted '
                             'with a warning by default)')
    parser.add_argument('--write-costs', nargs=2, metavar=('LOG', 'JSON'),
                        help='write the chunk compile times of a gamebuild.log to games/xcde/compile_cost.json')
    # recomplauncher.json passes one argument list to this program on Windows and to the AppImage on Linux, whose
    # AppRun needs --prepare to pick the builder; here it changes nothing.
    parser.add_argument('--prepare', action='store_true', help='(ignored: the AppImage\'s setup-step switch)')
    # RecompLauncher's "Prepare the shaders now" tick (recomplauncher.json, prepare.options)
    parser.add_argument('--precompile', action='store_true',
                        help='then precompile the shaders with the game executable (MONADO_GAME_EXE, or build/xcde.exe)')
    args = parser.parse_args(argv)
    if args.write_costs:
        write_compile_costs(*args.write_costs)
        return 0
    if not args.root:
        parser.error('--root is required')

    dump = Path(args.root).expanduser().resolve()
    try:
        nup_package(dump)  # (a .nup that can't be used stops here, with the reason: exit code 2, bad game files)
    except (nup.NupError, OSError) as error:
        print(f'gamebuild: {error}', flush=True)
        return 2
    resources = resources_dir(args.resources)
    link = json.loads((resources / 'link.json').read_text(encoding='utf-8'))
    out = Path(args.out).expanduser().resolve() if args.out else default_data_root() / 'gamebuild'
    jobs = args.jobs or default_jobs()
    set_shares(jobs)
    if args.precompile:
        _launcher_build_share[0] = PRECOMPILE_BUILD_SHARE
    library = out / os.path.basename(link['library'])
    current = fingerprint(dump, resources, link)
    stamp = out / 'build.json'
    if not args.force and stamp.is_file() and library.is_file():
        try:
            if json.loads(stamp.read_text(encoding='utf-8')).get('fingerprint') == current:
                print(f'gamebuild: up to date: {library}')
                report(out, 'link', 1, 1, 'Already built')
                if args.precompile:
                    precompile_shaders(dump)
                return 0
        except (OSError, ValueError):
            pass
    validate(dump, resources, strict=args.strict_dump)
    if args.dry_run:
        print(f'gamebuild: would build {library} from {dump} with {resources / link["compiler"]}')
        return 0
    started = time.time()
    out.mkdir(parents=True, exist_ok=True)
    for name in ('progress.txt', 'gamebuild.log'):
        (out / name).unlink(missing_ok=True)
    with (out / 'gamebuild.log').open('w', encoding='utf-8', errors='replace') as log:
        log.write(f'gamebuild: {dump} -> {out}\n')
        log.flush()
        # GitHub #104: a crash inside unicorn or capstone (native code) ended the builder with an empty log. faulthandler
        # writes the Python stack of every thread into this log on an access violation, stack overflow or illegal
        # instruction (and on Windows also names the exception code). A fail-fast (0xC0000409) can't be caught: the
        # launcher records the exit code for that one.
        faulthandler.enable(file=log, all_threads=True)
        try:
            describe_compiler(resources, out, link, log)
            note(log, 'generate: start')
            modules = generate_modules(dump, resources, out, jobs, log)
            note(log, f'generate: done; compile: start ({jobs} at a time)')
            units = compile_units(resources, out, link, jobs, log)
            note(log, 'compile: done; link: start')
            report(out, 'link', 0, 1, 'Linking the game code')
            link_library(resources, out, link, units, log)
            note(log, 'link: done')
        except SystemExit as error:
            note(log, f'failed: {error.code}')
            raise
        except BaseException:  # noqa: BLE001 - logged, then raised as before
            note(log, 'failed with an unexpected error:\n' + traceback.format_exc())
            raise
        finally:
            faulthandler.disable()
        stamp.write_text(json.dumps({
            'fingerprint': current,
            'gamelib_id': (resources / 'gamelib-id.txt').read_text(encoding='utf-8').strip(),
            'library': str(library),
            'units': {key: signature for key, signature, _ in units},
            'modules': [m['name'] for m in modules],
        }, indent=2) + '\n', encoding='utf-8')
        # The generated C is large and reproducible from the dump; the objects keep
        # rebuilds cheap, so only the sources are dropped.
        shutil.rmtree(out / 'generated', ignore_errors=True)
    report(out, 'link', 1, 1, 'Game code ready')
    print(f'gamebuild: {library} ({time.time() - started:.0f}s)')
    if args.precompile:
        precompile_shaders(dump)
    return 0


if __name__ == '__main__':
    sys.exit(main())
