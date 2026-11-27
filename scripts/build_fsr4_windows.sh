#!/usr/bin/env bash
# [XCDE] Stage AMD's signed amd_fidelityfx_upscaler_vk.dll (FidelityFX SDK 2.4.0, MIT) beside xcde.exe in build/.
set -euo pipefail
cd "$(dirname "$0")/.."
DLL_URL="https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll"
DLL_SHA256="4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7"
LIC_URL="https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/docs/license.md"
mkdir -p build
if [ ! -f build/amd_fidelityfx_upscaler_vk.dll ] || [ "$(sha256sum build/amd_fidelityfx_upscaler_vk.dll | cut -d' ' -f1)" != "$DLL_SHA256" ]; then
  echo "Downloading AMD amd_fidelityfx_upscaler_vk.dll (FidelityFX SDK 2.4.0, 36 MB)..."
  curl -fsSL -o build/amd_fidelityfx_upscaler_vk.dll.part "$DLL_URL"
  got=$(sha256sum build/amd_fidelityfx_upscaler_vk.dll.part | cut -d' ' -f1)
  if [ "$got" != "$DLL_SHA256" ]; then
    rm -f build/amd_fidelityfx_upscaler_vk.dll.part
    echo "FAIL: amd_fidelityfx_upscaler_vk.dll SHA-256 $got, expected $DLL_SHA256" >&2
    exit 1
  fi
  mv -f build/amd_fidelityfx_upscaler_vk.dll.part build/amd_fidelityfx_upscaler_vk.dll
fi
if [ ! -f build/fsr4-LICENSE-AMD-SDK.md ]; then
  curl -fsSL -o build/fsr4-LICENSE-AMD-SDK.md "$LIC_URL" || true
fi
echo "FSR 4.1.2 Vulkan DLL ready: build/amd_fidelityfx_upscaler_vk.dll"
