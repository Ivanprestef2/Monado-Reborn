# FidelityFX SDK 2.4.0: research and migration plan

Status: research prototype, branch `research/ffx-sdk-2.4.0` (2026-11-17).
Scope: AMD FidelityFX / FSR integrations in Monado. This document is the
research report; the branch also contains the validated API-header prototype
(section 5) and the validated Linux native FSR 3.1.6 extraction prototype
(section 6.2).

## 1. Executive summary

- **The latest SDK is AMD FSR SDK v2.4.0** (`7539ae8b6568ad29ed78a753404351daf3b5cca7`,
  released 2026-10-28). It is a major change from the SDK 1.1.4 we build the
  Windows FSR 3 runtime from:
  - FSR Upscaling **4.1.2** (including RX 6000 / RDNA 2 and Strix Halo support),
  - FidelityFX Super Resolution **3.1.6** upscaler,
  - FSR Frame Generation **4.0.2** (ML) and Frame Generation Swapchain **4.0.0**,
  - FSR 3.1.6 FG and 3.1.7 FG swapchain,
  - Vulkan support at parity with DX12.
- **Windows: updating is worthwhile and practical.** The 2.4.0 distribution is a
  loader DLL plus signed effect DLLs (the single `amd_fidelityfx_vk.dll` is gone).
  The API change is mostly additive but requires three descriptors that 1.1.4 did
  not have, plus the new loader name. The prototype in this branch compiles with
  the 2.4.0 headers, keeps the Linux 1.1.4 native component working, and passes
  the native FSR 3 regression test.
- **Linux native FSR 3 cannot simply be bumped to 2.4.0, but a working
  extraction prototype now exists.** SDK 2.x no longer ships GLSL shaders,
  generated shader-permutation headers, or a CMake build; the effects are
  prebuilt Windows DLLs. The branch now contains a Linux-only opt-in path:
  `tools/extract_fsr3_240_blobs.py` reads the 2.4.0 Vulkan upscaler DLL's own
  `FfxShaderBlobInfo` arrays and permutation tables, decompresses the selected
  wave32/FP32 SPIR-V, and the 2.4.0 host/backend compile against
  `adapter_240.cpp`. It reports FSR 3.1.6 and passes the native regression with
  both backend descriptor forms. A first in-game run (NVIDIA GTX 1050 Ti,
  driver 580.196.07) showed no visible regression and possibly slightly less
  shimmering; performance is not measured yet. Recommendation: **keep 1.1.4 as
  the default Linux component** until the 3.1.6 path has a performance
  comparison, and keep FSR 4 Lite as the Linux ML path.
- **FSR 4 preparation is already pinned to 2.4.0** (`tools/prepare_fsr4_lite.py`,
  `tools/fsr4_prepare.py`, ADR 0012). Only the `third_party/fsr4cap/ffx` headers
  are still from 2.3.0; the differences that matter are small and are listed in
  section 7.
- **Version pinning is now essential.** 2.1+ can select a newer
  driver-provided implementation unless an `ffxOverrideVersion` is supplied.
  For reproducible output, pin FSR 3.1.6 / 3.1.7 by default; expose FSR 4.1.2
  (Windows, AMD) and ML frame generation as explicit opt-ins after validation.

## 2. What the project uses today

| Component | SDK | Pin / files | Used by |
|---|---|---|---|
| Windows FSR 3.1.4 upscaling + frame generation | FidelityFX SDK **1.1.4** | `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`, signed `amd_fidelityfx_vk.dll` | `runtime/nvn/vulkan/temporal.c`, `temporal_fg.inc`; loader at `temporal.c:46-83` |
| Linux native FSR 3.1.4 | FidelityFX SDK **1.1.4** | `tools/build_fsr3_linux.py` (`REVISION`), GLSL shaders, optional AMD DLL SPIR-V extraction | `build/linux/libmonado_fsr3.so`, `third_party/fsr3_native/adapter.cpp` |
| FidelityFX API headers | **1.1.4** | `third_party/ffx_api/{ffx_api.h,ffx_api_types.h,ffx_upscale.h,ffx_framegeneration.h,vk/ffx_api_vk.h}` | runtime and native adapter |
| FSR 4.1.2 Lite (Linux/Windows) | **2.4.0 assets** | `tools/prepare_fsr4_lite.py`, `tools/fsr4_prepare.py` | `third_party/fsr4_native`, `runtime/nvn/vulkan/fsr4.c` |
| FSR 4 capture tooling | SDK 2.3.0 headers | `third_party/fsr4cap/ffx` at `60f4ea8…` | `third_party/fsr4cap` |

`temporal.c:2` still says SDK 1.1.4 is "the last with Vulkan". That is no longer
true: 2.4.0 has Vulkan at parity with DX12. The comment should be corrected in
any case.

## 3. What SDK 2.4.0 contains

Release: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v2.4.0

