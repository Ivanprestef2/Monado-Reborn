// [XCDE] T282: the "are the precompiled shaders current?" signature.
//
// What the precompile writes (cache/spirv, the driver's shader and pipeline caches warmed by the program/seed/pipeline
// passes) is decided by the game-file discovery, translator/replacement sources, SPIR-V compiler options and game files -
// not by the size and time of the executable. The done stamp used to name the executable, so every rebuild recompiled
// the shaders again; an edit to the launcher, the UI or gameplay code has no effect on the shaders, but cost a full warm
// run (the scan, every translation and driver pipeline creation).
//
// The signature (FNV-1a 64) covers:
//  - XCDE_SHADER_SIG: the build's fingerprint of the game-file discovery, translator, replacement-GLSL and SPIR-V
//    compiler sources (tools/build/shader_sig.py; <build>/inc/shader_sig.h, or -DXCDE_SHADER_SIG for a native test).
//  - XCDE_PRECOMPILE_ARTIFACTS_REV (precompile_rev.h): revision of exec.c's build path, which shader_sig.py cannot hash
//    without invalidating on gameplay changes; bump it when the pipeline state/libraries that pass builds change.
//  - the effective xcde.ini GPU pipeline policy (gpu_profile compatibility / gpu_gpl on/off): it decides whether the
//    run warms graphics-pipeline libraries or whole pipelines.
//  - the environment options, by PRESENCE and value (a set-but-empty variable is not the same as an unset one, because
//    stage_opt()/getenv() see "" as set): the SPIR-V optimisation/cache modes (LARECOMP_VK_OPT, XCDE_FS_OPT,
//    XCDE_SPIRV_LOOSE, LARECOMP_VK_NO_SPIRV_CACHE), the translator switches that change the emitted GLSL
//    (XCDE_STATIC_STACKS, XCDE_RSC_PIXEL, XCDE_D16_REF, XCDE_SCALED_GATHER, LARECOMP_NO_DISCARD, XCDE_HW_FP16,
//    XCDE_EARLY_FS, XCDE_STRUCTURED_CFG, LARECOMP_SAVE_MSL), the replacement/patch sources and overrides
//    (XCDE_BLOOM_PREFILTER, XCDE_BLOOM_FILTER, XCDE_GTAO, XCDE_CHAR_LIGHT, XCDE_TMAA_TEXEL, XCDE_WATER_REFRACT_FIX, XCDE_GLSL_DIR; the
//    override directory's .glsl bytes are hashed too), the
//    pipeline/layout policy reachable from the precompile build (LARECOMP_VK_GPL, XCDE_VK_GPL, XCDE_PUSH_DESC,
//    XCDE_CANON, XCDE_GPL_RETAIN, LARECOMP_VK_D24) and the precompile's own pass switches (XCDE_PRECOMPILE_LIBS,
//    XCDE_PRECOMPILE_PIPELINES).
//  - the game files: every file under <root>/romfs by relative name, size and mtime (for a .nup package: its 64-bit
//    size and mtime). A new, changed or removed shader container can add or change programs.
//  - the shipped variant seed (XCDE_VARIANTS_SEED or data/variants_seed.bin): size and mtime. New recorded variants
//    mean new jobs to warm.
// A root is a dump folder with a readable romfs/ or a .nup package file; anything else is "unreadable" (2), not "stale".
// An unreadable shader container or directory inside romfs also makes the signature unreadable: a partial walk must not
// describe a complete cache. Only an entry proven irrelevant is skipped (a dangling non-container link whose metadata
// error is ENOENT/ENOTDIR); EACCES, other errors and DT_UNKNOWN stay fatal.
//
// Not covered: cache/variants.bin (the player's own recordings: it grows while playing, so it must not invalidate the
// cache) and the graphics driver, including the Vulkan route / selected adapter from xcde.ini (the driver's shader
// caches are its own: after a driver update, a GPU switch or a Vulkan API route change press "Precompile shaders").
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../host/utf8_path.h"
#include "../../platform/dir_utf8.h"
#include "precompile_rev.h"
#include "precompile_sig.h"

#ifndef XCDE_SHADER_SIG
#include "shader_sig.h"  // <build>/inc: tools/build/shader_sig.py --out
#endif

