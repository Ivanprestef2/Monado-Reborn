#!/usr/bin/env bash
# Build the launcher: build/xcde_launcher.exe (SDL3 + Dear ImGui, SDL renderer). Run it with "Play Monado.bat".
cd "$(dirname "$0")/../.."
Z=".venv/Scripts/python -m ziglang c++"
I=third_party/imgui
M=/c/msys64/ucrt64
mkdir -p build/launcher
for f in $I/imgui.cpp $I/imgui_draw.cpp $I/imgui_tables.cpp $I/imgui_widgets.cpp $I/backends/imgui_impl_sdl3.cpp \
         $I/backends/imgui_impl_sdlrenderer3.cpp tools/launcher/launcher.cpp; do
  o=build/launcher/$(basename ${f%.cpp}).o
  # T248: shared runtime headers also affect the launcher; always rebuild its translation unit.
  [ "$f" = tools/launcher/launcher.cpp ] || [ ! "$o" -nt "$f" ] || [ ! "$o" -nt tools/launcher/build.sh ] || continue
  $Z -c $f -o $o -O3 -march=x86_64_v2 -std=c++17 "-DMONADO_VERSION=\"${MONADO_VERSION:-dev}\"" -I$I -I$I/backends -idirafter $M/include -w || exit 1
done
ICON_OBJ=build/launcher/monado_icon.o
[ -f "$ICON_OBJ" ] && [ "$ICON_OBJ" -nt packaging/windows/monado.ico ] && [ "$ICON_OBJ" -nt tools/build/ico_to_coff.py ] || \
  .venv/Scripts/python tools/build/ico_to_coff.py packaging/windows/monado.ico "$ICON_OBJ" || exit 1
$Z -o build/xcde_launcher.exe build/launcher/*.o $M/lib/libSDL3.dll.a $M/lib/libvulkan-1.dll.a -march=x86_64_v2 -Wl,--subsystem,windows -static-libstdc++ 2> build/launcher/link.err
rc=$?; [ $rc = 0 ] || cat build/launcher/link.err | head -20; exit $rc
