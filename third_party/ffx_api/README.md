# AMD FidelityFX API headers

Headers of the AMD FidelityFX SDK API (MIT; the licence is at the top of each
file), from
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK at commit
`7539ae8b6568ad29ed78a753404351daf3b5cca7` (`v2.4.0`, `Kits/FidelityFX/api/include`
and `Kits/FidelityFX/upscalers/include`).

Files:

- `ffx_api.h`, `ffx_api_types.h` - base API and types;
- `ffx_upscale.h` - upscaler descriptors;
- `ffx_framegeneration.h`, `ffx_framegeneration_api_types.h` - frame generation;
- `vk/ffx_api_vk.h`, `vk/ffx_api_framegeneration_vk.h` - Vulkan descriptors.

Local `[XCDE]` changes, kept deliberately small:

- `FFX_API_ENTRY` is empty on non-Windows (upstream uses
  `__declspec(dllexport)` unconditionally);
- `<wchar.h>` is included for C consumers (`ffxApiMessage` uses `wchar_t`);
- `VkQueueInfoFFXAPI` gets a typedef because the Vulkan header is also consumed
  as C;
- cross-directory includes are flattened for this vendored layout.

See `docs/dev/fidelityfx-sdk-2.4.0.md` for the upgrade research and the
difference from the 1.1.4 headers still used by the native Linux FSR 3
component.