static uint64_t mix(uint64_t h, const void *data, size_t n) {
    const uint8_t *b = data;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}
static uint64_t mix_stat(uint64_t h, uint64_t size, int64_t mtime) {
    uint64_t v[2] = {size, (uint64_t)mtime};
    return mix(h, v, sizeof v);
}

// Whether an environment variable exists at all: getenv()-style helpers (xcde_utf8_getenv included) also report false
// for a set-but-empty value, and "" is a meaningful setting here (stage_opt("") turns the optimisation off).
static bool env_present(const char *name) {
#ifdef _WIN32
    wchar_t wname[256];
    if (xcde_utf8_to_wide(name, wname, 256) < 0) return false;
    return GetEnvironmentVariableW(wname, NULL, 0) != 0;
#else
    return getenv(name) != NULL;
#endif
}

// The shared helper keeps sizes 64-bit on Windows (a .nup package is ~15 GB) and follows a symlinked root such as
// the launcher's targets/ to its target.
typedef XcdeFileInfo RootInfo;
static bool root_info(const char *path, RootInfo *out) { return xcde_utf8_file_info(path, out); }

// A package file is told by its first bytes (runtime/services/game_files.c), never by its name.
static bool is_package_file(const char *path) {
    FILE *f = xcde_utf8_fopen(path, "rb");
    if (!f) return false;
    uint8_t magic[4];
    bool yes = fread(magic, 1, sizeof magic, f) == sizeof magic && !memcmp(magic, "NUP\x01", 4);
    fclose(f);
    return yes;
}

// The same list precompile.c scans (wishp/shader libraries and the model/effect containers that hold them).
bool vk_precompile_is_shader_container(const char *name) {
    const char *e = strrchr(name, '.');
    if (!e) return false;
    static const char *ext[] = {".wishp", ".wimdo", ".wismt", ".wismda", ".wiefb", NULL};
    for (int i = 0; ext[i]; i++) if (!strcmp(e, ext[i])) return true;
    return false;
}

bool vk_precompile_entry_skippable(const char *name, unsigned char d_type, int err) {
    if (vk_precompile_is_shader_container(name)) return false;
    if (d_type == DT_DIR || d_type == DT_UNKNOWN) return false;  // (could be a directory: its contents matter)
    return err == ENOENT || err == ENOTDIR;  // (a dangling link or a path with a missing/non-directory component)
}

// Every file under dir, by relative name (rel): added to *sum, so the folder enumeration order does not matter. An
// unreadable shader container or directory makes the signature unreadable rather than silently describing an incomplete
// scan; only an entry proven irrelevant (a dangling non-container link) is skipped. The depth cap turns a symlink loop
// into "unreadable" instead of a stack exhaustion.
#define MIX_DIR_MAX_DEPTH 64
static bool mix_dir(const char *dir, const char *rel, int depth, uint64_t *sum, uint64_t *files) {
    if (depth > MIX_DIR_MAX_DEPTH) return false;
    XcdeDir *dd = xcde_opendir(dir);
    if (!dd) return false;
    bool ok = true;
    XcdeDirent *e;
    while ((e = xcde_readdir(dd))) {
        if (e->d_name[0] == '.') continue;
        char p[XCDE_PRECOMPILE_PATH_MAX], r[XCDE_PRECOMPILE_PATH_MAX];
        int np = snprintf(p, sizeof p, "%s/%s", dir, e->d_name), nr = snprintf(r, sizeof r, "%s/%s", rel, e->d_name);
        if (np < 0 || (size_t)np >= sizeof p || nr < 0 || (size_t)nr >= sizeof r) { ok = false; break; }
        XcdeFileInfo fi;
        if (!xcde_utf8_file_info(p, &fi)) {
            int err = errno;
            if (!vk_precompile_entry_skippable(e->d_name, e->d_type, err)) { ok = false; break; }
            fprintf(stderr, "[precompile] skipping dangling entry %s: %s%c", p, strerror(err), 10);
            continue;
        }
        if (fi.isdir) {
            if (!mix_dir(p, r, depth + 1, sum, files)) { ok = false; break; }
        } else if (fi.isreg) {
            *sum += mix_stat(mix(0xcbf29ce484222325ull, r, strlen(r)), fi.size, fi.mtime);
            (*files)++;
        }
    }
    xcde_closedir(dd);
    return ok;
}

