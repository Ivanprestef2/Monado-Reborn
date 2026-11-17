#!/usr/bin/env python3
"""Extract the FSR 3.1.6 shader permutation tables from the pinned FidelityFX 2.4.0
Vulkan upscaler DLL and emit a self-contained C++ source for a native Linux build.

The DLL is the only supported artifact of SDK 2.x (there are no GLSL sources or
generated permutation headers).  Each FfxShaderBlobInfo entry in the DLL stores a
deflate-compressed SPIR-V module, its resource binding names/indices, and the
indirection table that maps the 6-bit FSR3 permutation key to one of the modules.

We emit the wave32/FP32 set only.  It matches the 1.1.4 native component's
defaults and keeps the Linux port free of the SDK's runtime deflate dependency.
"""
import argparse
import hashlib
import struct
import zlib
from pathlib import Path
from types import SimpleNamespace

SDK_SHA256 = '4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7'


def parse_pe(data):
    """Minimal PE32/PE32+ section table parser (no external packages)."""
    if data[:2] != b'MZ':
        raise SystemExit('not a PE file')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise SystemExit('missing PE signature')
    coff = pe + 4
    sections = struct.unpack_from('<H', data, coff + 2)[0]
    opt_size = struct.unpack_from('<H', data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from('<H', data, opt)[0]
    if magic == 0x20b:
        image_base = struct.unpack_from('<Q', data, opt + 24)[0]
    elif magic == 0x10b:
        image_base = struct.unpack_from('<I', data, opt + 28)[0]
    else:
        raise SystemExit(f'unsupported PE optional header {magic:#x}')
    table = opt + opt_size
    out = []
    for i in range(sections):
        at = table + i * 40
        name = data[at:at + 8].rstrip(b'\0').decode('ascii', 'replace')
        vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', data, at + 8)
        out.append(SimpleNamespace(name=name, VirtualAddress=va, Misc_VirtualSize=vsize,
                                   SizeOfRawData=rawsize, PointerToRawData=rawptr))
    return image_base, out

# Base (wave32/FP32) arrays in the pinned amd_fidelityfx_upscaler_vk.dll.  The
# VAs are image-base-relative and stable for the pinned SHA-256.  The first
# array of a pass is the non-wave64 set; for prepare_inputs and accumulate the
# 64-entry indirection table is part of the set.
PASSES = [
    # name, array VA, count, table VA (0 = every key maps to entry 0)
    ('prepare_inputs',           0x1822393a0,  8, 0x182025460),
    ('prepare_reactivity',       0x18223de00,  1, 0),
    ('shading_change',           0x182239d30,  1, 0),
    ('accumulate',               0x182237100, 24, 0x181fec890),
    ('rcas',                     0x18223ae40,  1, 0),
    ('luma_pyramid',             0x18223a7a0,  1, 0),
    ('generate_reactive',        0x18223e9f0,  1, 0),
    ('shading_change_pyramid',   0x18223ad10,  1, 0),
    ('luma_instability',         0x18223a260,  1, 0),
    ('debug_view',               0x182239030,  1, 0),
]

COUNTS = (0x18, 0x40, 0x68, 0x90, 0xb8, 0xe0, 0x108)
GROUPS = (
    ('cb', 0x20),
    ('srv', 0x48),
    ('uav', 0x70),
    ('srvbuf', 0x98),
    ('uavbuf', 0xc0),
    ('sampler', 0xe8),
    ('rt', 0x110),
)
STRUCT_SIZE = 0x130


class Dll:
    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        digest = hashlib.sha256(self.data).hexdigest()
        if digest != SDK_SHA256:
            raise SystemExit(f'{path}: SHA-256 {digest} is not the pinned {SDK_SHA256}')
        self.base, self.sections = parse_pe(self.data)
        self.blobs = {}
        at = 0
        while True:
            pos = self.data.find(b'FXZF', at)
            if pos < 0:
                break
            at = pos + 4
            magic, size = struct.unpack_from('<II', self.data, pos)
            if magic != 0x465A5846 or not 1000 < size < 10_000_000:
                continue
            try:
                out = zlib.decompress(self.data[pos + 8:], 15)
            except zlib.error:
                continue
            if len(out) == size and out[:4] == b'\x03\x02\x23\x07':
                self.blobs[self.off_to_va(pos)] = out

    def off_to_va(self, off):
        for s in self.sections:
            if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
                return self.base + s.VirtualAddress + (off - s.PointerToRawData)

    def va_to_off(self, va):
        rva = va - self.base
        for s in self.sections:
            if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
                return s.PointerToRawData + (rva - s.VirtualAddress)

    def q(self, va):
        return struct.unpack_from('<Q', self.data, self.va_to_off(va))[0]

    def u32(self, va):
        return struct.unpack_from('<I', self.data, self.va_to_off(va))[0]

    def cstr(self, va):
        off = self.va_to_off(va)
        return self.data[off:off + 512].split(b'\0')[0].decode('ascii')

    def strings(self, va, count):
        off = self.va_to_off(va)
        ptrs = struct.unpack_from('<%dQ' % count, self.data, off)
        return [self.cstr(p) for p in ptrs]

    def u32s(self, va, count):
        off = self.va_to_off(va)
        return list(struct.unpack_from('<%dI' % count, self.data, off))

    def blob_info(self, va):
        """Parse one FfxShaderBlobInfo at va."""
        off = self.va_to_off(va)
        blob_size = struct.unpack_from('<I', self.data, off)[0]
        blob_data = struct.unpack_from('<Q', self.data, off + 8)[0]
        entry = struct.unpack_from('<Q', self.data, off + 0x10)[0]
        counts = [struct.unpack_from('<I', self.data, off + o)[0] for o in COUNTS]
        if blob_data not in self.blobs:
            raise SystemExit(f'{va:#x}: blob data {blob_data:#x} not found')
        # blobSize is the compressed total size (FXZF header + zlib payload);
        # the decompressed module is what we emit, so it is not used here.
        groups = {}
        for (name, name_off), count in zip(GROUPS, counts):
            if count == 0:
                groups[name] = None
                continue
            names_va = struct.unpack_from('<Q', self.data, off + name_off)[0]
            bind_va = struct.unpack_from('<Q', self.data, off + name_off + 8)[0]
            cnt_va = struct.unpack_from('<Q', self.data, off + name_off + 16)[0]
            space_va = struct.unpack_from('<Q', self.data, off + name_off + 24)[0]
            groups[name] = (self.strings(names_va, count), self.u32s(bind_va, count),
                            self.u32s(cnt_va, count), self.u32s(space_va, count))
        return dict(va=va, data=self.blobs[blob_data], entry=self.cstr(entry) if entry else 'CS',
                    counts=counts, groups=groups)


def c_bytes(data):
    return ','.join(str(b) for b in data)


def emit_blob(out, tag, info):
    out.append(f'alignas(4) static const uint8_t spv_{tag}[] = {{{c_bytes(info["data"])}}};')
    for name, _ in GROUPS:
        group = info['groups'][name]
        if group is None:
            continue
        names, bindings, counts, spaces = group
        out.append(f'static const char* names_{tag}_{name}[] = {{' + ','.join(f'"{n}"' for n in names) + '};')
        out.append(f'static const uint32_t bindings_{tag}_{name}[] = {{{",".join(map(str, bindings))}}};')
        out.append(f'static const uint32_t counts_{tag}_{name}[] = {{{",".join(map(str, counts))}}};')
        out.append(f'static const uint32_t spaces_{tag}_{name}[] = {{{",".join(map(str, spaces))}}};')
    fields = []
    for name, _ in GROUPS:
        if info['groups'][name] is None:
            fields.extend(['nullptr'] * 4)
        else:
            fields.extend([f'names_{tag}_{name}', f'bindings_{tag}_{name}',
                           f'counts_{tag}_{name}', f'spaces_{tag}_{name}'])
    counts = info['counts']
    out.append(
        f'static const FfxShaderBlob blob_{tag} = {{\n'
        f'  spv_{tag}, sizeof(spv_{tag}), "{info["entry"]}",\n'
        f'  {counts[0]}, {counts[1]}, {counts[2]}, {counts[3]}, {counts[4]}, {counts[5]}, {counts[6]},\n'
        f'  ' + ',\n  '.join(', '.join(fields[i:i + 4]) for i in range(0, len(fields), 4)) + '\n};')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--dll', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    args = ap.parse_args()

    dll = Dll(args.dll)
    out = ['// Generated by tools/extract_fsr3_240_blobs.py from the pinned FidelityFX 2.4.0 DLL.',
           '// Do not edit; the DLL SHA-256 and the selected permutation arrays are pinned.',
           '#include "ffx_fsr3upscaler_shaderblobs.h"',
           '#include <cstring>',
           '']

    tables = {}
    for name, va, count, table_va in PASSES:
        infos = [dll.blob_info(va + i * STRUCT_SIZE) for i in range(count)]
        for i, info in enumerate(infos):
            emit_blob(out, f'{name}_{i}', info)
        out.append(f'static const FfxShaderBlob* const {name}_blobs[] = {{' +
                   ','.join(f'&blob_{name}_{i}' for i in range(count)) + '};')
        if table_va:
            table = dll.u32s(table_va, 64)
            out.append(f'static const uint32_t {name}_indirection[64] = {{' + ','.join(map(str, table)) + '};')
            assert max(table) < count, (name, max(table), count)
            tables[name] = True
        else:
            tables[name] = False

    out.append(r'''
extern "C" FfxErrorCode fsr3UpscalerGetPermutationBlobByIndex(
    FfxFsr3UpscalerPass passId, uint32_t permutationOptions, FfxShaderBlob* outBlob)
{
    if (!outBlob) return FFX_ERROR_INVALID_ARGUMENT;
    const uint32_t key = permutationOptions & 0x3f;
    const FfxShaderBlob* selected = nullptr;
    switch (passId)
    {
    case FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS:
        selected = prepare_inputs_blobs[prepare_inputs_indirection[key]]; break;
    case FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY:
        selected = prepare_reactivity_blobs[0]; break;
    case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE:
        selected = shading_change_blobs[0]; break;
    case FFX_FSR3UPSCALER_PASS_ACCUMULATE:
    case FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN:
        selected = accumulate_blobs[accumulate_indirection[key]]; break;
    case FFX_FSR3UPSCALER_PASS_RCAS:
        selected = rcas_blobs[0]; break;
    case FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID:
        selected = luma_pyramid_blobs[0]; break;
    case FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE:
        selected = generate_reactive_blobs[0]; break;
    case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID:
        selected = shading_change_pyramid_blobs[0]; break;
    case FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY:
        selected = luma_instability_blobs[0]; break;
    case FFX_FSR3UPSCALER_PASS_DEBUG_VIEW:
        selected = debug_view_blobs[0]; break;
    default:
        break;
    }
    if (!selected) { std::memset(outBlob, 0, sizeof(*outBlob)); return FFX_ERROR_INVALID_ARGUMENT; }
    std::memcpy(outBlob, selected, sizeof(*outBlob));
    return FFX_OK;
}

extern "C" FfxErrorCode fsr3UpscalerIsWave64(uint32_t, bool& isWave64)
{
    // The native Linux port ships the wave32 set; the Vulkan backend does not
    // request 64-lane subgroups (shader model 5.1 capability report).
    isWave64 = false;
    return FFX_OK;
}
''')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('\n'.join(out) + '\n')
    print(f'wrote {args.output}: {len(dll.blobs)} blobs parsed')


if __name__ == '__main__':
    main()
