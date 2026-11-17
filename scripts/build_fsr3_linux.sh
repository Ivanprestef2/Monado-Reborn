#!/usr/bin/env bash
# Optional native FSR 3.1.4 (default, SDK 1.1.4 GLSL) or FSR 3.1.6
# (--sdk-version 2.4.0, extracts SPIR-V from AMD's pinned signed DLL).
# No game data or Windows tools required on Linux.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 tools/build_fsr3_linux.py "$@"
