#!/usr/bin/env python3
"""Build Monado's native Linux FSR 4.1.2 Vulkan component (amd_fidelityfx_upscaler_vk.so) from auz466/fsr4up-vk.

Downloads AMD's MIT-licensed amd_fidelityfx_upscaler_vk.dll (FidelityFX SDK 2.4.0, SHA-256 checked), reads its
SPIR-V shaders and initializer data at build time (no Proton / Wine / GPU recording needed), and builds the
self-contained native Linux shared library with FP8 (RDNA 4) and INT8 / Lite (RDNA 2, RDNA 3, Intel Arc) networks.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
URL = 'https://github.com/auz466/fsr4up-vk.git'
REVISION = 'bef010431d775de1d07e1145d387798ecf84c47b'
DLL_URL = ('https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/'
           'v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll')
DLL_SHA256 = '4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7'


def run(args, env=None):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, env=env)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def ensure_checkout(checkout):
    if (checkout / '.git').exists():
        cur = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
        if cur == REVISION:
            return
        run(['git', '-C', str(checkout), 'fetch', '--depth', '1', 'origin', REVISION])
        run(['git', '-C', str(checkout), 'checkout', '-q', REVISION])
        return
    checkout.parent.mkdir(parents=True, exist_ok=True)
    run(['git', 'clone', '--no-checkout', '--depth', '1', URL, str(checkout)])
    run(['git', '-C', str(checkout), 'fetch', '--depth', '1', 'origin', REVISION])
    run(['git', '-C', str(checkout), 'checkout', '-q', REVISION])


def ensure_dll(path):
    if path.is_file() and sha256_file(path) == DLL_SHA256:
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    part = path.with_suffix(path.suffix + '.part')
    print(f'FSR4: downloading amd_fidelityfx_upscaler_vk.dll (FidelityFX SDK 2.4.0, 36 MB)...', flush=True)
    req = urllib.request.Request(DLL_URL, headers={'User-Agent': 'Monado-FSR4-Build'})
    with urllib.request.urlopen(req, timeout=300) as resp, open(part, 'wb') as out:
        shutil.copyfileobj(resp, out, 1 << 20)
    digest = sha256_file(part)
    if digest != DLL_SHA256:
        part.unlink(missing_ok=True)
        raise SystemExit(f'FSR4: amd_fidelityfx_upscaler_vk.dll SHA-256 {digest}, expected {DLL_SHA256}')
    os.replace(part, path)
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--src', type=Path, default=ROOT / 'build/reference/fsr4up-vk',
                    help='fsr4up-vk checkout directory')
    ap.add_argument('--dll', type=Path,
                    default=Path(os.environ['MONADO_FSR4_AMD_DLL']) if os.environ.get('MONADO_FSR4_AMD_DLL') else None,
                    help='path to AMD amd_fidelityfx_upscaler_vk.dll (FidelityFX SDK 2.4.0)')
    ap.add_argument('--work', type=Path, default=ROOT / 'build/fsr4up/build',
                    help='CMake build directory for fsr4up-vk')
    ap.add_argument('--output', type=Path, default=ROOT / 'build/linux/amd_fidelityfx_upscaler_vk.so',
                    help='output path for amd_fidelityfx_upscaler_vk.so')
    a = ap.parse_args()

    src = a.src.resolve()
    ensure_checkout(src)

    if a.dll:
        dll = a.dll.resolve()
        if not dll.is_file():
            raise SystemExit(f'FSR4: {dll} does not exist')
        digest = sha256_file(dll)
        if digest != DLL_SHA256:
            raise SystemExit(f'FSR4: {dll} SHA-256 {digest}, expected {DLL_SHA256}')
    else:
        dll = ensure_dll((ROOT / 'build/fsr4up/amd_fidelityfx_upscaler_vk.dll').resolve())

    work = a.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    cfg = ['cmake', '-S', str(src), '-B', str(work),
           '-DCMAKE_BUILD_TYPE=Release',
           f'-DFSR4UP_AMD_DLL={dll}',
           '-DFSR4UP_BUILD_HARNESS=OFF',
           '-DFSR4UP_BUILD_HARNESS_EXE=OFF']
    if not (work / 'CMakeCache.txt').is_file():
        cfg += ['-G', 'Ninja' if shutil.which('ninja') else 'Unix Makefiles']
    env = dict(os.environ)
    if not (work / 'CMakeCache.txt').is_file() and not env.get('CXX') and shutil.which('clang++'):
        env['CXX'] = 'clang++'
    run(cfg, env=env)
    run(['cmake', '--build', str(work), '--target', 'fsr4up', f'-j{os.cpu_count() or 4}'], env=env)

    built = work / 'amd_fidelityfx_upscaler_vk.so'
    if not built.is_file():
        raise SystemExit(f'FSR4: build succeeded but {built} is missing')

    out = a.output.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(built, out)
    # Keep fsr4up-vk's licence and AMD MIT third-party notice beside the built library for packaging.
    for src_name, dst_name in (('LICENSE', 'fsr4-LICENSE'), ('THIRD_PARTY_NOTICES.md', 'fsr4-THIRD_PARTY_NOTICES.md')):
        notice = src / src_name
        if notice.is_file():
            shutil.copy2(notice, out.parent / dst_name)
    print(f'Built {out}', flush=True)


if __name__ == '__main__':
    main()