// XCDE_GLSL_DIR is a debug override directory read directly by exec.c's compile_program. Its path is already in the
// environment signature; hash the .glsl bytes too so an in-place edit cannot leave an old precompile looking current.
// exec.c ignores an override it cannot read (a directory, a dangling link, an unreadable file): skip those here as
// well, with a warning, instead of making every shader look unreadable. The bytes are streamed, so any file size works.
static bool mix_glsl_dir(const char *dir, uint64_t *sum, uint64_t *files) {
    XcdeDir *dd = xcde_opendir(dir);
    if (!dd) return errno == ENOENT || errno == ENOTDIR || !*dir;  // (missing, or a file: no override directory)
    XcdeDirent *e;
    while ((e = xcde_readdir(dd))) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || n < 5 || strcmp(e->d_name + n - 5, ".glsl")) continue;
        char path[XCDE_PRECOMPILE_PATH_MAX];
        int len = snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (len < 0 || (size_t)len >= sizeof path) {
            fprintf(stderr, "[precompile] ignoring XCDE_GLSL_DIR entry %s/%s: path is too long%c", dir, e->d_name, 10);
            continue;
        }
        XcdeFileInfo fi;
        if (!xcde_utf8_file_info(path, &fi) || !fi.isreg) {
            fprintf(stderr, "[precompile] ignoring XCDE_GLSL_DIR entry %s: not a readable regular file%c", path, 10);
            continue;
        }
        FILE *f = xcde_utf8_fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "[precompile] ignoring XCDE_GLSL_DIR entry %s: it could not be opened%c", path, 10);
            continue;
        }
        uint64_t h = mix(0xcbf29ce484222325ull, e->d_name, n);
        h = mix_stat(h, fi.size, fi.mtime);
        uint8_t buf[8192];
        size_t got;
        uint64_t total = 0;
        while ((got = fread(buf, 1, sizeof buf, f)) != 0) { h = mix(h, buf, got); total += got; }
        bool ok = !ferror(f) && total == fi.size;
        fclose(f);
        if (!ok) {
            fprintf(stderr, "[precompile] ignoring XCDE_GLSL_DIR entry %s: it could not be read completely%c", path, 10);
            continue;
        }
        *sum += h;
        (*files)++;
    }
    xcde_closedir(dd);
    return true;
}

// Everything that changes what a run warms. Presence AND value: XCDE_FS_OPT= (empty) disables the optimisation, like
// unset leaves it on; a set-but-empty Diagnostics row is a different cache input.
static const char *const k_sig_env[] = {
    // SPIR-V optimisation and cache modes (runtime/nvn/vulkan/shaders.c stage_opt / pack_open / vk_glsl_to_spirv)
    "LARECOMP_VK_OPT", "XCDE_FS_OPT", "XCDE_SPIRV_LOOSE", "LARECOMP_VK_NO_SPIRV_CACHE",
    // translator switches that change the emitted GLSL (runtime/nvn/maxwell/translate.c). SAVE_MSL also adds source
    // annotations (and writes build/glsl), so it changes the translated source/cache key rather than only logging.
    "XCDE_STATIC_STACKS", "XCDE_RSC_PIXEL", "XCDE_D16_REF", "XCDE_SCALED_GATHER", "LARECOMP_NO_DISCARD",
    "XCDE_HW_FP16", "XCDE_EARLY_FS", "XCDE_STRUCTURED_CFG", "LARECOMP_SAVE_MSL",
    // replacement GLSL, source patches and hand-edited stage overrides (runtime/nvn/vulkan/exec.c compile_program)
    "XCDE_BLOOM_PREFILTER", "XCDE_BLOOM_FILTER", "XCDE_GTAO", "XCDE_CHAR_LIGHT", "XCDE_TMAA_TEXEL", "XCDE_WATER_REFRACT_FIX",
    "XCDE_GLSL_DIR",
    // pipeline/layout policy and artifacts reached by vk_precompile_program / vk_precompile_variant (device.c / exec.c /
    // textures.c: LARECOMP_VK_D24 selects the depth format used when a recorded pipeline is rebuilt)
    "LARECOMP_VK_GPL", "XCDE_VK_GPL", "XCDE_PUSH_DESC", "XCDE_CANON", "XCDE_GPL_RETAIN", "LARECOMP_VK_D24",
    // the passes this run would execute (runtime/nvn/vulkan/precompile.c)
    "XCDE_PRECOMPILE_LIBS", "XCDE_PRECOMPILE_PIPELINES",
    NULL};
