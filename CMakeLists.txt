# Monado: CMake + Ninja build of the same objects and executables as the shell scripts
# (scripts/build_rt_gfx.sh + build_boot.sh + link_gfx.sh + tools/launcher/build.sh on Windows,
#  scripts/build_linux.sh + tools/launcher/build_linux.sh on Linux), with the same compilers and flags.
#
#   tools/build/gen_all.py --out generated               # game code: first, and again after a new dump or patch change
#   cmake --preset windows    (or linux, linux-zig)
#   cmake --build --preset windows
#
# Ninja tracks header dependencies, compiles in parallel and rebuilds only what changed. The executables land where
# the launchers expect them: build/xcde.exe + build/xcde_launcher.exe (Windows), build/linux/monado +
# build/linux/monado-launcher (Linux). See docs/guides/BUILD-CMAKE.md.
cmake_minimum_required(VERSION 3.21)
project(Monado LANGUAGES C CXX)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
option(MONADO_RUNTIME_ONLY "Compile the runtime only (no generated game code, no link)" OFF)
# Release packages (docs/guides/BUILD-CMAKE.md): the executable holds no game code. It loads lasgame.so / lasgame.dll, which
# the launcher builds once from the player's own dump (tools/gamebuild/gamebuild.py); nothing derived from the game ships.
option(MONADO_RELEASE "Runtime without game code: loads the game library the launcher builds on first start" OFF)
# GitHub #119: the version the launcher shows and compares with the latest GitHub release (the release workflow passes
# the tag's version; "dev" = a local build, which checks only when asked)
set(MONADO_VERSION "dev" CACHE STRING "Package version shown by the launcher's update check")
if(CMAKE_C_COMPILER MATCHES "zig")
  set(MONADO_ZIG ON)
  set(MARCH -march=x86_64_v2)   # (zig's CPU model name)
