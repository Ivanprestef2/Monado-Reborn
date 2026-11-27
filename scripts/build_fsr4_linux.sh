#!/usr/bin/env bash
# [XCDE] Native Linux FSR 4.1.2 Vulkan component (auz466/fsr4up-vk: amd_fidelityfx_upscaler_vk.so).
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 tools/build/build_fsr4_linux.py "$@"