// Runtime-only pipeline selection (XCDE_GPL_OPTIMIZE, XCDE_GPL_DET, XCDE_PREWARM_WHOLE, XCDE_PREBUILD_LIBS,
// XCDE_PRELINK, XCDE_WHOLE_BATCH) stays out: the offline path always warms the shader libraries and whole optimized
// pipelines; those switches choose how the running game consumes or prewarms them. Execution/reporting controls
// (XCDE_PRECOMPILE_FORCE/THREADS/STALL_S/LIST and crash tests) and trace/log/debug-dump switches do not change successful
// shader/pipeline artifact contents. The AMD-integrated-GPU auto-off of the whole-pipeline pass is device-dependent and
// stays out: the pass policy for the current machine is not knowable without a Vulkan device, which --precompile-status
// deliberately never opens.

bool vk_precompile_sig_policy(const char *root, int gpu_gpl, int gpu_profile, char out[17]) {
    RootInfo ri;
    if (!root_info(root, &ri)) return false;
    uint64_t h = mix(0xcbf29ce484222325ull, XCDE_SHADER_SIG, strlen(XCDE_SHADER_SIG));
    uint32_t rev = XCDE_PRECOMPILE_ARTIFACTS_REV;
    h = mix(h, &rev, sizeof rev);
    for (int i = 0; k_sig_env[i]; i++) {
        bool set = env_present(k_sig_env[i]);
        char value[1100];
        const char *v = "";
        if (set && xcde_utf8_getenv(k_sig_env[i], value, sizeof value)) v = value;  // ("" when set but empty)
        h = mix(h, set ? "1" : "0", 1);
        h = mix(h, v, strlen(v));
        h = mix(h, "|", 1);
    }
    // xcde.ini's pipeline policy (settings_load): compatibility / gpu_gpl=off makes the run warm whole pipelines instead
    // of graphics-pipeline libraries, so a cache warmed under the other policy is not the cache the game will use.
    int32_t policy[2] = {(int32_t)gpu_gpl, (int32_t)gpu_profile};
    h = mix(h, policy, sizeof policy);
    if (env_present("XCDE_GLSL_DIR")) {
        char glsl_dir[1100];
        const char *dir = xcde_utf8_getenv("XCDE_GLSL_DIR", glsl_dir, sizeof glsl_dir) ? glsl_dir : "";
        uint64_t overrides = 0;
        if (!mix_glsl_dir(dir, &h, &overrides)) return false;
        h = mix(h, &overrides, sizeof overrides);
    }
    uint64_t files = 0;
    if (ri.isdir) {
        char romfs[XCDE_PRECOMPILE_PATH_MAX];
        int n = snprintf(romfs, sizeof romfs, "%s/romfs", root);
        if (n < 0 || (size_t)n >= sizeof romfs || !mix_dir(romfs, "", 0, &h, &files)) return false;
    } else {  // (a .nup package: the file itself is the game files)
        if (!is_package_file(root)) return false;  // (a plain file is not a game root: unreadable, not stale)
        h = mix_stat(h, ri.size, ri.mtime);
        files = 1;
    }
    char seed_env[1100];
    const char *seed = xcde_utf8_getenv("XCDE_VARIANTS_SEED", seed_env, sizeof seed_env) ? seed_env : "data/variants_seed.bin";
    RootInfo si;
    if (root_info(seed, &si)) { h = mix(h, "seed", 4); h = mix_stat(h, si.size, si.mtime); }
    h = mix(h, &files, sizeof files);
    snprintf(out, 17, "%016llx", (unsigned long long)h);
    return true;
}

bool vk_precompile_sig(const char *root, char out[17]) {
    return vk_precompile_sig_policy(root, XCDE_SIG_GPU_GPL_AUTO, XCDE_SIG_GPU_PROFILE_AUTO, out);
}
