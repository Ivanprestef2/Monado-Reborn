"""T253: build/test modified components without linking the game.

python tools/diag/validate_port.py [--build-ui]
Uses project-local build/toolchain/ziglang and build/deps/ucrt64, or XCDE_ZIG / XCDE_SDK.
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--build-ui', action='store_true')
    parser.add_argument('--compile-only', action='store_true', help='Build native tests without executing unsigned binaries')
    parser.add_argument('--clang', type=Path, help='Optional standalone Clang driver (same Windows GNU ABI); default remains Zig')
    parser.add_argument('--skip-test', action='append', default=[], help='Report a native test as skipped, for an external execution restriction')
    args = parser.parse_args()
    os.chdir(ROOT)
    output = ROOT / ('build/validation-clang' if args.clang else 'build/validation')
    output.mkdir(parents=True, exist_ok=True)
    temp = ROOT / 'build/tmp'
    temp.mkdir(parents=True, exist_ok=True)
    os.environ.update(TEMP=str(temp), TMP=str(temp), ZIG_GLOBAL_CACHE_DIR=str(ROOT / 'build/zig-cache-validated'), ZIG_LOCAL_CACHE_DIR=str(ROOT / 'build/zig-local'))
    zig = os.environ.get('XCDE_ZIG', str(ROOT / 'build/toolchain/ziglang/zig.exe'))
    sdk = Path(os.environ.get('XCDE_SDK', ROOT / 'build/deps/ucrt64'))
    if not (args.clang.is_file() if args.clang else Path(zig).is_file()) or not (sdk / 'include/vulkan/vulkan.h').is_file():
        parser.error('Missing Zig/SDK. See docs/history/port/PORT_VALIDATION.md for project-local setup.')
    inc = ['-Iruntime', '-Iruntime/nvn', '-Ithird_party', '-Ithird_party/imgui',
           '-Ithird_party/imgui/backends', '-Ithird_party/streamline/include']
    def includes(cpp=False):
        # Zig's libc++ ships its own C headers (string.h, math.h, ...). They have to be found before the SDK's CRT
        # headers, or libc++ mixes two libc header sets and fails to compile; -idirafter still finds SDL3/Vulkan/VMA.
        sdk_inc = ['-idirafter', str(sdk / 'include')] if cpp else [f'-I{sdk / "include"}']
        return [*inc, *sdk_inc]
    failures = []
    skipped = []
    def compiler(cpp=False):
        if args.clang:
            prefix=args.clang.resolve().parent.parent
            gcc_versions=list((prefix/'lib/gcc/x86_64-w64-mingw32').glob('*'))
            gcc=max(gcc_versions,key=lambda p: tuple(int(v) for v in p.name.split('.'))) if gcc_versions else None
            flags=[str(args.clang.resolve()), '--driver-mode=g++' if cpp else '--driver-mode=gcc',
                   f'--sysroot={sdk}', f'-L{sdk / "lib"}', '-fuse-ld=lld', '-march=x86-64-v2']
            if gcc:
                flags += [f'-L{gcc}', f'-L{prefix / "lib"}']
                if cpp:
                    include=prefix/'include/c++'/gcc.name
                    flags += ['-isystem', str(include), '-isystem', str(include/'x86_64-w64-mingw32'),
                              '-isystem', str(include/'backward')]
            return flags
        return [zig, 'c++' if cpp else 'cc', '-march=x86_64_v2']
    for dll in ['SDL3.dll', 'vulkan-1.dll', 'libwinpthread-1.dll', 'libshaderc_shared.dll']:
        if (sdk / 'bin' / dll).is_file():
            shutil.copy2(sdk / 'bin' / dll, output / dll)

    def run(name, command):
        with (output / f'{name}.log').open('w', encoding='utf-8') as log:
            try:
                result = subprocess.run([str(a) for a in command], stdout=log, stderr=subprocess.STDOUT, timeout=300)
                code = result.returncode
            except (OSError, subprocess.TimeoutExpired) as error:
                log.write(f'Execution unavailable: {error}\n')
                code = 1
        print(f'{name}: {"PASS" if not code else "FAIL"}', flush=True)
        if code:
            failures.append(name)
            print((output / f'{name}.log').read_text(encoding='utf-8', errors='replace')[-3000:])
        return code == 0

    sources = ['host/settings_ini.c', 'hle/framepace.c', 'hle/party.c', 'hle/bindings.c', 'hle/collision_bvh.c', 'services/fs.c', 'diag/diag.c', 'log.c', 'nvn/objects.c', 'services/romfs_virtual.c', 'services/game_files.c', 'nvn/output_scale.c',
               'nvn/vulkan/device.c', 'nvn/vulkan/exec.c', 'nvn/vulkan/post.c', 'nvn/vulkan/shaders.c',
               'nvn/vulkan/textures.c', 'nvn/vulkan/precompile.c', 'nvn/vulkan/precompile_sig.c', 'nvn/vulkan/temporal.c', 'nvn/vulkan/vkq.c', 'nvn/vulkan/fsr4.c',
               'nvn/vulkan/imgui_glue.cpp', 'ui/menu_vk.cpp', 'nvn/vulkan/dlss.cpp']
    for source in sources:
        cpp = source.endswith('.cpp')
        # [XCDE] T282: the build generates <build>/inc/shader_sig.h for the precompile fingerprint; a validation compile
        # does not need it, so the unit gets the macro instead.
        extra = ['-DXCDE_SHADER_SIG="validation"'] if source.endswith('precompile_sig.c') else []
        run(source.replace('/', '_'), [*compiler(cpp), '-O3', '-c', '-w',
            '-std=c++17' if cpp else '-std=gnu11', *includes(cpp), *extra, ROOT / 'runtime' / source,
            '-o', output / (source.replace('/', '_') + '.o')])
    extra_sources = {
        'test_settings_roundtrip': ['runtime/host/settings_ini.c', 'runtime/hle/cheats_options.c', 'runtime/hle/graphics.c', 'runtime/hle/foliage_distance.c',
                                    str(sdk / 'lib/libwinpthread.dll.a')],
        'test_output_scale': ['runtime/nvn/output_scale.c'],
        'test_log_timestamp': ['runtime/log.c'],
        'test_host_fma': ['-O1', '-fno-strict-aliasing'],  # [XCDE] FMA3 path vs the library fma
        'test_texture_index': ['-O1', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections'],
        'test_romfs_cache': ['-O1', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', str(sdk / 'lib/libwinpthread.dll.a')],
        'test_translate_tex': ['runtime/nvn/maxwell/translate.c'],
        'test_translate_ldc': ['runtime/nvn/maxwell/translate.c'],
        'test_translate_xfb': ['runtime/nvn/maxwell/translate.c'],
        'test_translate_cfg': ['runtime/nvn/maxwell/translate.c', str(sdk / 'lib/libshaderc_shared.dll.a')],
        # [XCDE] T282 review: parallel translations must match the serial one (the precompile is multi-threaded).
        'test_translate_threads': ['runtime/nvn/maxwell/translate.c', str(sdk / 'lib/libwinpthread.dll.a')],
        'test_tegra_copy': ['runtime/nvn/tegra.c', 'runtime/nvn/formats.c'],
        'test_tegra_block_height': ['runtime/nvn/tegra.c', 'runtime/nvn/formats.c'],
        'test_fibers': ['runtime/hle/fibers.c'],
        'test_condvar': ['runtime/ipc.c', 'runtime/platform/windows.c', 'runtime/log.c', 'runtime/diag/diag.c', 'runtime/diag/sampler.c', '-lwinmm', '-ldbghelp'],
        'test_foliage_distance': ['runtime/hle/foliage_distance.c'],
        'test_graphics_options': ['runtime/hle/graphics.c'],
        # [XCDE] T037: atomic save writes/commits; fs.c is included by the test.
        'test_fs_commit': ['-O1', str(sdk / 'lib/libwinpthread.dll.a')],
        # [XCDE] T282: the precompile signature. Its own source unit compiles with the macro instead of the header.
        'test_precompile_sig': ['runtime/nvn/vulkan/precompile_sig.c', '-DXCDE_SHADER_SIG="native"'],
    }
    strict_tests = {
        'test_cliff_batch', 'test_display_options', 'test_foliage_distance', 'test_gpu_probe',
        'test_graphics_options', 'test_port_core', 'test_render_policy', 'test_precompile_sig',
        'test_settings_roundtrip', 'test_stick_click', 'test_tex_resize', 'test_ui_scale', 'test_window_size',
    }
    standalone_tests = [
        'test_cliff_batch', 'test_display_options', 'test_port_core', 'test_gpu_probe',
        'test_render_policy', 'test_settings_roundtrip', 'test_stick_click', 'test_tex_resize',
        'test_time_math', 'test_movie_buf', 'test_ui_scale', 'test_window_size', 'test_utf8_paths',
    ]  # (test_precompile_sig is built from extra_sources above: it needs its own source unit + macro; do not list it twice)
    for test in [*standalone_tests, *[k for k in extra_sources if k != 'test_settings_roundtrip']]:
        if test in args.skip_test:
            skipped.append(test)
            print(f'{test}: SKIPPED (explicit --skip-test)', flush=True)
            continue
        exe = output / f'{test}.exe'
        extra = extra_sources.get(test, [])
        if test in ('test_tegra_copy', 'test_tegra_block_height', 'test_condvar'):
            extra = [*extra, str(sdk / 'lib/libwinpthread.dll.a')]
        warnings = ['-Wall', '-Wextra', '-Werror'] if test in strict_tests else ['-w']
        if run(test + '_build', [*compiler(), '-std=gnu11', *warnings, *includes(), '-Iruntime/nvn/maxwell',
                                ROOT / 'tests/native' / f'{test}.c', *extra, '-o', exe]):
            if args.compile_only:
                skipped.append(test)
                print(f'{test}: NOT EXECUTED (--compile-only)', flush=True)
            else:
                run(test, [exe, output / 'test-settings.ini'] if test in ('test_port_core', 'test_settings_roundtrip') else [exe, output] if test in ('test_romfs_cache', 'test_utf8_paths') else [exe])
    libs = [sdk / 'lib/libSDL3.dll.a', sdk / 'lib/libvulkan-1.dll.a']
    signature_test = output / 'test_nvidia_signature.exe'
    if run('test_nvidia_signature_build', [*compiler(True), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                          'tests/native/test_nvidia_signature.cpp', '-o', signature_test]):
        if not args.compile_only and (ROOT / 'build/sl.interposer.dll').is_file():
            run('test_nvidia_signature', [signature_test, ROOT / 'build/sl.interposer.dll'])
        else:
            skipped.append('test_nvidia_signature')
            print('test_nvidia_signature: NOT EXECUTED (compile-only or official DLL absent)', flush=True)
    run('gpu_diagnostics_build', [*compiler(), '-std=c11', *includes(), 'tools/diag/gpu_diagnostics.c', *libs,
                                  '-o', output / 'gpu_diagnostics.exe'])
    if args.build_ui:
        files = ['imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp',
                 'backends/imgui_impl_sdl3.cpp', 'backends/imgui_impl_sdlrenderer3.cpp']
        objects = []
        for source in [*(str(ROOT / 'third_party/imgui' / f) for f in files), str(ROOT / 'tools/launcher/launcher.cpp')]:
            obj = output / (Path(source).stem + '.o')
            if run(Path(source).stem + '_build', [*compiler(True), '-O3', '-std=c++17', '-w', *includes(True), '-c', source, '-o', obj]):
                objects.append(obj)
        if len(objects) == len(files) + 1:
            icon_obj = output / 'monado_icon.o'
            if run('launcher_icon', [sys.executable, str(ROOT / 'tools/build/ico_to_coff.py'),
                                     str(ROOT / 'packaging/windows/monado.ico'), str(icon_obj)]):
                objects.append(icon_obj)
            run('launcher_link', [*compiler(True), *objects, *libs, '-static-libstdc++', '-static-libgcc',
                                  '-o', output / 'xcde_launcher.exe', '-Wl,--subsystem,windows'])
    print(f'Logs/artifacts: {output}; failures: {len(failures)}; skipped: {len(skipped)}', flush=True)
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
