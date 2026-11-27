"""[XCDE] T-LINUX-APPIMAGE: package this checkout's Linux build as one AppImage (never the dump).

The package holds the recompiled game code of THIS build (generated from your own dump), so it is for your own use:
do not redistribute it (CONTRIBUTING.md, art. I). It contains build/linux/monado, the launcher, the optional native
FSR 3 / FSR 4 Lite components when they were built (with their sources and licences), the shader variant seed, and
the non-system shared libraries they need. GPU drivers, the Vulkan loader and glibc always come from the host.

  python3 tools/package/make_appimage.py [--output build/Monado-x86_64.AppImage] [--appimagetool appimagetool]

--release packages a runtime built with CMake -DMONADO_RELEASE=ON instead: it holds NO game code and can be shared.
The launcher builds the game code once from the player's own dump with the packaged builder (build/gamebuild:
tools/gamebuild/gamebuild_stage.py + tools/gamebuild/freeze_gamebuild.py). The packaged tree is checked with tools/package/package_guard.py.
Needs appimagetool (https://github.com/AppImage/appimagetool) on PATH or given with --appimagetool.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
# The GLib / GTK desktop stack is the host's too (GitHub #99). The host's own GTK (which SDL3 loads in the launcher)
# and its plugins (gvfs GIO modules, gdk-pixbuf loaders such as the SVG one) are built against the host's GLib, so
# bundled copies from the build runner shadowed them through LD_LIBRARY_PATH: on Debian testing, libgvfsdbus.so found
# no g_variant_builder_init_static, libpixbufloader_svg.so no rsvg_handle_get_pixbuf_and_error, and the launcher
# aborted in GTK. They came in through the distro's full FFmpeg (librsvg, cairo, pango...); the release links none.
DESKTOP = re.compile(r"^lib(glib|gobject|gio|gmodule|gthread|girepository)-2\.0\.so|"
                     r"^lib(gdk_pixbuf|gdk|gtk|rsvg|atk|atk-bridge|atspi|epoxy|glycin)[-_.]|"
                     r"^lib(cairo|cairo-gobject|pango|pangocairo|pangoft2|harfbuzz|fontconfig|freetype|fribidi|thai|"
                     r"datrie|graphite2|pixman)[-_.0-9]*\.so|"
                     r"^lib(mount|blkid|uuid|selinux|pcre2-8|dbus-1|systemd)\.so")
# Host ABI / graphics driver components must not be replaced by packaged copies. That includes what the GPU drivers
# themselves link against: AppRun puts usr/lib first on LD_LIBRARY_PATH, so a bundled copy older than the host's (the
# CI builds on Ubuntu 24.04) is what Mesa gets too. RADV then failed to load (libxcb-dri3: undefined symbol
# xcb_dri3_import_syncobj_checked) and the launcher found no GPU on SteamOS and other current distributions.
HOST = re.compile(r"^(ld-linux|lib(c|m|pthread|dl|rt|resolv|util|anl)\.so|libnss_|libvulkan\.so|"
                  r"libnvidia-ngx[-.]|"
                  r"lib(GL|EGL|GLX|OpenGL|GLdispatch|drm|gbm)[_.]|"
                  r"lib(stdc\+\+|gcc_s|z|expat|ffi|lzma|zstd|elf|udev|xshmfence)\.so|"
                  r"lib(X11|X11-xcb|Xau|Xdmcp|Xext|Xfixes|Xrender|Xxf86vm|xcb|xcb-[a-z0-9]+|wayland-[a-z]+)\.so)|" +
                  DESKTOP.pattern)


def linked(binary):
    """Names of every shared library the binary loads (ldd: direct and indirect)."""
    out = subprocess.run(['ldd', str(binary)], text=True, capture_output=True, check=True).stdout
    return {Path(m[1]).name for m in re.finditer(r'^\s*(\S+)\s', out, re.M)}


def dependencies(binary):
    result = subprocess.run(['ldd', str(binary)], text=True, capture_output=True, check=True)
    if 'not found' in result.stdout:
        raise RuntimeError('Unresolved shared libraries:\n' + result.stdout)
    paths = []
    for line in result.stdout.splitlines():
        match = re.search(r'=> (/\S+)', line) or re.match(r'\s*(/\S+)', line)
        if match and not HOST.match(Path(match[1]).name):
            paths.append(Path(match[1]))
    return paths


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', type=Path, default=ROOT / 'build/linux', help='scripts/build_linux.sh output')
    ap.add_argument('--output', type=Path, default=ROOT / 'build/Monado-x86_64.AppImage')
    ap.add_argument('--appimagetool', default='appimagetool')
    ap.add_argument('--release', action='store_true', help='runtime without game code + the first-start builder')
    ap.add_argument('--gamebuild', type=Path, default=ROOT / 'build/gamebuild', help='staged builder (--release)')
    a = ap.parse_args()
    build, output = a.build.resolve(), a.output.resolve()
    for name in ('monado', 'monado-launcher'):
        if not (build / name).is_file():
            ap.error(f'missing {build / name}: run scripts/build_linux.sh first (docs/guides/LINUX.md)')
    if shutil.which(a.appimagetool) is None and not Path(a.appimagetool).is_file():
        ap.error(f'{a.appimagetool} not found (https://github.com/AppImage/appimagetool)')
    if a.release:
        gamebuild = a.gamebuild.resolve()
        for name in ('link.json', 'gamelib-id.txt', 'monado-gamebuild'):
            if not (gamebuild / name).is_file():
                ap.error(f'missing {gamebuild / name}: run tools/gamebuild/gamebuild_stage.py and tools/gamebuild/freeze_gamebuild.py')
        # The release runtime must not contain lifted game functions (f_<guest address>).
        symbols = subprocess.run(['nm', '--defined-only', str(build / 'monado')], text=True, capture_output=True).stdout
        if re.search(r' f_7[0-9a-f]{9}\b', symbols):
            ap.error(f'{build / "monado"} contains game code: build it with CMake -DMONADO_RELEASE=ON')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='monado-appdir-', dir=output.parent) as temp:
        appdir = Path(temp)  # (fresh: no stale files, caches or dumps can enter the package)
        bin_, lib, share = appdir / 'usr/bin', appdir / 'usr/lib', appdir / 'usr/share/monado'
        docs = appdir / 'usr/share/doc/monado'
        for d in (bin_, lib, share, docs):
            d.mkdir(parents=True)
        payloads = []
        for name in ('monado', 'monado-launcher'):
            shutil.copy2(build / name, bin_ / name)
            payloads.append(build / name)
        fsr3, fsr4 = build / 'libmonado_fsr3.so', build / 'amd_fidelityfx_upscaler_vk.so'
        if fsr3.is_file():
            shutil.copy2(fsr3, lib / fsr3.name)
            payloads.append(fsr3)
            shutil.copytree(ROOT / 'third_party/fsr3_native', docs / 'fsr3-source')
            shutil.copy2(ROOT / 'tools/build/build_fsr3_linux.py', docs / 'fsr3-source/build_fsr3_linux.py')
        else:
            print('note: no native FSR 3 component (scripts/build_fsr3_linux.sh): packaged without it')
        dlss_lib = build / 'libmonado_dlss.so'
        dlss_ngx = build / 'libnvidia-ngx-dlss.so.310.9.1'
        if dlss_lib.is_file():
            shutil.copy2(dlss_lib, lib / dlss_lib.name)
            payloads.append(dlss_lib)
        else:
            print('note: no native Linux DLSS adapter (tools/package/setup_linux_dlss.py, scripts/build_dlss_linux.sh): packaged without it')
        if dlss_ngx.is_file():
            (share / 'dlss').mkdir(parents=True, exist_ok=True)
            shutil.copy2(dlss_ngx, share / 'dlss' / dlss_ngx.name)
            if (build / 'dlss-LICENSE.txt').is_file():
                shutil.copy2(build / 'dlss-LICENSE.txt', docs / 'dlss-LICENSE.txt')
        if fsr4.is_file():
            shutil.copy2(fsr4, lib / fsr4.name)
            payloads.append(fsr4)
            for notice in ('fsr4-LICENSE', 'fsr4-THIRD_PARTY_NOTICES.md'):
                if (build / notice).is_file():
                    shutil.copy2(build / notice, docs / notice)
        else:
            print('note: no native FSR 4 component (scripts/build_fsr4_linux.sh): packaged without it')
        if a.release:  # (a shared package must not need the host's desktop stack: build FFmpeg minimal, #107)
            desktop = sorted({f'{p.name}: {n}' for p in payloads for n in linked(p) if DESKTOP.match(n)})
            if desktop:
                raise SystemExit('release payloads link the GLib/GTK desktop stack (scripts/build_ffmpeg.sh):\n  ' +
                                 '\n  '.join(desktop))
        for dep in {d for p in payloads for d in dependencies(p)}:
            target = lib / dep.name
            if target.exists() and target.read_bytes() != dep.read_bytes():
                raise RuntimeError(f'library name collision: {dep.name}')
            shutil.copy2(dep, target)
        for name in ('AppRun', 'monado.desktop', 'monado.svg'):
            shutil.copy2(ROOT / 'packaging/linux' / name, appdir / name)
        # RecompLauncher reads /recomplauncher.json from the AppImage itself (no file beside it in a release)
        shutil.copy2(ROOT / 'recomplauncher.json', appdir / 'recomplauncher.json')
        shutil.copy2(ROOT / 'data/variants_seed.bin', share / 'variants_seed.bin')
        if a.release:  # the first-start builder and its resources (AppRun exports XCDE_GAMEBUILD_DIR)
            shutil.copytree(a.gamebuild.resolve(), share / 'gamebuild', symlinks=False,
                            ignore=shutil.ignore_patterns('obj', 'generated', 'progress.txt', '*.log'))
            (share / 'gamebuild/monado-gamebuild').chmod(0o755)
            guard = subprocess.run(['python3', str(ROOT / 'tools/package/package_guard.py'), str(appdir)], text=True)
            if guard.returncode != 0:
                raise SystemExit('package_guard: the AppImage tree holds game or derived files (see above)')
        for name in ('LICENSE', 'NOTICE.md'):
            shutil.copy2(ROOT / name, docs / name)
        for source in (ROOT / 'third_party').rglob('*'):  # (bundled source dependencies' licence notices)
            if source.is_file() and source.name.lower().startswith(('license', 'copying', 'copyright')):
                target = docs / source.relative_to(ROOT)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        # Never truncate a running AppImage: build beside it, then replace it in one step.
        with tempfile.TemporaryDirectory(prefix='monado-output-', dir=output.parent) as dest:
            staged = Path(dest) / output.name
            subprocess.run([a.appimagetool, str(appdir), str(staged)], env=dict(os.environ, ARCH='x86_64'), check=True)
            os.replace(staged, output)
    print(f'Created {output} ' + ('(release: no game code; the launcher builds it from the player\'s dump)' if a.release
                                  else '(contains the game code recompiled from your dump: for your own use only)'))


if __name__ == '__main__':
    main()
