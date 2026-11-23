#!/usr/bin/env bash
# [XCDE] LINUX-017: optional native NVIDIA NGX DLSS SR/DLAA component, no game dependencies.
set -euo pipefail
cd "$(dirname "$0")/.."
active=sdk/dlss-linux/active-sdk.txt
if [[ ! -f $active ]]; then
  echo 'missing sdk/dlss-linux/active-sdk.txt: run python3 tools/package/setup_linux_dlss.py first' >&2
  exit 1
fi
sdk=$(<"$active")
if [[ ! -f $sdk/lib/Linux_x86_64/libnvsdk_ngx.a || ! -f $sdk/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.1 ]]; then
  echo "incomplete DLSS SDK in $sdk: run python3 tools/package/setup_linux_dlss.py" >&2
  exit 1
fi
mkdir -p build/linux
${CXX:-clang++} -O2 -std=c++17 -march=x86-64-v2 -fPIC -shared -fvisibility=hidden \
  -Wl,--exclude-libs,ALL -Iruntime/nvn/vulkan -I"$sdk/include" \
  runtime/nvn/vulkan/dlss_linux.cpp "$sdk/lib/Linux_x86_64/libnvsdk_ngx.a" \
  -o build/linux/libmonado_dlss.so $(pkg-config --cflags --libs vulkan) -ldl -pthread
cp -f "$sdk/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.1" build/linux/libnvidia-ngx-dlss.so.310.9.1
cp -f "$sdk/LICENSE.txt" build/linux/dlss-LICENSE.txt
echo 'Built build/linux/libmonado_dlss.so'