| Effect | Version in 2.4.0 | Notes |
|---|---|---|
| AMD FSR Upscaling (ML) | 4.1.2 | RX 6000/RDNA 2, Strix Halo, Strix Point lightweight model |
| FidelityFX Super Resolution 3 upscaler | 3.1.6 | More precise accumulate, refined reprojection |
| FidelityFX SR Frame Generation | 3.1.6 | |
| FidelityFX SR Frame Generation (Swapchain) | 3.1.7 | |
| AMD FSR Frame Generation (ML) | 4.0.2 | RX 9000 / RDNA 4, Windows 11; needs camera vectors and exact call order |
| AMD FSR Frame Generation (Swapchain) | 4.0.0 | Driver-assisted frame pacing on RDNA 4 + Adrenalin 26.10.1+ |
| FSR Ray Regeneration | 1.2.1 | not used by Monado |
| FSR Radiance Caching | preview | not used by Monado |

Layout (`Kits/FidelityFX/`):

```
api/include/                  ffx_api.h, ffx_api_types.h, ffx_api_loader.h,
                              dx12/, vk/
backend/vk/                   ffx_vk.cpp, ffx_backends_vk.cpp, ffx_vk.h
upscalers/fsr3/               FSR 2.3.4/3.1.6 host code + HLSL
upscalers/include/            ffx_upscale.h
framegeneration/fsr3/         FSR 3 FG host code + HLSL
framegeneration/include/      ffx_framegeneration.h, VK/DX12 swapchain headers
signedbin/                    loader + effect DLLs (the only supported binaries)
docs/                         API and technique documentation
```

There is no SDK/effect build system (the only CMake file in the tag builds a
Cauldron shader-reloader helper), no `.glsl` file (0 in the tag) and no
generated `*_permutations.h` in the repository. Shader permutations are
generated by AMD's internal `FidelityFX_SC` tool and linked into the signed
DLLs. The SDK samples use Visual Studio + vcpkg on Windows. The repository states that the
effects are shipped as prebuilt libraries; only a limited reference set of HLSL
is provided.

### 3.1 Signed Vulkan DLLs in 2.4.0

| File | File version | Size | SHA-256 |
|---|---|---|---|
| `amd_fidelityfx_loader_vk.dll` | 1.0.2.4555 | 39,184 | `38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9` |
| `amd_fidelityfx_upscaler_vk.dll` | 4.1.2.4555 | 36,135,696 | `4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7` |
| `amd_fidelityfx_framegeneration_vk.dll` | 4.0.2.4555 | 36,806,928 | `d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4` |

All three export the same five entry points as before
(`ffxCreateContext`, `ffxDestroyContext`, `ffxDispatch`, `ffxQuery`,
`ffxConfigure`). Imports:

- loader: `MSVCP140`, `VCRUNTIME140`, `VCRUNTIME140_1`, UCRT, KERNEL32;
- upscaler: those plus `vulkan-1.dll`, `VERSION.dll`;
- framegeneration: those plus `vulkan-1.dll`, `VERSION.dll`, `USER32.dll`.

The VC++ runtime is still required app-local, exactly as with 1.1.4.

The upscaler DLL contains FSR 2.3.4, FSR 3.1.6 and FSR 4.1.2 providers
(provider classes `ffxProvider_FSR2`, `ffxProvider_FSR3Upscale`,
`ffxProvider_FSR4`, `ffxProvider_FSR4_Int8`, `ffxProvider_FSR4_Int8_Navi2`,
`ffxProvider_FSR4Lite_Int8`). The frame-generation DLL contains
`ffxProvider_Fsr3FrameGeneration` (3.1.6), `...SwapChain` (3.1.7),
`ffxProvider_MLFrameGeneration` (4.0.2) and ML swapchain (4.0.0).

### 3.2 DLL structure

Starting with 2.0.0, effects are split by type; an application must load the
loader DLL, not `amd_fidelityfx_vk.dll`:

- `amd_fidelityfx_loader_vk.dll` is interface- and behaviour-compatible with
  `amd_fidelityfx_vk.dll`, but only loads effect DLLs and routes calls;
- `amd_fidelityfx_upscaler_vk.dll` provides upscaling;
- `amd_fidelityfx_framegeneration_vk.dll` provides FG and the FG swapchain;
- denoiser/radiance-cache DLLs are DX12-only.

Lookup order for effect DLLs: already-loaded module, executable folder, loader
folder. Putting all three beside `xcde.exe` is therefore sufficient.

## 4. API/ABI changes from 1.1.4 to 2.4.0

### 4.1 Descriptor values that stay compatible

The base type values of the descriptors we use are unchanged:

| Descriptor | 1.1.4 | 2.4.0 |
|---|---|---|
| upscale context | `0x00010000` | `0x00010000` |
| upscale dispatch | `0x00010001` | `0x00010001` |
| FG context | `0x00020001` | `0x00020001` |
| FG configure | `0x00020002` | `0x00020002` |
| FG dispatch | `0x00020003` | `0x00020003` |
| FG prepare | `0x00020004` | `0x00020004` (deprecated) |
| FG swapchain VK | `0x00040001` | `0x00040001` |
| FG swapchain functions VK | `0x00040005` | `0x00040005` |

### 4.2 Breaking / required changes

1. **Vulkan backend descriptor changed.**
   - 1.1.4: type `0x00000003`, fields `vkDevice`, `vkPhysicalDevice`,
     `vkDeviceProcAddr`.
   - 2.4.0: type `0x01000003` (`FFX_API_MAKE_BACKEND_SUB_ID(FFX_API_BACKEND_ID_VK, 3)`),
     field `deviceContext` of type `VkDeviceContext`.
   - 2.4.0 keeps the old form as `ffxCreateBackendVKDesc_DEPRECATED` /
     `FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED`, specifically for
     older providers. New effect providers require the new form.
2. **API version descriptors are required.**
   - `ffxCreateContextDescUpscaleVersion` (`0x0001000b`), `version = FFX_UPSCALER_VERSION`.
   - `ffxCreateContextDescFrameGenerationVersion` (`0x0002000e`),
     `version = FFX_FRAMEGENERATION_VERSION`.
   - `ffxCreateContextDescFrameGenerationSwapChainVersionVK` (`0x0004000a`),
     `version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION`.
   - The signed DLLs explicitly validate these ("An instance of ... must be
     attached ... and specify a valid version"). The version is an API version,
     not the selected effect version.
3. **Backend IDs moved between 2.3.0 and 2.4.0.** In 2.3.0
   `FFX_API_BACKEND_ID_VK` was `0x02000000`; in 2.4.0 it is `0x01000000`
   (XBOX moved the other way). A 2.3.0 header must not be mixed with 2.4.0
   Vulkan DLLs.
4. **Frame-generation preparation.**
   - `ffxDispatchDescFrameGenerationPrepare` is deprecated; `PrepareV2`
     (`0x0002000c`) is the replacement and embeds camera position/up/right/forward.
   - ML FG 4.0.2 requires `PrepareV2` and correct camera data; targeting it
     without camera vectors is not valid.
   - Legacy FSR 3.1.6 FG still accepts the old prepare path (deprecated), so the
     first migration can keep `Prepare` and defer `PrepareV2`.
   - `unused_reset` was never implemented; `PrepareV2::reset` works.
5. **New return code** `FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE = 7`,
   useful for diagnostics when an old driver/effect DLL sees a new descriptor.
6. **Global debug descriptor**: `ffxConfigureDescGlobalDebug1` is deprecated in
   favour of `ffxConfigureDescGlobalDebug` (type 7, per-effect `effectId`). We
   use the per-context `fpMessage`/`DEBUG_CHECKING` path, so no change is needed.
7. **`ffx_api_loader.h` is Windows-only** (`windows.h`/`libloaderapi.h`). Our
   existing Windows `LoadLibrary`/`GetProcAddress` and Linux `dlopen`/`dlsym`
   code still works; the header is not needed.

### 4.3 Effect version selection (new in 2.1)

`ffxQueryDescGetVersions` lists the implementations available for a create
descriptor type, including driver-provided versions. A driver version has a
trailing ` *` and bit 31 set in its id. Supplying `ffxOverrideVersion` pins the
effect and **disables automatic selection and future driver upgrades**; without
an override the newest implementation is chosen, preferring a driver version
only when it is strictly newer.

Consequence for Monado:

- leaving no override gives AMD users FSR 4.1.2 for free on supported GPUs, but
  output then varies with GPU, driver and DLL version;
- pinning `FSR 3.1.6` / `FSR 3.1.7` keeps the current look and testability;
- the safest product shape is an explicit version setting with "auto" as one
  choice, and a log line showing the provider version (we already log
  `ffxQueryGetProviderVersion`).

## 5. Windows migration (validated prototype)

This branch contains the following changes; they build on Linux and keep the
1.1.4 native component working. They have **not** been run against the Windows
2.4.0 DLLs yet (no Windows machine in this session), so treat this as a
compile-clean prototype.

### 5.1 Headers

`third_party/ffx_api` is updated to 2.4.0:

- `ffx_api.h`, `ffx_api_types.h`, `ffx_upscale.h`, `ffx_framegeneration.h`,
  `vk/ffx_api_vk.h` replaced;
- new `ffx_framegeneration_api_types.h` and
  `vk/ffx_api_framegeneration_vk.h`;
- local patches (marked `[XCDE]`):
  - `FFX_API_ENTRY` is empty on non-Windows (the upstream header uses
    `__declspec(dllexport)` unconditionally);
  - `<wchar.h>` is included (C consumers);
  - `VkQueueInfoFFXAPI` gets a typedef (C consumers);
  - cross-directory includes are flattened for our vendored layout.

The new headers compile as C11/C17 and C++17/C++20.

### 5.2 Runtime

`runtime/nvn/vulkan/temporal.c`:

- includes the real `ffx_api/vk/ffx_api_vk.h` instead of a hand-declared backend
  struct;
- deliberately keeps the 1.1.4 chain
  `ffxCreateContextDescUpscale -> ffxCreateBackendVKDesc_DEPRECATED` while
  `ffx_load()` still loads `amd_fidelityfx_vk.dll`. SDK 2.x renamed the Vulkan
  backend descriptor from `0x0000003` to `0x01000003`; the 1.1.4 DLL only
  recognises the old value, so passing the new type makes `ffxCreateContext`
  fail with `FFX_API_RETURN_ERROR`. The 2.4.0 chain
  (`...UpscaleVersion -> ffxCreateBackendVKDesc`) is built when the loader
  switch lands (section 5.3) and is exercised today by the native test with
  `FFX_BACKEND_TYPE=0x01000003`.

`runtime/nvn/vulkan/temporal_fg.inc`:

- uses the official frame-generation and swapchain headers instead of local
  copies of the 1.1.4 structs;
- likewise keeps the deprecated backend descriptor and no version structures
  until the loader switch;
- keeps the deprecated `ffxDispatchDescFrameGenerationPrepare` for the first
  step; `PrepareV2` + camera vectors is the follow-up for ML FG (section 8).

`third_party/fsr3_native/adapter.cpp`:

- accepts both backend descriptor forms (`0x01000003` and the 1.1.4 `0x3`);
- ignores the new API version descriptor (`ffxCreateContextDescUpscaleVersion`);
- still reports `FSR 3.1.4 native Vulkan (FP32)`.

### 5.3 What still has to change before a merge

1. **DLL loading.** `ffx_load()` at `temporal.c:46-83` still loads
   `amd_fidelityfx_vk.dll`. It must load
   `amd_fidelityfx_loader_vk.dll` on Windows. Either require 2.4.0 and drop the
   old file, or add a two-mode path (loader -> new descriptors; old DLL ->
   deprecated backend descriptor and no version structs). The latter is more
   user-friendly but more code and needs Windows testing; the 1.1.4 path
   must keep the legacy `0x0000003` backend descriptor.
2. **Version policy.** Add `ffxQueryDescGetVersions` and either leave selection
   automatic (FSR 4 on supported AMD GPUs) or pin FSR 3.1.6 via
   `ffxOverrideVersion`. Decide whether full FSR 4.1.2 becomes a new
   `upscaler` option or an automatic upgrade of `fsr3`. It must not silently
   change the meaning of an existing `xcde.ini` value.
3. **Frame generation.** Decide pinning: FSR 3.1.6 FG + 3.1.7 swapchain is the
   conservative default. ML FG 4.0.2 is Windows 11 + RX 9000+ only and requires
   `PrepareV2` with camera vectors; gate it behind a setting and validate.
4. **Loader errors.** Add the new return code to logs and handle a missing
   effect DLL / bad version with a clear message (the loader itself loads, so
   the failure now appears at context creation, not at `LoadLibrary`).
5. **Packaging and CI.**
   - `scripts/copy_upscaler_dlls.sh` and `cmake/copy_upscaler_dlls.cmake`:
     copy `amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll`,
     `amd_fidelityfx_framegeneration_vk.dll` (not `amd_fidelityfx_vk.dll`).
   - `.github/workflows/release.yml`: replace the 1.1.4 zip download with the
     three 2.4.0 signed files from the pinned tag (or the 2.4.0 zip); verify
     SHA-256 and Authenticode; keep the VC++ runtime packaging.
   - `tools/make_portable.py` error/help texts and
     `docs/BUILD-CMAKE.md` still name `amd_fidelityfx_vk.dll`.
   - The portable packager already copies every `build/*.dll` root, so the
     three effect DLLs will be included once the copy step places them there;
     update the checks/messages to the loader name.
6. **Docs/notices.** Update `NOTICE.md` (SDK 2.4.0, split DLLs), `docs/LINUX.md`,
   `docs/BUILD-CMAKE.md`, `third_party/fsr3_native/README.md` and the stale
   "1.1.4 is the last with Vulkan" comments.
7. **Tests.** `tests/native/test_fsr3_temporal.cpp` now chains the version
   descriptor when `FFX_BACKEND_TYPE != 3`, so it can drive the 2.4.0 loader
   under Wine. Add a Windows smoke test that checks:
   - loader loads and `ffxQuery` lists versions;
   - FSR 3.1.6 context creates (and, if enabled, FSR 4.1.2);
   - FG swapchain creates with the version descriptor;
   - `Prepare`/`PrepareV2` path used matches the pinned FG version.

### 5.4 Effort and risk

- Code changes are small (the prototype is ~800 added lines, mostly vendored
  headers). The real effort is Windows validation: DLL deployment, VC++ runtime,
  version selection, FG behavior, and driver-specific testing.
- Risk is moderate: the API is additive but the DLL split and version
  descriptors are mandatory; a wrong mix of headers/DLLs now returns errors or
  selects a different effect version instead of failing early.
- No game-content work is involved; all inputs (color, depth, motion, reactive)
  stay the same.

## 6. Linux native FSR 3: why it is not a version bump

`tools/build_fsr3_linux.py` builds the 1.1.4 FSR 3.1.4 host code and compiles the
11 FSR 3 passes as GLSL permutations with `glslangValidator`. For 2.4.0:

- there are **no GLSL shaders** in the tag;
- the HLSL sources reference generated `*_permutations.h` and
  `git_hash_branch.h` that are not in the repository;
- there is no CMake/build system for the effects; the supported artifacts are
  the signed Windows DLLs;
- the host/backend interface was reworked (`FfxApi*` type names, `CreateHeapVK`,
  shader blob passed into `CreatePipelineVK`, `ffxDecompressShaderBlob` with
  `miniz`, new interface callbacks);
- `miniz` is pulled by vcpkg in the samples and is not in the repository.

So "update Linux to 2.4.0" means writing a new build pipeline, not changing a
pin.

### 6.1 Feasibility of a native 3.1.6 port

A native port is technically possible:

- `amd_fidelityfx_upscaler_vk.dll` embeds **1,778 compressed SPIR-V modules**
  (`FXZF` header + zlib, `FfxBlobCompressHeader`). Filtering by resource names
  gives about **294 FSR 3-family modules**, 214 FSR 2 and 1,389 FSR 4 modules.
  The 294 should cover the pass/permutation matrix (wave32/wave64, FP16, LUT,
  sharpening).
- Our existing `reflect()` in `tools/build_fsr3_linux.py` already extracts
  descriptor names, types and bindings from SPIR-V; the FSR 3.1.6 resources use
  the same style of names (`r_input_color_jittered`, `rw_accumulation`, ...).
- The 2.4.0 `ffx_fsr3upscaler.cpp` is mostly the 1.1.4 host code with the new
  type/interface changes; the provider wrapper is separate and not needed if we
  keep a direct adapter.

Work required:

1. Extend the build tool with a 2.4.0 path that sparse-checks out
   `Kits/FidelityFX/{api,backend/vk,upscalers/fsr3}`, downloads the signed
   `amd_fidelityfx_upscaler_vk.dll` with a pinned SHA-256, inflates the `FXZF`
   blobs, filters the FSR 3-family modules and writes the permutation tables
   (our `FfxShaderBlob` emission code can be reused).
2. Implement the 2.4.0 shader-blob entry points
   (`fsr3UpscalerGetPermutationBlobByIndex`, `fsr3UpscalerIsWave64`) and a
   `git_hash_branch.h` stub.
3. Port `third_party/fsr3_native/adapter.cpp` from the 1.1.4 `FfxInterface` to
   the 2.4.0 backend interface (resource/heap/pipeline signatures), or compile
   the provider path instead of the direct host API.
4. Use `miniz` (or pre-decompress all blobs so the runtime never needs it) and
   keep the Linux context-size alignment patch.
5. Re-validate with `tests/native/test_fsr3_temporal.cpp` and the real game,
   including the LUT/FP16/wave64 permutations.

Estimate: 1-2 weeks of focused work plus GPU validation, with a real risk of
subtle descriptor/layout differences. Unless 3.1.6's accumulate/reprojection
improvement is important for this game, the cost/benefit does not justify
replacing the working 1.1.4 native component now.

**Recommendation:** keep Linux native FSR 3.1.4 on SDK 1.1.4 as the default;
keep the shared `third_party/ffx_api` headers at 2.4.0 (the adapter accepts both
descriptor forms), and treat the new 3.1.6 path as an opt-in until gameplay
validation is complete.

### 6.2 Linux native 3.1.6 prototype (validated)

The feasibility path from 6.1 is implemented in this branch and works on Linux:

| Area | File | What it does |
|---|---|---|
| Extraction | `tools/extract_fsr3_240_blobs.py` | parses the pinned `amd_fidelityfx_upscaler_vk.dll` (minimal PE parser, no `pefile` dependency), reads the `FfxShaderBlobInfo` arrays and their 64-entry indirection tables, inflates the selected `FXZF`/zlib SPIR-V and emits `blobs.cpp` |
| Host | SDK 2.4.0 `ffx_fsr3upscaler.cpp` | compiled directly; only the provider/`ffx_backends.h` include and the `GetResourceSizeFromDescription` declaration are adjusted |
| Backend | SDK 2.4.0 `ffx_vk.cpp` | compiled directly; the miniz decompression hook is bypassed because the generated blobs are already raw SPIR-V |
| Adapter | `third_party/fsr3_native/adapter_240.cpp` | implements the 1.1.4-compatible `ffxCreateContext`/`ffxDispatch` ABI on the 2.4.0 host and accepts both backend descriptor forms |
| Build | `tools/build_fsr3_linux.py --sdk-version 2.4.0`, CMake `-DMONADO_FSR3_SDK=2.4.0` | sparse-checks out the pinned tag, verifies the DLL SHA-256, builds the component |

Binary details that matter:

- `FfxShaderBlobInfo` starts with the **compressed total size**, then the
  `blobData` pointer, `entryName`, the seven counts and the per-group
  name/binding/count/space arrays. `blobSize` is not the decompressed size.
- The FSR 3 base arrays selected for the Linux port are the wave32/FP32 sets
  (`prepare_inputs` 8 entries + table, `accumulate` 24 entries + table; one
  entry for the other passes). The FP16 bit is masked out and the Vulkan
  backend reports shader model 5.1, so wave64 is never requested.
- The `accumulate` table maps the 6-bit key as expected: bit 0 = LUT, bit 2 =
  low-resolution motion vectors, bit 5 = accumulate-sharpen; bit 4 is ignored.
  The keys used by the game are 5 (accumulate) and 37 (accumulate-sharpen) on
  RDNA wave32/FP32.
- The selected `prepare_inputs` and `accumulate` shaders are the base arrays;
  the other FSR 3 passes are identical across the base/wave64 duplicates in
  this DLL.
- The adapter's context is enlarged to `4 * FFX_SDK_DEFAULT_CONTEXT_SIZE` for
  Linux's 32-bit `wchar_t` (the private context is 837,824 bytes).

Verification:

```bash
# build with the pinned SDK/DLL and run the native regression
scripts/build_fsr3_linux.sh --sdk-version 2.4.0
clang++ -std=c++17 -Ithird_party tests/native/test_fsr3_temporal.cpp \
  -Lbuild/linux -Wl,-rpath,"$PWD/build/linux" -lmonado_fsr3 -lvulkan \
  -o build/fsr3/test-temporal
build/fsr3/test-temporal
FFX_BACKEND_TYPE=0x01000003 build/fsr3/test-temporal
```

Both runs print `native FSR3 synthetic dispatch OK` with the same
RMS/amplitude values as the 1.1.4 component (frame 0 `0.0472/0.3332`, frame 7
`0.0164/0.3769`, frame 31 `0.0182/0.4251`). `FSR3_SHARPEN=1` additionally
exercises the accumulate-sharpen and RCAS pipelines; the values also match
1.1.4 exactly (`0.0314/0.3555`, `0.0026/0.3977`, `0.0308/0.4431`). CMake builds
the same target with `-DMONADO_FSR3_SDK=2.4.0`; the default stays `1.1.4`.

In-game validation (2026-11-17): with `upscaler = fsr3` at 1920x1080 on an
NVIDIA GTX 1050 Ti (driver 580.196.07, Vulkan 1.4), the runtime log reports
`[tmp] FSR context 1920x1080 (FSR 3.1.6 native Vulkan (FP32))` and the game
showed no visible regression versus 3.1.4; a slight reduction in shimmering is
plausible but was not confirmed by a side-by-side capture. This is a
non-AMD Vulkan result; RDNA 2/3 has not been tried. GPU time was not measured.

The remaining performance check needs no rebuild: run `build/linux/monado
targets` once per component with `MONADO_FSR3_LIBRARY` pointing at
`libmonado_fsr3_114check.so` or the 3.1.6 `libmonado_fsr3.so`, set
`XCDE_METRICS=<file>.csv`, play the same save/route, then compare:
`python tools/performance_report.py <3.1.6>.csv --compare <1.1.4>.csv
--start-frame <N>` (look at `gpu_ms_p50`; `frame_ms` includes V-Sync).

Not covered yet: performance of 3.1.6 versus 3.1.4, the FP16 and wave64
permutation sets, frame generation, and the Windows loader/effect-DLL split
(section 5).

## 7. FSR 4 Lite and fsr4cap

- `tools/prepare_fsr4_lite.py` and `tools/fsr4_prepare.py` already use SDK
  **2.4.0** (`7539ae8…`) and the 4.1.2.4555 upscaler DLL hash
  `aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace`.
  ADR 0012 is consistent with 2.4.0.
- `third_party/fsr4cap/ffx` was still pinned to 2.3.0 (`60f4ea8…`). The
  2.3.0 -> 2.4.0 differences in the vendored subset are:
  - `ffx_api.h`: `FFX_API_BACKEND_ID_VK` / `XBOX` values swapped; global-debug
    struct deprecation; `FFX_API_EFFECT_COUNT`;
  - `ffx_upscale.h`: `FFX_UPSCALER_VERSION_PATCH` 1 -> 2, documentation;
  - `ffx_api_types.h`: only a trailing newline;
  - `ffx_api_loader.h` and `ffx_api_dx12.h`: unchanged.
  This branch updates those headers to 2.4.0 and the README pin. The capture
  tool uses DX12, so the VK ID change is harmless there, and it pins the effect
  with `ffxOverrideVersion`. A capture-tool rebuild is still needed to exercise
  the new headers.
- The native FSR 4 Lite component (GPL) remains the Linux path. On Windows, the
  2.4.0 signed upscaler can provide full FSR 4.1.2 on supported AMD hardware;
  that is an opportunity, not a replacement for Lite on non-AMD/Linux.

## 8. Recommended phased plan

### Phase 1 - Windows runtime on 2.4.0 (recommended now)

1. Merge the header/descriptor prototype (section 5) after Windows validation.
2. Switch `ffx_load()` to `amd_fidelityfx_loader_vk.dll`; ship loader + upscaler
   + framegeneration DLLs; keep the VC++ runtime packaging.
3. Pin FSR 3.1.6 upscaler, FSR 3.1.6 FG and 3.1.7 swapchain by querying
   versions (no hard-coded ids); log the chosen provider.
4. Keep frame generation on the legacy `Prepare` path initially; add the
   version descriptors. Validate on RX 6000/7000/9000.
5. Update packaging, CI, NOTICE and docs; correct the "last with Vulkan"
   comments.

### Phase 2 - FSR 4.1.2 and ML frame generation (opt-in)

1. Add `ffxQueryDescGetVersions` selection and expose:
   - `FSR 3.1.6` (default, deterministic),
   - `FSR 4.1.2` (Windows, supported AMD GPUs),
   - "auto" if desired.
   Keep `FSR 4 Lite` as the cross-platform/fallback option.
2. Add `PrepareV2` with camera position/right/up/forward derived from
   `TmpCamera.view` (the 3x4 view matrix in `temporal.h`); use it for ML FG and
   keep the legacy path for 3.1.6 if needed.
3. Gate ML FG 4.0.2 to Windows 11 + RX 9000+; validate call order,
   `HUDLessColor`, camera cut reset and frame pacing.
4. Add tests for version selection, descriptor chains and FG call order.

### Phase 3 - Linux native FSR 3.1.6 (prototype done)

The extraction + host/backend port from section 6.1 is implemented behind
`--sdk-version 2.4.0` / `-DMONADO_FSR3_SDK=2.4.0` and passes the native
regression. Remaining work before changing the default:

1. Measure GPU time against 3.1.4 (`XCDE_METRICS`, `gpu_ms_p50`) on the same
   save/route; the quality pass was subjective only. Broaden the quality check
   to RDNA 2/3 and a second non-AMD device.
2. Validate the prepare-inputs base permutation choice and, if needed, ship the
   FP16/wave64 sets (the backend does not request wave64 today).
3. Run the full Linux runtime/launcher smoke test with the 2.4.0 component and
   decide whether to keep 1.1.4 as the fallback package or ship both.

## 9. Verification performed in this branch

- Updated headers compile as C11/C17 (`clang -std=gnu17 -fsyntax-only`) and
  C++17/C++20.
- `scripts/build_linux.sh --runtime-only` builds with the 2.4.0 headers and the
  dual-descriptor adapter; the same build with `XCDE_FSR3_SDK=2.4.0` builds the
  FSR 3.1.6 component.
- CMake: `cmake --preset linux -DMONADO_RUNTIME_ONLY=ON -DMONADO_FSR3_SDK=2.4.0`
  then `cmake --build --preset linux --target native_fsr` builds
  `build/linux/libmonado_fsr3.so` from the pinned signed DLL.
- Native FSR 3 regression:
  `clang++ -std=c++17 -Ithird_party tests/native/test_fsr3_temporal.cpp -Lbuild/linux -Wl,-rpath,$PWD/build/linux -lmonado_fsr3 -lvulkan -o build/fsr3/test-temporal`
  then `build/fsr3/test-temporal`:
  `native FSR3 synthetic dispatch OK` (same RMS/amplitude as before). The same
  test with `FFX_BACKEND_TYPE=0x01000003` exercises the new backend descriptor
  and the API version descriptor against our adapter and also passes.
  The 2.4.0/3.1.6 component (`scripts/build_fsr3_linux.sh --sdk-version 2.4.0`)
  passes both backend forms with the same frame 0/7/31 values. `FSR3_SHARPEN=1`
  (accumulate-sharpen + RCAS) also matches 1.1.4 exactly.
- In-game: the 3.1.6 component runs at 1920x1080 on an NVIDIA GTX 1050 Ti
  (driver 580.196.07) with no visible regression versus 3.1.4. Performance
  comparison and formal side-by-side captures are still pending.
- Not verified: any Windows 2.4.0 DLL path, FSR 4 full, ML FG, driver-provided
  versions, 3.1.6 performance, FP16/wave64 permutations.

## 10. Open questions

1. Do we want FSR 4.1.2 full on Windows as a user-visible option, or only
   automatic version selection on supported GPUs?
2. Is deterministic output across machines a hard requirement (it decides
   whether to pin versions by default)?
3. Can we derive correct camera basis vectors from `TmpCamera.view` for ML FG,
   and does the game's near/far convention match the ML provider's expectations?
4. Which Windows test hardware is available for RX 6000 / RX 9000 / NVIDIA
   fallback, and can we run the native test under Wine with the 2.4.0 loader?
5. Does 3.1.6's measured performance justify making it the default Linux
   component? The quality gate passed on one device (no visible regression);
   the FP16/wave64 sets and the prepare-inputs permutation choice still need
   validation.

## Appendix A - Useful commands

```bash
# Header/API compile check
clang -std=gnu17 -fsyntax-only -Ithird_party /tmp/ffx_header_test.c
clang++ -std=c++17 -fsyntax-only -Ithird_party /tmp/ffx_header_test.cpp

# Linux runtime (uses the 1.1.4 native FSR 3 component)
scripts/build_linux.sh --runtime-only

# Native FSR 3 regression
clang++ -std=c++17 -O1 -Ithird_party tests/native/test_fsr3_temporal.cpp \
  -Lbuild/linux -Wl,-rpath,"$PWD/build/linux" -lmonado_fsr3 -lvulkan \
  -o build/fsr3/test-temporal
build/fsr3/test-temporal

# 2.4.0 signed Vulkan DLLs (pinned; for a Windows checkout)
BASE=https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/7539ae8b6568ad29ed78a753404351daf3b5cca7/Kits/FidelityFX/signedbin
# amd_fidelityfx_loader_vk.dll       38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9
# amd_fidelityfx_upscaler_vk.dll     4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7
# amd_fidelityfx_framegeneration_vk.dll d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4
```

## Appendix B - File map for the Windows change

| Area | File | Change |
|---|---|---|
| API headers | `third_party/ffx_api/*` | 2.4.0 + Linux/C patches |
| Upscale context | `runtime/nvn/vulkan/temporal.c:508-535` | legacy backend descriptor until the loader switch (2.4.0 chain in the native test) |
| Upscale loader | `runtime/nvn/vulkan/temporal.c:46-83` | loader DLL / fallback |
| FG context/swapchain | `runtime/nvn/vulkan/temporal_fg.inc:46-120` | legacy backend descriptor; version descriptors with the loader switch |
| FG per-frame | `runtime/nvn/vulkan/temporal_fg.inc:140-185` | pin/prepare path |
| Native adapter | `third_party/fsr3_native/adapter.cpp` | accept both backend descriptors |
| DLL copy | `scripts/copy_upscaler_dlls.sh`, `cmake/copy_upscaler_dlls.cmake` | three new DLLs |
| CI | `.github/workflows/release.yml:36-37, 215-227` | 2.4.0 URLs/hashes/signatures |
| Packaging | `tools/make_portable.py:47, 329, 416` | loader name / MSVC text |
| Notices | `NOTICE.md:33, 51, 59` | SDK 2.4.0, split DLLs |
| Docs | `docs/BUILD-CMAKE.md:96`, `docs/LINUX.md:47-53` | names/versions |
| Tests | `tests/native/test_fsr3_temporal.cpp` | version descriptor for 2.4 |

## Appendix C - Reproducing the shader-blob count

The Linux feasibility claim in section 6.1 can be checked against a downloaded
2.4.0 `amd_fidelityfx_upscaler_vk.dll`:

```python
import struct, zlib
data = open('amd_fidelityfx_upscaler_vk.dll', 'rb').read()
blobs, at = [], 0
while (pos := data.find(b'FXZF', at)) >= 0:
    at = pos + 4
    magic, size = struct.unpack_from('<II', data, pos)
    if magic != 0x465A5846 or not 1000 < size < 10_000_000:
        continue
    out = zlib.decompress(data[pos + 8:], 15)
    if len(out) == size and out[:4] == b'\x03\x02\x23\x07':
        blobs.append(out)
fsr3 = [b for b in blobs if any(n in b for n in
        (b'r_input_color_jittered', b'r_accumulation', b'r_lanczos_lut'))]
print(len(blobs), 'SPIR-V modules')
print(len(fsr3), 'FSR 3-family modules')
```

The actual count is 1,778 modules; 294 are in the FSR 3 family (the rest are
FSR 2 and the FSR 4 model graphs). Blob bytecode is compressed with a
`FfxBlobCompressHeader` (`FXZF` magic + zlib), as documented by
`Kits/FidelityFX/api/internal/ffx_blob_compression.h` in 2.4.0.