else()
  set(MONADO_ZIG OFF)
  set(MARCH -march=x86-64-v2)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Optimisation level of the generated game code and the C runtime, and the profile-guided build (docs/guides/PGO.md, after
# TotK Recomp's docs/pgo.md). MONADO_PGO=gen instruments both: the game adds its counts to build/pgo/raw/*.profraw
# every minute and at exit; tools/build/pgo.py merge turns them into a .profdata. MONADO_PGO=<file.profdata> compiles with it.
set(MONADO_OPT "O1" CACHE STRING "Optimisation level of the game code and the C runtime: O1 or O2 (a PGO build uses O2)")
set(MONADO_PGO "" CACHE STRING "Profile-guided build: empty (off), gen (instrumented build), or a .profdata file")
set(PGO_FLAGS)
set(PGO_LINK)
set(PGO_DEPENDS)
# a static function's profile name carries its source path unless told otherwise: the first-start builder compiles in
# another folder than the training build
set(PGO_NAMES -mllvm -static-func-full-module-prefix=false)
if(MONADO_PGO STREQUAL "gen")
  set(PGO_FLAGS "-fprofile-generate=${ROOT}/build/pgo/raw" ${PGO_NAMES} -DMONADO_PGO_GEN)
  set(PGO_LINK -fprofile-generate)
elseif(MONADO_PGO)
  get_filename_component(MONADO_PGO_FILE "${MONADO_PGO}" ABSOLUTE BASE_DIR "${ROOT}")
  if(NOT EXISTS "${MONADO_PGO_FILE}")
    message(FATAL_ERROR "MONADO_PGO=${MONADO_PGO}: no such profile")
  endif()
  # unprofiled / out-of-date: code the training never ran, or that changed since; it compiles without a profile
  # MONADO_PGO_ID: the profile's digest on the command line. zig's compiler cache keys on the arguments and the
  # included files, not on the profile's contents, and would hand back objects built with an older profile.
  file(SHA256 "${MONADO_PGO_FILE}" _pgo_sha)
  string(SUBSTRING "${_pgo_sha}" 0 16 _pgo_id)
  set(PGO_FLAGS "-fprofile-use=${MONADO_PGO_FILE}" -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date
      -Wno-backend-plugin ${PGO_NAMES} -DMONADO_PGO_ID=${_pgo_id})
  set(PGO_DEPENDS "${MONADO_PGO_FILE}")
endif()
if(MONADO_PGO AND MONADO_OPT STREQUAL "O1")
  set(MONADO_OPT O2)  # the profile's inlining and hot/cold layout work at -O2
endif()
if(NOT MONADO_OPT MATCHES "^O[0-3]$")
  message(FATAL_ERROR "MONADO_OPT=${MONADO_OPT}: use O1 or O2")
endif()
message(STATUS "game code and runtime: -${MONADO_OPT}, PGO: ${MONADO_PGO}")
set(OPT -${MONADO_OPT} ${PGO_FLAGS})

# ---- unit lists: the one place a new runtime file is added ----
set(RT_UNITS loader dispatch kernel ipc fnprof log services/fs services/romfs_virtual services/game_files services/am services/acc services/time
    services/hid_set services/kbicons services/misc services/audio services/hwopus hle/heaps hle/pl hle/vi hle/nvn_boot
    hle/fibers hle/input hle/party hle/bindings hle/movie hle/framepace hle/foliage_distance hle/foliage_pool hle/graphics hle/cheats_options hle/cheats_combat hle/cheats_rewards hle/cheats_enemy_health hle/title_exit hle/system_exit hle/desktop_exit_caption hle/expanded_saves hle/expanded_save_scan hle/quickstep_native hle/quickstep_picker hle/quickstep_save hle/quickstep_slot hle/quickstep_stats hle/quickstep_stats_ui hle/quickstep_ui_memory hle/collision_bvh
    host/stubs host/settings_ini host/main_boot diag/diag diag/sampler diag/watchdog
    diag/crashdump paths)
set(NVN_UNITS astc cmdbuf formats maxwell/translate objects output_scale smaa_data tegra vulkan/device
    vulkan/exec vulkan/post vulkan/precompile vulkan/precompile_sig vulkan/shaders vulkan/textures vulkan/temporal
    vulkan/vkq vulkan/fsr4)
set(IMGUI third_party/imgui)
set(UI_SOURCES ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_vulkan.cpp runtime/nvn/vulkan/imgui_glue.cpp runtime/ui/menu_vk.cpp)
set(LAUNCHER_SOURCES ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer3.cpp tools/launcher/launcher.cpp)
if(WIN32)
  list(APPEND RT_UNITS platform/windows)
else()
  list(APPEND RT_UNITS platform/linux)
endif()
set(RT_SOURCES)
if(MONADO_RELEASE)
  list(APPEND RT_UNITS gamelib)
  # GAMELIB_ID: the fingerprint of the generator, guest headers and guest flags (tools/build/game_id.py). The first-start
  # builder stamps the library with the same one; the runtime loads no other.
  execute_process(COMMAND "${Python3_EXECUTABLE}" tools/build/game_id.py WORKING_DIRECTORY "${ROOT}"
                  OUTPUT_VARIABLE GAMELIB_ID OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _gid)
  if(NOT _gid EQUAL 0 OR NOT GAMELIB_ID)
    message(FATAL_ERROR "tools/build/game_id.py failed")
  endif()
  message(STATUS "release runtime, GAMELIB_ID ${GAMELIB_ID}")
else()
  list(APPEND RT_UNITS host/gamelib_static)
  set(GAMELIB_ID dev)
endif()
foreach(u IN LISTS RT_UNITS)
  list(APPEND RT_SOURCES runtime/${u}.c)
endforeach()
foreach(u IN LISTS NVN_UNITS)
  list(APPEND RT_SOURCES runtime/nvn/${u}.c)
endforeach()

# ---- generated sources ----
set(GEN_INC "${CMAKE_BINARY_DIR}/inc")
add_custom_command(OUTPUT "${GEN_INC}/smaa_hlsl.inc"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${GEN_INC}"
  COMMAND "${Python3_EXECUTABLE}" tools/build/gen_smaa_inc.py third_party/smaa/SMAA.hlsl "${GEN_INC}/smaa_hlsl.inc"
  DEPENDS tools/build/gen_smaa_inc.py third_party/smaa/SMAA.hlsl WORKING_DIRECTORY "${ROOT}" VERBATIM)
set_source_files_properties(runtime/nvn/smaa_data.c PROPERTIES OBJECT_DEPENDS "${GEN_INC}/smaa_hlsl.inc")

# [XCDE] T282: the shader precompile fingerprint (tools/build/shader_sig.py). cache/precompile_done.txt stores it, and
# `<game> --precompile-status` compares it with the sources to tell an unrelated rebuild from one that can change the
# precompiled shaders (the launcher's JUGAR check and RecompLauncher's builder). Generated when any of the game-file
# discovery, translator, replacement-GLSL or SPIR-V compiler sources changes; only runtime/nvn/vulkan/precompile_sig.c
# includes it. The translator folder is globbed recursively (a new helper header or subfolder there is a fingerprint input
# too), and directories are dropped from the DEPENDS list. Do not glob every Vulkan header: exec.c's unrelated rendering
# headers change for gameplay work and are covered by XCDE_PRECOMPILE_ARTIFACTS_REV when the build path changes.
file(GLOB_RECURSE SHADER_SIG_SOURCES CONFIGURE_DEPENDS
     "${ROOT}/runtime/nvn/maxwell/*" "${ROOT}/runtime/nvn/vulkan/shaders.c" "${ROOT}/runtime/nvn/vulkan/shaders.h"
     "${ROOT}/runtime/nvn/vulkan/vk.h" "${ROOT}/runtime/nvn/vulkan/precompile.c" "${ROOT}/runtime/nvn/objects.c"
     "${ROOT}/runtime/cache_validation.h" "${ROOT}/runtime/services/game_files.c" "${ROOT}/runtime/services/game_files.h"
     "${ROOT}/runtime/host/utf8_path.h" "${ROOT}/runtime/platform/dir_utf8.h"
     "${ROOT}/runtime/nvn/vulkan/bloom_prefilter.h" "${ROOT}/runtime/nvn/vulkan/bloom.h"
     "${ROOT}/runtime/nvn/vulkan/gtao.h")
foreach(_sig_source IN LISTS SHADER_SIG_SOURCES)
  if(IS_DIRECTORY "${_sig_source}")
    list(REMOVE_ITEM SHADER_SIG_SOURCES "${_sig_source}")
  endif()
endforeach()
add_custom_command(OUTPUT "${GEN_INC}/shader_sig.h"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${GEN_INC}"
  COMMAND "${Python3_EXECUTABLE}" tools/build/shader_sig.py --out "${GEN_INC}/shader_sig.h"
  DEPENDS tools/build/shader_sig.py ${SHADER_SIG_SOURCES} WORKING_DIRECTORY "${ROOT}" VERBATIM)
set_source_files_properties(runtime/nvn/vulkan/precompile_sig.c PROPERTIES
  OBJECT_DEPENDS "${GEN_INC}/shader_sig.h" INCLUDE_DIRECTORIES "${GEN_INC}")

if(NOT MONADO_RUNTIME_ONLY AND NOT MONADO_RELEASE)
  # (another folder of tools/build/gen_all.py output, for A/B builds of a lifter change)
  set(MONADO_GENERATED "${ROOT}/generated" CACHE PATH "Folder with the generated game code (tools/build/gen_all.py --out)")
  file(GLOB GAME_SOURCES CONFIGURE_DEPENDS "${MONADO_GENERATED}/main/*.c" "${MONADO_GENERATED}/sdk/*.c")
  if(NOT GAME_SOURCES OR NOT EXISTS "${MONADO_GENERATED}/main/main_id.h")
    message(FATAL_ERROR "generated/ is empty: run tools/build/gen_all.py --out generated first (it needs your dump in targets/), "
                        "or configure with -DMONADO_RUNTIME_ONLY=ON")
  endif()
endif()

if(WIN32)
  # =========================== Windows (MSYS2 UCRT64 libraries, zig cc) ===========================
  set(MSYS2_PREFIX "C:/msys64/ucrt64" CACHE PATH "MSYS2 UCRT64 prefix with SDL3, Vulkan, shaderc, FFmpeg, Opus, zlib")
  set(OUT "${ROOT}/build")
  # scripts/build_rt_gfx.sh CF, and the C++ flags of its ImGui / DLSS / VMA units
  set(RT_WARN_FLAGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare -Wno-format -Wno-unused-function)
  set(RT_C_FLAGS ${OPT} ${MARCH} -fno-strict-aliasing -fvisibility=hidden ${RT_WARN_FLAGS})
  set(RT_C_INCLUDES runtime runtime/nvn third_party "${MSYS2_PREFIX}/include")
  set(CXX_FLAGS -std=c++17 -O1 ${MARCH} -fvisibility=hidden -w "SHELL:-idirafter \"${MSYS2_PREFIX}/include\"")
  # scripts/build_boot.sh CF (the game code)
  set(GAME_C_FLAGS ${OPT} ${MARCH} -ffp-contract=off -fno-strict-aliasing -fvisibility=hidden -w)

  add_library(monado_rt OBJECT ${RT_SOURCES})
  target_compile_options(monado_rt PRIVATE ${RT_C_FLAGS})
  target_include_directories(monado_rt PRIVATE ${RT_C_INCLUDES})
  target_compile_definitions(monado_rt PRIVATE "GAMELIB_ID=\"${GAMELIB_ID}\"")
  set_source_files_properties(runtime/host/main_boot.c PROPERTIES COMPILE_DEFINITIONS XCDE_GFX)
  set_source_files_properties(runtime/nvn/smaa_data.c PROPERTIES INCLUDE_DIRECTORIES "${GEN_INC};${ROOT}/third_party/smaa/Textures")
  # [XCDE] T-RUNTIME-O2: hot Vulkan/kernel/platform/collision units get -O2 on Windows as well as Linux
  set_source_files_properties(
      runtime/nvn/vulkan/exec.c runtime/nvn/vulkan/textures.c runtime/nvn/vulkan/vkq.c
      runtime/nvn/vulkan/device.c runtime/nvn/vulkan/post.c runtime/nvn/vulkan/temporal.c
      runtime/nvn/cmdbuf.c runtime/nvn/objects.c
      runtime/kernel.c runtime/dispatch.c runtime/platform/windows.c runtime/hle/collision_bvh.c
      PROPERTIES COMPILE_OPTIONS -O2)

  add_library(monado_ui OBJECT ${UI_SOURCES})
  target_compile_options(monado_ui PRIVATE ${CXX_FLAGS})
  target_include_directories(monado_ui PRIVATE runtime runtime/nvn runtime/nvn/vulkan ${IMGUI} ${IMGUI}/backends)
  add_library(monado_dlss OBJECT runtime/nvn/vulkan/dlss.cpp)
  target_compile_options(monado_dlss PRIVATE ${CXX_FLAGS})
  target_include_directories(monado_dlss PRIVATE runtime runtime/nvn third_party/streamline/include)
  add_library(monado_vma OBJECT runtime/nvn/vulkan/vma.cpp)
  target_compile_options(monado_vma PRIVATE ${CXX_FLAGS} -O2)

  set(GAME_LINK_LIBS)
  foreach(l SDL3.dll shaderc_shared.dll vulkan-1.dll opus.dll avcodec.dll avformat.dll avutil.dll swscale.dll
            z)
    list(APPEND GAME_LINK_LIBS "${MSYS2_PREFIX}/lib/lib${l}.a")
  endforeach()
  list(APPEND GAME_LINK_LIBS winmm dbghelp)
  set(GAME_LINK_OPTIONS ${MARCH} -Wl,--stack,268435456)
  if(MONADO_RELEASE)
    # The game library links against xcde.exe's exports (tools/gamebuild/gamebuild_stage.py --link-file xcde_imports.a). zig's
    # linker has no --export-all-symbols, so the runtime objects' globals are exported through a generated .drectve
    # (tools/gamebuild/coff_exports.py) and the import library is written from the linked executable (tools/gamebuild/pe_implib.py).
    set(RELEASE_EXPORTS "${CMAKE_BINARY_DIR}/release_exports.c")
    add_custom_command(OUTPUT "${RELEASE_EXPORTS}"
      COMMAND "${Python3_EXECUTABLE}" "${ROOT}/tools/gamebuild/coff_exports.py" --output "${RELEASE_EXPORTS}" --objects
              "$<TARGET_OBJECTS:monado_rt>" "$<TARGET_OBJECTS:monado_ui>" "$<TARGET_OBJECTS:monado_dlss>"
              "$<TARGET_OBJECTS:monado_vma>"
      DEPENDS monado_rt monado_ui monado_dlss monado_vma "${ROOT}/tools/gamebuild/coff_exports.py"
              "$<TARGET_OBJECTS:monado_rt>" "$<TARGET_OBJECTS:monado_ui>"
      COMMAND_EXPAND_LISTS VERBATIM)
  endif()
  # Windows PE resource object (RT_GROUP_ICON ID 1) embedded into both xcde.exe and xcde_launcher.exe
  set(WIN_ICON_OBJ "${CMAKE_BINARY_DIR}/monado_icon.o")
  add_custom_command(OUTPUT "${WIN_ICON_OBJ}"
    COMMAND "${Python3_EXECUTABLE}" tools/build/ico_to_coff.py packaging/windows/monado.ico "${WIN_ICON_OBJ}"
    DEPENDS tools/build/ico_to_coff.py packaging/windows/monado.ico WORKING_DIRECTORY "${ROOT}" VERBATIM)
  set_source_files_properties("${WIN_ICON_OBJ}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

  set(GAME_NAME xcde)
  set(GAME_OBJECTS $<TARGET_OBJECTS:monado_rt> $<TARGET_OBJECTS:monado_ui> $<TARGET_OBJECTS:monado_dlss>
                   $<TARGET_OBJECTS:monado_vma> "${WIN_ICON_OBJ}")
  if(MONADO_RELEASE)
    list(APPEND GAME_OBJECTS "${RELEASE_EXPORTS}")
  endif()

  # tools/launcher/build.sh
  set(LAUNCHER_NAME xcde_launcher)
  list(APPEND LAUNCHER_SOURCES "${WIN_ICON_OBJ}")
  set(LAUNCHER_FLAGS -O2 ${MARCH} -std=c++17 "SHELL:-idirafter \"${MSYS2_PREFIX}/include\"")
  set(LAUNCHER_WARN_FLAGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare -Wno-format-security)
  set(LAUNCHER_LIBS "${MSYS2_PREFIX}/lib/libSDL3.dll.a" "${MSYS2_PREFIX}/lib/libvulkan-1.dll.a")
  set(LAUNCHER_LINK_OPTIONS ${MARCH} -Wl,--subsystem,windows -static-libstdc++)

  # Windows FSR 4.1.2 uses AMD's signed amd_fidelityfx_upscaler_vk.dll (FidelityFX SDK 2.4.0, scripts/build_fsr4_windows.sh).
else()
  # =========================== Linux (pkg-config libraries; system clang, or zig cc) ===========================
  set(OUT "${ROOT}/build/linux")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET sdl3 vulkan shaderc opus libavcodec libavformat libavutil libswscale
                    zlib)
  pkg_check_modules(LAUNCHER_DEPS REQUIRED IMPORTED_TARGET sdl3 vulkan)
  set(VMA_INCLUDE_DIR "$ENV{VMA_INCLUDE_DIR}" CACHE PATH "Directory with vk_mem_alloc.h, if it is not installed system-wide")
  # scripts/build_linux.sh flags (runtime and game code alike)
  set(RT_WARN_FLAGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare -Wno-format -Wno-unused-function)
  set(LINUX_BASE_FLAGS ${OPT} ${MARCH} -ffp-contract=off -fno-strict-aliasing -D_GNU_SOURCE)
  if(NOT MONADO_RELEASE)  # a release runtime keeps default visibility: the game library resolves its symbols (-rdynamic)
    list(APPEND LINUX_BASE_FLAGS -fvisibility=hidden)
  endif()
  set(LINUX_FLAGS ${LINUX_BASE_FLAGS} -w)
  set(LINUX_INCLUDES runtime runtime/nvn third_party "${GEN_INC}")
  if(VMA_INCLUDE_DIR)
    list(APPEND LINUX_INCLUDES "${VMA_INCLUDE_DIR}")
  endif()
  option(MONADO_LOCAL_ISA "Enable safe local host ISA flags (-mfma -mbmi -mbmi2 -mlzcnt) for local builds" ON)
  option(MONADO_FAST_FP "Skip per-instruction FPCR/NaN fixup checks in lifted FP/SIMD ops" ON)
  option(MONADO_THINLTO "Enable ThinLTO (-flto=thin + lld) for cross-chunk PGO inlining" OFF)
  set(LINUX_GAME_EXTRA)
  if(MONADO_LOCAL_ISA AND NOT MONADO_RELEASE)
    list(APPEND LINUX_GAME_EXTRA -mfma -mbmi -mbmi2 -mlzcnt)
  endif()
  if(MONADO_FAST_FP)
    list(APPEND LINUX_GAME_EXTRA -DMONADO_FAST_FP)
  endif()
  if(MONADO_THINLTO)
    list(APPEND LINUX_GAME_EXTRA -flto=thin)
  endif()
  set(GAME_C_FLAGS ${LINUX_FLAGS} ${LINUX_GAME_EXTRA})

  add_library(monado_rt OBJECT ${RT_SOURCES})
  target_compile_options(monado_rt PRIVATE ${LINUX_BASE_FLAGS} ${RT_WARN_FLAGS})
  target_include_directories(monado_rt PRIVATE ${LINUX_INCLUDES} third_party/smaa/Textures)
  target_compile_definitions(monado_rt PRIVATE "GAMELIB_ID=\"${GAMELIB_ID}\"" XCDE_GFX)
  target_link_libraries(monado_rt PRIVATE PkgConfig::DEPS)
  # [XCDE] T-RUNTIME-O2: match Windows hot Vulkan/kernel/platform/collision -O2 list on Linux
  set_source_files_properties(
      runtime/nvn/vulkan/exec.c runtime/nvn/vulkan/textures.c runtime/nvn/vulkan/vkq.c
      runtime/nvn/vulkan/device.c runtime/nvn/vulkan/post.c runtime/nvn/vulkan/temporal.c
      runtime/nvn/cmdbuf.c runtime/nvn/objects.c
      runtime/kernel.c runtime/dispatch.c runtime/platform/linux.c runtime/hle/collision_bvh.c
      PROPERTIES COMPILE_OPTIONS -O2)

  add_library(monado_ui OBJECT ${UI_SOURCES} runtime/nvn/vulkan/dlss.cpp runtime/nvn/vulkan/vma.cpp)
  target_compile_options(monado_ui PRIVATE ${LINUX_FLAGS} -std=c++17)
  target_include_directories(monado_ui PRIVATE ${LINUX_INCLUDES} ${IMGUI} ${IMGUI}/backends third_party/streamline/include
                             runtime/nvn/vulkan)
  target_compile_definitions(monado_ui PRIVATE "GAMELIB_ID=\"${GAMELIB_ID}\"")
  target_link_libraries(monado_ui PRIVATE PkgConfig::DEPS)

  set(GAME_LINK_LIBS PkgConfig::DEPS Threads::Threads ${CMAKE_DL_LIBS} m)
  find_package(Threads REQUIRED)
  set(GAME_LINK_OPTIONS -rdynamic)
  if(MONADO_THINLTO)
    list(APPEND GAME_LINK_OPTIONS -flto=thin -fuse-ld=lld)
  endif()
  set(GAME_NAME monado)
  set(GAME_OBJECTS $<TARGET_OBJECTS:monado_rt> $<TARGET_OBJECTS:monado_ui>)

  # tools/launcher/build_linux.sh
  set(LAUNCHER_NAME monado-launcher)
  set(LAUNCHER_FLAGS -O2 ${MARCH} -std=c++17 -D_GNU_SOURCE)
  set(LAUNCHER_WARN_FLAGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare -Wno-format-security)
  set(LAUNCHER_LIBS PkgConfig::LAUNCHER_DEPS ${CMAKE_DL_LIBS})
  set(LAUNCHER_LINK_OPTIONS)

  # [XCDE] T-LINUX-FSR3 / T-FSR4-LITE: the optional native upscaler components (their own scripts; a failure only
  # leaves that upscaler unavailable)
  option(MONADO_NATIVE_FSR "Build the optional native FSR 3 / FSR 4 Lite components" ON)
  if(MONADO_NATIVE_FSR)
    add_custom_target(native_fsr ALL
      COMMAND bash -c "scripts/build_fsr3_linux.sh || echo 'warning: native FSR 3 not built (see third_party/fsr3_native/README.md)' >&2"
      COMMAND bash -c "scripts/build_fsr4_linux.sh || echo 'warning: native FSR 4 (fsr4up-vk) not built (needs git, cmake, python3)' >&2"
      COMMAND bash -c "scripts/build_dlss_linux.sh || echo 'warning: native Linux DLSS not built (run python3 tools/package/setup_linux_dlss.py)' >&2"
      WORKING_DIRECTORY "${ROOT}" USES_TERMINAL VERBATIM)
  endif()
endif()

# A/B builds (docs/guides/PGO.md): put this configuration's executables in another folder than the default one
set(MONADO_OUT_DIR "" CACHE PATH "Folder for the game executable and launcher (default: build/ or build/linux/)")
if(MONADO_OUT_DIR)
  set(OUT "${MONADO_OUT_DIR}")
endif()

if(MONADO_RUNTIME_ONLY)
  return()
endif()

# ---- the game code (scripts/build_boot.sh on Windows) and the game executable ----
if(MONADO_RELEASE)
  set(GAME_CODE)
else()
add_library(monado_game OBJECT ${GAME_SOURCES})
target_compile_options(monado_game PRIVATE ${GAME_C_FLAGS})
target_include_directories(monado_game PRIVATE runtime)
if(NOT WIN32)
  target_include_directories(monado_game PRIVATE ${LINUX_INCLUDES})
  target_compile_definitions(monado_game PRIVATE "GAMELIB_ID=\"${GAMELIB_ID}\"")
  target_link_libraries(monado_game PRIVATE PkgConfig::DEPS)
endif()

set(GAME_CODE $<TARGET_OBJECTS:monado_game>)
endif()
add_executable(monado_game_exe ${GAME_OBJECTS} ${GAME_CODE})
set_target_properties(monado_game_exe PROPERTIES OUTPUT_NAME ${GAME_NAME} LINKER_LANGUAGE CXX
                      RUNTIME_OUTPUT_DIRECTORY "${OUT}")
target_link_libraries(monado_game_exe PRIVATE ${GAME_LINK_LIBS})
target_link_options(monado_game_exe PRIVATE ${GAME_LINK_OPTIONS})
target_link_options(monado_game_exe PRIVATE ${PGO_LINK})  # (gen: links the profile runtime)
if(PGO_DEPENDS)  # a new profile recompiles everything it applies to
  set_property(SOURCE ${RT_SOURCES} ${GAME_SOURCES} APPEND PROPERTY OBJECT_DEPENDS "${PGO_DEPENDS}")
endif()
if(WIN32 AND MONADO_RELEASE)
  # zig's linker writes no import library: make one from xcde.exe's export table (tools/gamebuild/pe_implib.py)
  add_custom_command(TARGET monado_game_exe POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${ROOT}/tools/gamebuild/pe_implib.py" "$<TARGET_FILE:monado_game_exe>"
            "${ROOT}/build/gamebuild/xcde_imports.a" --zig "${CMAKE_C_COMPILER}" VERBATIM)
endif()
if(WIN32)
  # scripts/copy_upscaler_dlls.sh: the AMD / NVIDIA runtime DLLs beside build/xcde.exe, if they were downloaded
  set(SDK_DIR "${ROOT}/sdk" CACHE PATH "Downloaded FidelityFX / Streamline SDKs (scripts/copy_upscaler_dlls.sh)")
  add_custom_command(TARGET monado_game_exe POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -DSDK_DIR=${SDK_DIR} -DOUT=${OUT} -P "${ROOT}/cmake/copy_upscaler_dlls.cmake" VERBATIM)
endif()

# ---- the launcher ----
add_executable(monado_launcher ${LAUNCHER_SOURCES})
set_target_properties(monado_launcher PROPERTIES OUTPUT_NAME ${LAUNCHER_NAME} RUNTIME_OUTPUT_DIRECTORY "${OUT}")
target_compile_options(monado_launcher PRIVATE ${LAUNCHER_FLAGS})
set_source_files_properties(${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
                            ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer3.cpp
                            PROPERTIES COMPILE_OPTIONS -w)
set_source_files_properties(tools/launcher/launcher.cpp PROPERTIES COMPILE_OPTIONS "${LAUNCHER_WARN_FLAGS}")
target_include_directories(monado_launcher PRIVATE ${IMGUI} ${IMGUI}/backends)
target_link_libraries(monado_launcher PRIVATE ${LAUNCHER_LIBS})
target_link_options(monado_launcher PRIVATE ${LAUNCHER_LINK_OPTIONS})
target_compile_definitions(monado_launcher PRIVATE MONADO_VERSION="${MONADO_VERSION}")
