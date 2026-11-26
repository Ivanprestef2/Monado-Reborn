#include <SDL3/SDL.h>
#include "../../settings.h"
#include "../../resource_policy.h"
// [XCDE] T187: shader precompilation from the game's own shader containers (RomFS), before the first play.
//
// `xcde.exe --precompile <dump dir>` (the launcher's "Precompilar shaders" button; JUGAR runs it the first time) walks
// <dump dir>/romfs, opens every file that holds shaders (and every xbc1 zlib block inside it), finds the game's shader
// programs and builds each one the way its first draw would (exec.c vk_precompile_program): translation, SPIR-V
// (cache/spirv) and the vertex / fragment pipeline libraries, compiled by the driver into its own shader cache. The game then finds a program's pieces compiled the first
// time it draws with it: a fast link instead of a translation, a GLSL compile and two native compiles on the spot.
// Nothing is written that the game would not write itself for the same program: a wrong guess of a translation key
// only costs work nobody uses, never a different image.
//
// Where the shaders are: shader containers ("HCPS" = SPCH: monolib/shader/*.wishp, inside models .wimdo, model streams
// .wismt, maps .wismda and effects .wiefb - mostly zlib-compressed in xbc1 blocks). Header u32s [8]/[9] = code section
// offset / size, [10]/[11] = control section offset / size (from the magic). Each shader has an NVN control section
// (magic 0x98761234; +0x6FC constants size, +0x700 constants offset, +0x704 code blob size) and a code blob (0x30-byte
// NVN header with magic 0x12345678, SPH, code, constants) packed in the same order, 256-byte aligned (a blob without
// the header is a compute shader: skipped). A program is a vertex shader and the fragment shader that follows it.
// Content hashes as the runtime's (objects.c set_shaders, T125). Measured (2026-10-20): 8,124 files, 61,908 control
// sections, 11,649 distinct shaders; 593 of the 595 the game registered in a title + field + battle run are among them.
//
// Translation keys (ShaderKey) depend on draw state the containers don't have. Guesses from the 966 variants of a cold
// title + field run (cache/variants.bin): vertex keys always depth_minus_one without flips; vertex input location 1
// unsigned-integer in 15 % (one family: such vertex shaders are built both ways), other locations float; colour targets
// float in 97 %; textures read as integers in 9 % of the fragment variants (not guessed). The game's log line
// "[shader] spirv cache: N hits, M compiled" says how many it still had to compile.
//
// Progress: cache/precompile_progress.txt rewritten ~5 times a second ("<phase> <overall done> <overall total> <this
// pass done> <this pass total>"; [XCDE] T281 made it cover the four passes - before it counted only the scan and the
// programs and the launcher's bar stood at 100 % through the seed pass and jumped back for the pipelines pass).
// cache/precompile_done.txt at the end of a persistent run whose scan and base programs completed (optional warmup
// failures do not block it; the launcher's "already done" check; loose SPIR-V also has cache/precompile_loose_done.txt,
// and persistence-disabled runs write neither). cache/precompile_times.txt keeps
// this run's pass durations: the next run weights the bar with them. Threads at below-normal priority.
// XCDE_PRECOMPILE_THREADS=<n> (default: all logical CPUs but one); XCDE_PRECOMPILE_LIST=<file>: the shader hashes and
// programs found (tests); XCDE_PRECOMPILE_LIBS=0: SPIR-V only (no Vulkan device, no pipeline libraries);
// XCDE_PRECOMPILE_FORCE=1: run even when the done stamp says the cache is current, and retry the jobs on the crash list
// (cache/precompile_skip.txt) and the in-flight ones; the manual launcher button sets it for its child process.
//
// [XCDE] T281, other changes: the translator is re-entrant now (runtime/nvn/maxwell/translate.c; its scratch buffers
// and the guest-heap range are thread-local), so the workers translate in parallel instead of queueing on one global
// lock; the scan opens up to N >64 MB files at a time (was one) and orders the biggest files first (they used to be
// the last, serialized tail); the seed pass skips the jobs the program pass already built; jobs on the crash skip
// list are counted "skipped" instead of "failed". A user cancel must no longer look like a driver crash: the launcher
// deletes cache/precompile_inflight.bin when it stops a run itself (tools/launcher/launcher.cpp pc_stop).
//
// [XCDE] T282: the done stamp carries a `sig` line, the signature of this run's inputs (runtime/nvn/vulkan/precompile_sig.c):
// the build's translator/SPIR-V compiler fingerprint (tools/build/shader_sig.py), the options that change the compiled
// SPIR-V and the game files. `xcde --precompile-status <dump>` recomputes it and says whether the cache is current -
// without a scan - and this program exits at once when it is (an unrelated rebuild no longer precompiles everything:
// the stamp used to name the executable's size and time). The launcher's JUGAR check and RecompLauncher's builder ask
// for that status before starting a run.
// T282 also made the cache converge. Two reasons it never did: the found shaders' named-input masks (int_in/named) were
// undefined (realloc'd memory) for every container that does not name its inputs, so the build made random integer-input
// variants of those shaders - a different set every run - and, since T281, the build started during the scan while the
// scanners were still merging the masks of the shaders they had not reached. Both are fixed: the fields are initialized
// when a shader is added and the build workers start when the scan is done.
//
// [XCDE] T282 review follow-ups: the explicit button passes XCDE_PRECOMPILE_FORCE instead of deleting the done stamp (a
// failed spawn can no longer throw away a valid cache); scanner count and all source/decompressed buffers are RAM-bounded;
// a packed "current" cache also requires cache/spirv.idx (the pack is unreadable without it); completion is invalidated
// only after the game files and Vulkan were validated; and the signature covers the translator's environment switches and
// the precompile's own pass switches (runtime/nvn/vulkan/precompile_sig.c).
#include "../../platform/dir_utf8.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "../../host/utf8_path.h"  // [XCDE] T278: UTF-8 paths through the wide APIs
#include "../../rt.h"
#include "../../services/game_files.h"  // [XCDE] T280
#include "precompile_sig.h"
#include "shaders.h"

extern void plat_thread_background(void);
extern int plat_cpu_count(void);
extern int vk_precompile_program(const NvnProgram *np, const ShaderKey *vkey, const ShaderKey *fkey, uint32_t *vs_attr);
extern void ui_window_init(void);
static uint64_t now_ns(void) {  // (plat_init, which picks the game's clock, is not run in this mode)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ------------------------------------------------------------------ found shaders and programs (deduplicated)
typedef struct {
    uint64_t hash;
    int stage;
    uint32_t coff, csz;  // constants offset / size (control section)
    uint8_t *code;       // copy of the blob (zero-padded up to the translation limit)
    uint32_t int_in;     // [XCDE] T211 vertex: input locations named as integers in the container (n*: nWgtIdx);
                         // fragment: texture units named as stencil (gTStencil / gTCurStencil / gTPrevStencil)
    bool named;          // the container named its inputs (int_in is known, also when 0)
} Found;
typedef struct { uint32_t vs, fs; } Pair;  // indices into found (fs = UINT32_MAX: a vertex shader alone)
static Found *found;
static size_t nfound, capfound;
static Pair *progs;
static size_t nprogs, capprogs;
static pthread_mutex_t flock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fcond = PTHREAD_COND_INITIALIZER;
static size_t next_job;
static bool scan_done, use_libs;
static _Atomic uint64_t n_named_int;  // [XCDE] T211 (programs built with integer inputs named by the container)
// n_built/n_failed count required base programs only. Extra/named guesses are best-effort: keep their successes separate
// and count their failures as optional, so one unsupported guess cannot make every later run rebuild the same cache.
static _Atomic uint64_t n_built, n_failed, n_extra, n_optional_failed, files_done, n_controls, next_file;
static _Atomic uint64_t n_build_done, n_scan_failed;  // [review] completion/failures must include every required job
static _Atomic int abandon;  // [PRECOMPILE-STALL-FIX] set when a phase gives up: the workers take no new job
static uint64_t files_total;

// [XCDE] T281: the four passes, and the fraction of the whole job each one is. The weights are the previous run's
// measured seconds (cache/precompile_times.txt) or these defaults from a 4-thread PC; the launcher's bar and ETA use
// the overall fraction.
enum { P_SCAN, P_BUILD, P_SEED, P_PIPE, P_NUM };
static double pass_w[P_NUM] = {10, 65, 10, 15};
static bool pass_on[P_NUM] = {true, true, true, true};
static uint64_t pass_ms[P_NUM], t_run;  // t_run: start of the run (ETA)
static _Atomic uint64_t n_build_skipped, n_var_skipped, n_seed_reused;  // (skipped: on the driver-crash list)

// hash -> value + 1 (open addressing; flock held)
typedef struct { uint64_t *k; uint32_t *v; size_t cap, n; } Map;
static Map shader_map, prog_map;
static Map built_jobs;  // [XCDE] T281: programs the build pass made (the seed pass does not rebuild them)
static pthread_mutex_t built_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t map_get(Map *m, uint64_t h) {
    if (!m->cap) return 0;
    if (!h) h = 1;
    for (size_t j = (h ^ (h >> 29)) & (m->cap - 1); m->k[j]; j = (j + 1) & (m->cap - 1)) if (m->k[j] == h) return m->v[j];
    return 0;
}
static void map_put(Map *m, uint64_t h, uint32_t v) {
    if (!h) h = 1;
    if ((m->n + 1) * 2 > m->cap) {
        Map o = *m;
        m->cap = o.cap ? o.cap * 2 : 1 << 16;
        m->k = calloc(m->cap, 8); m->v = calloc(m->cap, 4); m->n = 0;
        for (size_t i = 0; i < o.cap; i++) if (o.k[i]) map_put(m, o.k[i], o.v[i]);
        free(o.k); free(o.v);
    }
    size_t j = (h ^ (h >> 29)) & (m->cap - 1);
    while (m->k[j] && m->k[j] != h) j = (j + 1) & (m->cap - 1);
    if (!m->k[j]) m->n++;
    m->k[j] = h; m->v[j] = v;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

// (flock held) the index of this shader, added if new
static uint32_t add_shader(const uint8_t *code, uint32_t blob, uint32_t coff, uint32_t csz, uint64_t h, int stage) {
    uint32_t v = map_get(&shader_map, h);
    if (v) return v - 1;
    if (nfound == capfound) found = realloc(found, (capfound = capfound ? capfound * 2 : 4096) * sizeof *found);
    Found *f = &found[nfound];
    f->hash = h; f->stage = stage; f->coff = coff; f->csz = csz;
    // [XCDE] T282: not every container names its inputs, and realloc leaves the fields undefined: a shader that no
    // container named had a garbage int_in/named, so the build made random "named input" variants of it - a different
    // set every run (heap contents) and a SPIR-V cache that never converged. Determinism starts here.
    f->int_in = 0; f->named = false;
    uint32_t lim = coff ? coff : 0x10000, sz = lim > blob ? lim : blob;
    f->code = calloc(1, sz);
    memcpy(f->code, code, blob);
    map_put(&shader_map, h, (uint32_t)nfound + 1);
    return (uint32_t)nfound++;
}
static void add_prog(uint32_t vs, uint32_t fs) {
    uint64_t h = found[vs].hash * 0x9E3779B97F4A7C15ull ^ (fs == UINT32_MAX ? 0x5555 : found[fs].hash);
    if (map_get(&prog_map, h)) return;
    if (nprogs == capprogs) progs = realloc(progs, (capprogs = capprogs ? capprogs * 2 : 4096) * sizeof *progs);
    progs[nprogs++] = (Pair){vs, fs};
    map_put(&prog_map, h, 1);
    pthread_cond_signal(&fcond);
}

// [XCDE] T211: each program's vertex input names (container program data, the layout of ScanMountGoat/xc3_lib's
// public description of SPCH, tools/research/spch_attrs.py): which locations the game declares as integers. Monolith names
// them by type - v* float vectors, f* float scalars, n* integers (nWgtIdx, the skinning weight index: location 1 in
// 17,729 character programs, location 0 in 356). A draw binds an integer vertex format there, so the translation key
// has an unsigned input at that location: built that way instead of guessed (the guess was float + "input 1 also
// unsigned"). Returns the number of programs; ctl_of / mask_of: their vertex control sections and integer inputs.
static bool stencil_name(const uint8_t *s, size_t lim) {  // "...Stencil" but not "...ExpStencil" (a float map)
    size_t l = 0;
    while (l < lim && l < 64 && s[l]) l++;
    if (l < 7 || memcmp(s + l - 7, "Stencil", 7)) return false;
    return !(l >= 10 && !memcmp(s + l - 10, "ExpStencil", 10));
}
static size_t spch_inputs(const uint8_t *d, size_t n, size_t b, size_t *ctl_of, uint32_t *mask_of, size_t max) {
    size_t np_ = 0;
    if (b + 0x30 > n) return 0;
    uint32_t nslct = rd32(d + b + 12), list = rd32(d + b + 8), sec = rd32(d + b + 24), ctl = rd32(d + b + 40);
    for (uint32_t i = 0; i < nslct && b + list + 8 * i + 4 <= n; i++) {
        size_t sl = b + sec + rd32(d + b + list + 8 * i);
        if (sl + 48 > n || memcmp(d + sl, "SLCT", 4)) continue;
        uint32_t pn = rd32(d + sl + 16), po = rd32(d + sl + 20);
        size_t cur = rd32(d + sl + 40);
        for (uint32_t k = 0; k < pn && sl + po + 8 * k + 8 <= n; k++) {
            size_t pd = sl + rd32(d + sl + po + 8 * k);
            uint32_t psz = rd32(d + sl + po + 8 * k + 4);
            if (pd + 88 > n || pd + psz > n) return np_;
            uint32_t so = rd32(d + pd + 24), sn = rd32(d + pd + 28);
            if (!sn || so + 20ull * sn > psz) continue;
            const uint8_t *e = d + pd + so + 20 * (sn - 1);
            uint32_t vctl = rd32(e + 12), fctl = rd32(e + 16), an = rd32(d + pd + 80), ao = rd32(d + pd + 84);
            uint32_t mask = 0;
            if (an <= 32 && ao + 8ull * an <= psz)
                for (uint32_t j = 0; j < an; j++) {
                    uint32_t no = rd32(d + pd + ao + 8 * j), loc = rd32(d + pd + ao + 8 * j + 4);
                    if (no + 2 < psz && loc < 32 && d[pd + no] == 'n' && d[pd + no + 1] >= 'A' && d[pd + no + 1] <= 'Z') mask |= 1u << loc;
                }
            uint32_t smask = 0;  // (samplers: count u16 at +0x38, list at +0x3C: name, unk, handle u8, visibility u8, unk u16)
            uint32_t sc = d[pd + 0x38] | (uint32_t)d[pd + 0x39] << 8, sp = rd32(d + pd + 0x3C);
            if (sc <= 32 && sp && sp + 12ull * sc <= psz)
                for (uint32_t j = 0; j < sc; j++) {
                    const uint8_t *se = d + pd + sp + 12 * j;
                    uint32_t no = rd32(se);
                    if (no < psz && se[8] < 32 && se[9] != 0 && stencil_name(d + pd + no, psz - no)) smask |= 1u << se[8];
                }
            if (np_ + 1 < max) {
                ctl_of[np_] = b + ctl + cur; mask_of[np_] = mask; np_++;
                if (fctl) { ctl_of[np_] = b + ctl + cur + vctl; mask_of[np_] = smask; np_++; }  // (its fragment shader)
            }
            cur += vctl + fctl;
        }
    }
    return np_;
}

// one container
static void scan_spch(const uint8_t *d, size_t n, size_t b) {
    if (b + 0x30 > n) return;
    enum { MAXP = 4096 };
    size_t *ctl_of = malloc(MAXP * sizeof *ctl_of);
    uint32_t *mask_of = malloc(MAXP * sizeof *mask_of);
    size_t nnamed = ctl_of && mask_of ? spch_inputs(d, n, b, ctl_of, mask_of, MAXP) : 0, ni = 0;
    size_t code = b + rd32(d + b + 0x20), code_end = code + rd32(d + b + 0x24);
    size_t ctl = b + rd32(d + b + 0x28), ctl_end = ctl + rd32(d + b + 0x2C);
    if (code_end > n || ctl_end > n || code_end < code || ctl_end < ctl) return;
    size_t pos = code;
    uint32_t last_vs = UINT32_MAX;  // a vertex shader waiting for its fragment shader
    for (size_t c = ctl; c + 0x710 <= ctl_end; c += 4) {
        if (rd32(d + c) != 0x98761234u) continue;
        size_t c0 = c;
        uint32_t csz = rd32(d + c + 0x6FC), coff = rd32(d + c + 0x700), blob = rd32(d + c + 0x704);
        if (!blob || blob > (4u << 20)) break;
        size_t t = pos;
        pos = (pos + blob + 255) & ~(size_t)255;
        n_controls++;
        c += 0x6FC;
        if (t + blob > code_end || rd32(d + t) != 0x12345678u) continue;
        uint64_t end = (uint64_t)coff + csz, h = 0xcbf29ce484222325ull;  // (objects.c set_shaders)
        if (end < 0x30 || end > (4u << 20)) end = 0x800;
        if (t + end > n) continue;
        for (uint64_t k = 0; k < end / 8; k++) { uint64_t w; memcpy(&w, d + t + k * 8, 8); h = (h ^ w) * 0x100000001b3ull; }
        uint32_t sph0 = rd32(d + t + 0x30);
        int stage = ((sph0 >> 10) & 0xF) == 1 ? STAGE_VERTEX : ((sph0 >> 10) & 0xF) == 5 ? STAGE_FRAGMENT : -1;
        if (stage < 0) continue;  // (geometry / tessellation: their keys come from the draw; 2 in the whole game)
        pthread_mutex_lock(&flock);
        uint32_t i = add_shader(d + t, blob, coff, csz, h, stage);
        if (nnamed) {  // (programs and control sections come in the same order)
            while (ni < nnamed && ctl_of[ni] < c0) ni++;
            if (ni < nnamed && ctl_of[ni] == c0) { found[i].int_in |= mask_of[ni]; found[i].named = true; }
        }
        if (stage == STAGE_VERTEX) {
            if (last_vs != UINT32_MAX) add_prog(last_vs, UINT32_MAX);
            last_vs = i;
        } else if (last_vs != UINT32_MAX) {
            add_prog(last_vs, i);
            last_vs = UINT32_MAX;
        }
        pthread_mutex_unlock(&flock);
    }
    if (last_vs != UINT32_MAX) { pthread_mutex_lock(&flock); add_prog(last_vs, UINT32_MAX); pthread_mutex_unlock(&flock); }
    free(ctl_of);
    free(mask_of);
}

// every container in a buffer (a file or a decompressed xbc1 block)
static void scan_buf(const uint8_t *d, size_t n) {
    for (size_t i = 0; i + 0x30 <= n; i += 4)
        if (!memcmp(d + i, "HCPS", 4)) scan_spch(d, n, i);
}

// [XCDE] T281: files over 64 MB used to be read and decompressed one at a time (a memory guard for 8 GB PCs) and,
// because of the scan order, they were the last ones: a ~29 s serial tail on a SATA SSD. Big files now come first
// (path_cmp) and several are open at once, so their reads and inflates overlap with the rest of the scan.
// [review] Every source buffer is byte-bounded now, not only files over 64 MB. Decompressed xbc1 blocks have their
// own global budget: an individual block can be 1 GiB, so a file-count limit alone does not bound scanner memory.
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int inflight, slot_inflight, slots;
    uint64_t bytes, limit;
} ScanBudget;
static ScanBudget source_budget = {.mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};
static ScanBudget xbc1_budget = {.mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};
static int scan_slots_for_ram(int ram) { return ram >= 24000 ? 6 : ram >= 12000 ? 4 : ram >= 8000 ? 2 : 1; }
static int scanner_cap_for_ram(int ram) {
    int cap = ram > 0 ? ram / 4 / 256 : 64;  // one scanner per 256 MB of a quarter of RAM; the byte budgets do the real cap
    return cap < 1 ? 1 : cap > 64 ? 64 : cap;
}
static uint64_t scan_limit_for_ram(int ram) {
    uint64_t limit = ram > 0 ? (uint64_t)(unsigned)ram * 1024 * 1024 / 8 : 0;  // (an eighth of RAM per budget)
    return limit < (256ull << 20) ? (256ull << 20) : limit;
}
static void scan_budgets_init(int ram) {
    int slots = scan_slots_for_ram(ram);
    uint64_t limit = scan_limit_for_ram(ram);
    source_budget.slots = xbc1_budget.slots = slots;
    source_budget.limit = xbc1_budget.limit = limit;
}
static void scan_budget_enter(ScanBudget *b, uint64_t size, bool slot_limited) {
    pthread_mutex_lock(&b->mutex);
    // One item is always admitted: on a small-RAM PC it can be larger than the whole budget. Waiting for a byte slot
    // while holding such an item would otherwise deadlock every other scanner. Small source files keep their I/O
    // overlap and are limited only by bytes; only large files and decompressed blocks consume the slot cap.
    while (b->inflight > 0 &&
           ((slot_limited && b->slot_inflight >= b->slots) ||
            (size > b->limit || b->bytes > b->limit - size)))
        pthread_cond_wait(&b->cond, &b->mutex);
    b->inflight++;
    if (slot_limited) b->slot_inflight++;
    b->bytes += size;
    pthread_mutex_unlock(&b->mutex);
}
static void scan_budget_leave(ScanBudget *b, uint64_t size, bool slot_limited) {
    pthread_mutex_lock(&b->mutex);
    b->inflight--;
    if (slot_limited) b->slot_inflight--;
    b->bytes -= size;
    pthread_cond_broadcast(&b->cond);
    pthread_mutex_unlock(&b->mutex);
}
static void scan_failure(const char *path, const char *why) {
    uint64_t n = atomic_fetch_add(&n_scan_failed, 1) + 1;
    if (n <= 20) printf("[precompile] scan failed for %s: %s%c", path, why, 10);
}
// The scan visits every aligned offset, so "xbc1" alone can occur in arbitrary model/shader bytes. XBC1 v1 has a
// version word and a 28-byte, printable/NUL-terminated name; implausible candidates are ignored, not hard failures.
// Once that header is recognized, bad bounds, allocation or zlib data mean a real block could not be scanned.
static bool xbc1_header(const uint8_t *d, size_t i) {
    if (rd32(d + i + 4) != 1) return false;
    for (size_t j = 0; j < 28; j++) {
        uint8_t c = d[i + 0x14 + j];
        if (!c) return true;
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;  // a full-width name need not have a terminator
}
static void scan_file(const char *path) {
    GfSource src;  // [XCDE] T280: from the romfs folder or the .nup package (runtime/services/game_files.c)
    if (!gf_source(path, &src)) { scan_failure(path, "source is missing or unreadable"); return; }
    if (!src.size) return;  // an empty, readable container simply has no shaders
    if (src.size > SIZE_MAX) { scan_failure(path, "source has an invalid size"); return; }
    scan_budget_enter(&source_budget, src.size, src.size > (64ull << 20));
    size_t n = (size_t)src.size;
    uint8_t *d = malloc(n);
    if (!d) {
        scan_failure(path, "source buffer could not be allocated");
        scan_budget_leave(&source_budget, src.size, src.size > (64ull << 20));
        return;
    }
    if (gf_pread(&src, d, src.size, 0) != (int64_t)src.size) {
        scan_failure(path, "source could not be read");
        free(d);
        scan_budget_leave(&source_budget, src.size, src.size > (64ull << 20));
        return;
    }
    scan_buf(d, n);
    // xbc1 blocks: "xbc1", u32 version, u32 decompressed size, u32 compressed size, u32 hash, name; zlib data at +0x30
    for (size_t i = 0; i + 0x30 <= (size_t)n; i += 4) {
        if (memcmp(d + i, "xbc1", 4) || !xbc1_header(d, i)) continue;
        uint32_t dsz = rd32(d + i + 8), csz = rd32(d + i + 12);
        if (!dsz || dsz > (1u << 30) || csz > n - i - 0x30) {
            char why[128];
            snprintf(why, sizeof why, "invalid xbc1 block at offset %zu", i);
            scan_failure(path, why);
            continue;
        }
        scan_budget_enter(&xbc1_budget, dsz, true);
        uint8_t *x = malloc(dsz);
        if (!x) {
            scan_failure(path, "decompressed xbc1 buffer could not be allocated");
        } else {
            uLongf out = dsz;
            int z = uncompress(x, &out, d + i + 0x30, csz);
            if (z != Z_OK) {
                char why[128];
                snprintf(why, sizeof why, "xbc1 decompression failed at offset %zu (%s)", i, zError(z));
                scan_failure(path, why);
            } else if (out != dsz) {
                char why[128];
                snprintf(why, sizeof why, "xbc1 size mismatch at offset %zu (%lu of %u bytes)", i, (unsigned long)out, dsz);
                scan_failure(path, why);
            } else {
                scan_buf(x, out);
            }
            free(x);
        }
        scan_budget_leave(&xbc1_budget, dsz, true);
        i += (size_t)0x30 + (csz & ~3u) - 4;
    }
    free(d);
    scan_budget_leave(&source_budget, src.size, src.size > (64ull << 20));
}

// files that hold shaders (measured over the whole RomFS); the monolib containers (title, menus, lighting, post
// effects) come first: the game needs them first
static bool wanted(const char *name) { return vk_precompile_is_shader_container(name); }
typedef struct { char *p; uint64_t sz; } Path;  // [XCDE] T281: size for the scan order (biggest first, path_cmp)
static Path *paths;
static size_t npaths, cappaths;
static void add_path(const char *p, uint64_t sz) {
    if (npaths == cappaths) paths = realloc(paths, (cappaths = cappaths ? cappaths * 2 : 1024) * sizeof *paths);
    paths[npaths++] = (Path){strdup(p), sz};
}
// [XCDE] T280: the same list from the romfs inside a .nup package
static void list_package(int node, const char *dir) {
    const uint32_t *children;
    if (node < 0 || !gf_pkg_is_dir(node)) {
        scan_failure(dir, "package directory is missing or unreadable");
        return;
    }
    int count = gf_pkg_children(node, &children);
    for (int i = 0; i < count; i++) {
        int child = (int)children[i];
        char p[XCDE_PRECOMPILE_PATH_MAX];
        int len = snprintf(p, sizeof p, "%s/%s", dir, gf_pkg_name(child));
        if (len < 0 || (size_t)len >= sizeof p) {
            scan_failure(dir, "package path is too long");
            continue;
        }
        if (gf_pkg_is_dir(child)) list_package(child, p);
        else if (wanted(gf_pkg_name(child))) {
            GfSource src;  // (a range of the package: its size, for the scan order)
            if (!gf_source(p, &src)) scan_failure(p, "source metadata is missing or unreadable");
            else add_path(p, src.size);
        }
    }
}
static void list_dir(const char *dir) {
    XcdeDir *dd = xcde_opendir(dir);
    if (!dd) {
        int err = errno;
        char why[128];
        snprintf(why, sizeof why, "directory could not be opened: %s", strerror(err));
        scan_failure(dir, why);
        return;
    }
    XcdeDirent *e;
    while ((e = xcde_readdir(dd))) {
        if (e->d_name[0] == '.') continue;
        char p[XCDE_PRECOMPILE_PATH_MAX];
        int len = snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (len < 0 || (size_t)len >= sizeof p) {
            scan_failure(dir, "path is too long");
            continue;
        }
        XcdeFileInfo fi;
        if (!xcde_utf8_file_info(p, &fi)) {
            int err = errno;
            if (!vk_precompile_entry_skippable(e->d_name, e->d_type, err)) {
                char why[128];
                snprintf(why, sizeof why, "metadata could not be read: %s", strerror(err));
                scan_failure(p, why);
            } else {
                printf("[precompile] skipping dangling entry %s: %s%c", p, strerror(err), 10);
            }
            continue;
        }
        if (fi.isdir) list_dir(p);
        else if (fi.isreg && wanted(e->d_name)) add_path(p, fi.size);
    }
    xcde_closedir(dd);
}
static int path_rank(const char *p) { return strstr(p, "/monolib/") ? 0 : strstr(p, ".wishp") ? 1 : 2; }
static int path_cmp(const void *a, const void *b) {
    const Path *x = a, *y = b;
    int rx = path_rank(x->p), ry = path_rank(y->p);
    if (rx != ry) return rx - ry;  // (monolib first: the game needs those first)
    // [XCDE] T281: biggest first inside a class. The >64 MB map/model files were the last files the scan read, one at
    // a time; first (and several at once, source_budget) their I/O and inflate overlap with the small files.
    return x->sz < y->sz ? 1 : x->sz > y->sz ? -1 : 0;
}

// ------------------------------------------------------------------ building
static ShaderKey vkey_guess, fkey_guess;

static void stage_of(NvnProgram *np, int s, const Found *f) {
    np->stages[s].stage = s;
    np->stages[s].code_addr = (uint64_t)(uintptr_t)f->code;
    np->stages[s].const_offset = f->coff;
    np->stages[s].const_size = f->csz;
    np->stages[s].code_hash = f->hash;
    np->stage_mask |= 1u << s;
}

// [XCDE] T240: crash-safe (idea from issn_pc): a driver that crashes on one shader / pipeline crashed every later run
// at the same place. Each thread writes the job it is building to cache/precompile_inflight.bin (8 bytes per thread,
// flushed before the driver is called, zeroed after); the next run moves what is left there to
// cache/precompile_skip.txt and skips those jobs from then on (logged). The file is removed after a clean run.
static FILE *inflight;
static pthread_mutex_t inflight_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int inflight_slots;
static _Thread_local int inflight_slot = -1;
static Map skip_map;
static _Atomic uint64_t n_skipped;
static uint64_t job_key(const void *a, size_t na, uint64_t salt) {
    uint64_t h = 0xcbf29ce484222325ull ^ salt;
    for (size_t i = 0; i < na; i++) { h ^= ((const uint8_t *)a)[i]; h *= 0x100000001B3ull; }
    return h ? h : 1;
}
static bool job_begin(uint64_t key) {  // false: skip it (it crashed the driver before)
    if (skip_map.n && map_get(&skip_map, key)) { atomic_fetch_add(&n_skipped, 1); return false; }
    if (!inflight) return true;
    if (inflight_slot < 0) inflight_slot = atomic_fetch_add(&inflight_slots, 1);
    pthread_mutex_lock(&inflight_lock);
    fseek(inflight, (long)inflight_slot * 8, SEEK_SET);
    fwrite(&key, 8, 1, inflight);
    fflush(inflight);
    pthread_mutex_unlock(&inflight_lock);
    {   // (tests: XCDE_TEST_PRECOMPILE_CRASH=<n> - the process dies at the n-th job, as a crashing driver would)
        static _Atomic int n;
        static int at = -1;
        if (at < 0) at = getenv("XCDE_TEST_PRECOMPILE_CRASH") ? atoi(getenv("XCDE_TEST_PRECOMPILE_CRASH")) : 0;
        if (at && atomic_fetch_add(&n, 1) + 1 == at) { fflush(stdout); _exit(3); }
    }
    return true;
}
static void job_end(void) {
    if (!inflight || inflight_slot < 0) return;
    uint64_t z = 0;
    pthread_mutex_lock(&inflight_lock);
    fseek(inflight, (long)inflight_slot * 8, SEEK_SET);
    fwrite(&z, 8, 1, inflight);
    fflush(inflight);
    pthread_mutex_unlock(&inflight_lock);
}
static void inflight_open(void) {
    FILE *f = fopen("cache/precompile_inflight.bin", "rb");
    if (f) {  // the last run died: what it was building goes to the skip list
        FILE *o = fopen("cache/precompile_skip.txt", "a");
        uint64_t k;
        int n = 0;
        while (fread(&k, 8, 1, f) == 1) if (k) { if (o) fprintf(o, "%016llx\n", (unsigned long long)k); n++; }
        if (o) fclose(o);
        fclose(f);
        if (n) printf("[precompile] the last run stopped while building %d jobs: they are skipped from now on (cache/precompile_skip.txt)\n", n);
    }
    FILE *sk = fopen("cache/precompile_skip.txt", "r");
    char line[64];
    while (sk && fgets(line, sizeof line, sk)) { uint64_t k = strtoull(line, NULL, 16); if (k) map_put(&skip_map, k, 1); }
    if (sk) fclose(sk);
    if (skip_map.n) printf("[precompile] %zu jobs on the skip list (they crashed the driver)\n", skip_map.n);
    inflight = fopen("cache/precompile_inflight.bin", "wb+");
    if (inflight) { static uint8_t zero[64 * 8]; fwrite(zero, 1, sizeof zero, inflight); fflush(inflight); }
}

// [XCDE] T281: identity of one program build: the shader contents and the translation keys that the translator reads
// (vs_out_mask is replaced from the vertex shader, so it never makes two jobs different). The seed pass asks it, and
// skips the 5,484 jobs whose keys are exactly the guessed ones the build pass already made.
static uint64_t job_ident(const Found *vs, const Found *fs, const ShaderKey *vk, const ShaderKey *fk) {
    ShaderKey a = *vk, b = fk ? *fk : (ShaderKey){0};
    uint64_t vh = vs->hash, fh = fs ? fs->hash : 0;
    a.vs_out_mask = b.vs_out_mask = 0;
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *parts[4] = {(const uint8_t *)&vh, (const uint8_t *)&fh, (const uint8_t *)&a, (const uint8_t *)&b};
    size_t lens[4] = {8, 8, sizeof a, sizeof b};
    for (int i = 0; i < 4; i++) for (size_t k = 0; k < lens[i]; k++) { h ^= parts[i][k]; h *= 0x100000001B3ull; }
    return h ? h : 1;
}
static void job_built(const Found *vs, const Found *fs, const ShaderKey *vk, const ShaderKey *fk) {
    pthread_mutex_lock(&built_lock);
    map_put(&built_jobs, job_ident(vs, fs, vk, fk), 1);
    pthread_mutex_unlock(&built_lock);
}
static void build_(const Found *vs, const Found *fs);
static void build(Pair pr, const Found *vs, const Found *fs) {
    (void)pr;
    uint64_t hk[2] = {vs->hash, fs ? fs->hash : 0};
    if (!job_begin(job_key(hk, sizeof hk, 1))) {
        atomic_fetch_add(&n_build_skipped, 1);
        atomic_fetch_add(&n_build_done, 1);
        return;  // (the build loop counts it)
    }
    build_(vs, fs);
    job_end();
    atomic_fetch_add(&n_build_done, 1);
}
static void build_(const Found *vs, const Found *fs) {
    NvnProgram np;
    memset(&np, 0, sizeof np);
    stage_of(&np, 0, vs);
    if (fs) stage_of(&np, 1, fs);
    uint32_t attr = 0;
    int ok = vk_precompile_program(&np, &vkey_guess, &fkey_guess, &attr);
    if (ok) { job_built(vs, fs, &vkey_guess, &fkey_guess); atomic_fetch_add(&n_built, 1); }
    else atomic_fetch_add(&n_failed, 1);
    if (ok && (attr & 2)) {  // (vertex input 1 read: also as an unsigned integer)
        ShaderKey k = vkey_guess;
        k.attr_kind[1] = 2;
        if (vk_precompile_program(&np, &k, &fkey_guess, NULL)) {
            job_built(vs, fs, &k, &fkey_guess);  // [review] only if built
            atomic_fetch_add(&n_extra, 1);
        } else atomic_fetch_add(&n_optional_failed, 1);
    }
    // [XCDE] T211: in addition, the versions the container's names point to (they say what CAN be an integer: a
    // draw without that data binds a float format there - measured: names alone missed 170 variants, the guess 25)
    if (ok && vs->named && (vs->int_in & ~2u)) {  // integer inputs other than location 1 (nWgtIdx at 0: 356 programs)
        ShaderKey k = vkey_guess;
        for (int l = 0; l < 32; l++) if (vs->int_in >> l & 1) k.attr_kind[l] = 2;
        if (vk_precompile_program(&np, &k, &fkey_guess, NULL)) {
            job_built(vs, fs, &k, &fkey_guess);
            atomic_fetch_add(&n_named_int, 1);
        } else atomic_fetch_add(&n_optional_failed, 1);
    }
    if (ok && fs && fs->named && fs->int_in) {  // stencil textures: also read as integers
        ShaderKey f = fkey_guess;
        f.tex_uint = fs->int_in;
        bool named_ok = true;
        if (vk_precompile_program(&np, &vkey_guess, &f, NULL)) job_built(vs, fs, &vkey_guess, &f);
        else { named_ok = false; atomic_fetch_add(&n_optional_failed, 1); }
        if (attr & 2) {
            ShaderKey k = vkey_guess;
            k.attr_kind[1] = 2;
            if (vk_precompile_program(&np, &k, &f, NULL)) job_built(vs, fs, &k, &f);
            else { named_ok = false; atomic_fetch_add(&n_optional_failed, 1); }
        }
        if (named_ok) atomic_fetch_add(&n_named_int, 1);
    }
}

// [XCDE] T211: the shipped variant seed (data/variants_seed.bin, exec.c T163 records: code hashes [3] u64, has_gs u8,
// gs_in u8, 6 pad, vertex ShaderKey, fragment ShaderKey, pipeline key): each distinct (shaders, keys) built exactly -
// the versions the guesses above miss (integer textures / targets / vertex inputs: they follow the data a draw binds,
// not the shader). Geometry-stage programs are skipped.
typedef struct { uint32_t vs, fs; ShaderKey vkey, fkey; } SeedJob;
static SeedJob *seed_jobs;
static size_t nseed;
static _Atomic size_t next_seed;
static _Atomic uint64_t n_seed_built, n_seed_failed;
static _Atomic size_t n_seed_done;  // [PRECOMPILE-STALL-FIX] seed jobs finished (n_seed_built counts only the successes)
// [XCDE] T240: and every recorded variant's whole optimized pipeline (exec.c vk_precompile_variant): the shipped list
// (the debugger tours of every map and cutscene, tools/tours/tour_batch.py) plus the player's own cache/variants.bin
extern size_t vk_precompile_variant_size(void);
extern bool vk_precompile_variant_valid(const void *rec, size_t rec_size);
extern int vk_precompile_variant(const NvnProgram *np, const void *rec, size_t rec_size);
typedef struct { uint32_t vs, fs; uint8_t *rec; } VarJob;
static VarJob *var_jobs;
static size_t nvar, var_rec_size;
static _Atomic size_t next_var;
static _Atomic uint64_t n_var_built, n_var_failed;
static Map var_done;
static uint64_t optional_built_total(void) {
    return atomic_load(&n_extra) + atomic_load(&n_named_int) + atomic_load(&n_seed_built) + atomic_load(&n_var_built);
}
static uint64_t optional_failed_total(void) {
    return atomic_load(&n_optional_failed) + atomic_load(&n_seed_failed) + atomic_load(&n_var_failed);
}
static void load_seed_file(const char *path, Map *done);
static void load_seed(void) {
    Map done = {0};
    // [XCDE] T-LINUX: the AppImage exports the bundled seed path.
    char seed_env[1100];
    const char *seed = xcde_utf8_getenv("XCDE_VARIANTS_SEED", seed_env, sizeof seed_env) ? seed_env : NULL;
    load_seed_file(seed ? seed : "data/variants_seed.bin", &done);
    load_seed_file("cache/variants.bin", &done);
}
static void load_seed_file(const char *path, Map *done_) {
    Map *done = done_;
    FILE *f = xcde_utf8_fopen(path, "rb");
    if (!f) return;
    uint32_t hdr[2];
    if (fread(hdr, 4, 2, f) != 2 || hdr[0] != 0x56435843u) {
        printf("[precompile] warning: skipping variant file %s: invalid header%c", path, 10);
        fclose(f);
        return;
    }
    size_t rec_size = vk_precompile_variant_size();
    if (hdr[1] != rec_size) {
        printf("[precompile] warning: skipping incompatible variant file %s: record size %u, expected %zu%c",
               path, hdr[1], rec_size, 10);
        fclose(f);
        return;
    }
    uint8_t *rec = malloc(rec_size);
    if (!rec) {
        printf("[precompile] warning: skipping variant file %s: record buffer could not be allocated%c", path, 10);
        fclose(f);
        return;
    }
    size_t bad = 0;
    while (fread(rec, rec_size, 1, f) == 1) {
        // [review] A record that passes the size check can still carry ncolor > 8 or other out-of-range fields and
        // corrupt pipeline_whole()'s fixed arrays. Validate before it becomes a job; vk_precompile_variant repeats the
        // check as the last line of defence.
        if (!vk_precompile_variant_valid(rec, rec_size)) {
            if (bad++ < 20) printf("[precompile] warning: skipping invalid variant record in %s%c", path, 10);
            continue;
        }
        uint64_t h[3];
        memcpy(h, rec, 24);
        if (rec[24]) continue;  // (has_gs)
        uint32_t vi = map_get(&shader_map, h[0]), fi = h[1] ? map_get(&shader_map, h[1]) : 0;
        if (!vi || (h[1] && !fi)) continue;  // (a shader the scan didn't find)
        SeedJob j = {vi - 1, fi ? fi - 1 : UINT32_MAX};
        memcpy(&j.vkey, rec + 32, sizeof(ShaderKey));
        memcpy(&j.fkey, rec + 32 + sizeof(ShaderKey), sizeof(ShaderKey));
        uint64_t k = 0xcbf29ce484222325ull;
        const uint8_t *b = (const uint8_t *)&j;
        for (size_t i = 0; i < sizeof j; i++) { k ^= b[i]; k *= 0x100000001B3ull; }
        {   // (the whole record: one pipeline each)
            uint64_t h = 0xcbf29ce484222325ull;
            for (uint32_t i = 0; i < hdr[1]; i++) { h ^= rec[i]; h *= 0x100000001B3ull; }
            if (!h) h = 1;
            if (!map_get(&var_done, h) && (!var_rec_size || var_rec_size == hdr[1])) {
                map_put(&var_done, h, 1);
                var_rec_size = hdr[1];
                var_jobs = realloc(var_jobs, (nvar + 1) * sizeof *var_jobs);
                var_jobs[nvar].vs = j.vs, var_jobs[nvar].fs = j.fs;
                var_jobs[nvar].rec = malloc(hdr[1]);
                memcpy(var_jobs[nvar++].rec, rec, hdr[1]);
            }
        }
        if (!k) k = 1;
        if (map_get(done, k)) continue;
        map_put(done, k, 1);
        seed_jobs = realloc(seed_jobs, (nseed + 1) * sizeof *seed_jobs);
        seed_jobs[nseed++] = j;
    }
    if (bad) printf("[precompile] warning: skipped %zu invalid variant record(s) in %s%c", bad, path, 10);
    free(rec);
    fclose(f);
}
static void *var_worker(void *arg) {
    (void)arg;
    plat_thread_background();
    for (size_t i; !atomic_load(&abandon) && (i = atomic_fetch_add(&next_var, 1)) < nvar;) {
        VarJob *j = &var_jobs[i];
        NvnProgram np;
        memset(&np, 0, sizeof np);
        stage_of(&np, 0, &found[j->vs]);
        if (j->fs != UINT32_MAX) stage_of(&np, 1, &found[j->fs]);
        if (!job_begin(job_key(j->rec, var_rec_size, 3))) { atomic_fetch_add(&n_var_skipped, 1); continue; }
        atomic_fetch_add(vk_precompile_variant(&np, j->rec, var_rec_size) ? &n_var_built : &n_var_failed, 1);
        job_end();
    }
    return NULL;
}
static void *seed_worker(void *arg) {
    (void)arg;
    plat_thread_background();
    for (size_t i; !atomic_load(&abandon) && (i = atomic_fetch_add(&next_seed, 1)) < nseed;) {
        SeedJob *j = &seed_jobs[i];
        Found *vsf = &found[j->vs], *fsf = j->fs != UINT32_MAX ? &found[j->fs] : NULL;
        if (map_get(&built_jobs, job_ident(vsf, fsf, &j->vkey, &j->fkey))) {  // [XCDE] T281: made by the build pass
            atomic_fetch_add(&n_seed_reused, 1);
            atomic_fetch_add(&n_seed_done, 1);
            continue;
        }
        NvnProgram np;
        memset(&np, 0, sizeof np);
        stage_of(&np, 0, vsf);
        if (fsf) stage_of(&np, 1, fsf);
        if (!job_begin(job_key(j, sizeof *j, 2))) { atomic_fetch_add(&n_seed_done, 1); continue; }
        if (vk_precompile_program(&np, &j->vkey, &j->fkey, NULL)) atomic_fetch_add(&n_seed_built, 1);
        else atomic_fetch_add(&n_seed_failed, 1);
        job_end();
        atomic_fetch_add(&n_seed_done, 1);
    }
    return NULL;
}

// [PRECOMPILE-STALL-FIX] A driver call that never returns (NVIDIA / Intel stuck inside a whole-pipeline compile) left
// this process waiting forever at the launcher's 100 %. The phases now watch their own progress: if no job finished
// for XCDE_PRECOMPILE_STALL_S seconds (default 120, 0 = never) the run stops the others, keeps what was in flight in
// cache/precompile_inflight.bin and exits with code 2, so the launcher restarts it by itself (crash-safe, T240) and the
// new run skips the stuck jobs.
static bool stalled(const char *phase, uint64_t count) {
    static char cur[16];
    static uint64_t lastc, lastt;
    static int limit_s = -1;
    if (limit_s < 0) limit_s = getenv("XCDE_PRECOMPILE_STALL_S") ? atoi(getenv("XCDE_PRECOMPILE_STALL_S")) : 120;
    uint64_t now = now_ns();
    if (strcmp(cur, phase) || count != lastc) { snprintf(cur, sizeof cur, "%s", phase); lastc = count; lastt = now; return false; }
    return limit_s > 0 && now - lastt > (uint64_t)limit_s * 1000000000ull;
}
static void give_up(const char *phase, uint64_t count) {
    printf("[precompile] %s: no job finished for a long time (%llu done): a driver call is stuck. Stopping the others; the stuck jobs go to the skip list and the run restarts%c", phase, (unsigned long long)count, 10);
    fflush(stdout);
    atomic_store(&abandon, 1);
    struct timespec ts = {3, 0};
    nanosleep(&ts, NULL);  // (the others finish the job they are on and stop: what stays in the file is what is stuck)
    if (inflight) fflush(inflight);
    _exit(2);
}

// [XCDE] T282: started after the scan (see vk_precompile_main): the named-input masks it reads are final then, so the
// same shaders always produce the same variants (a deterministic SPIR-V cache).
static void *worker(void *arg) {
    (void)arg;
    plat_thread_background();
    for (;;) {
        pthread_mutex_lock(&flock);
        while (next_job >= nprogs && !scan_done) pthread_cond_wait(&fcond, &flock);
        if (next_job >= nprogs || atomic_load(&abandon)) { pthread_mutex_unlock(&flock); return NULL; }
        Pair pr = progs[next_job++];
        Found vs = found[pr.vs], fs = pr.fs != UINT32_MAX ? found[pr.fs] : (Found){0};
        pthread_mutex_unlock(&flock);
        build(pr, &vs, pr.fs != UINT32_MAX ? &fs : NULL);
    }
}

static void *scanner(void *arg) {
    (void)arg;
    plat_thread_background();
    for (uint64_t i; (i = atomic_fetch_add(&next_file, 1)) < npaths;) {
        scan_file(paths[i].p);
        atomic_fetch_add(&files_done, 1);
    }
    return NULL;
}

// [XCDE] T281: the four passes. The launcher's bar and its ETA use the overall fraction; the log line keeps the raw
// counts. Weights: this run's measured pass times when a previous run left cache/precompile_times.txt (the fastest, a
// warm rerun, and a refresh after translator changes have very different profiles, so a fixed split would lie), else
// a 4-thread full run.
static const char *pass_name(int p) {
    static const char *n[P_NUM] = {"scan", "build", "seed", "pipelines"};
    return p >= 0 && p < P_NUM ? n[p] : "done";
}
static void pass_weights_load(void) {
    double w[P_NUM] = {10, 65, 10, 15};
    FILE *f = fopen("cache/precompile_times.txt", "r");
    char name[16];
    double sec;
    while (f && fscanf(f, "%15s %lf", name, &sec) == 2) {
        if (!(sec > 0.0)) continue;
        for (int i = 0; i < P_NUM; i++) if (!strcmp(name, pass_name(i))) w[i] = sec;
    }
    if (f) fclose(f);
    for (int i = 0; i < P_NUM; i++) pass_w[i] = w[i];
}
static void pass_save_times(void) {
    FILE *f = fopen("cache/precompile_times.tmp", "w");
    if (!f) return;
    for (int i = 0; i < P_NUM; i++) fprintf(f, "%s %.3f%c", pass_name(i), pass_ms[i] / 1000.0, 10);
    fclose(f);
    remove("cache/precompile_times.txt");
    rename("cache/precompile_times.tmp", "cache/precompile_times.txt");
}
static uint64_t pass_done(int p) {
    switch (p) {
    case P_SCAN: return atomic_load(&files_done);
    case P_BUILD: return atomic_load(&n_build_done);
    case P_SEED: return atomic_load(&n_seed_done);
    case P_PIPE: return n_var_built + n_var_failed + atomic_load(&n_var_skipped);
    }
    return 0;
}
static uint64_t pass_total(int p) {
    switch (p) {
    case P_SCAN: return files_total;
    case P_BUILD: return nprogs;
    case P_SEED: return nseed;
    case P_PIPE: return nvar;
    }
    return 0;
}
static void progress(int pass, bool force) {
    static uint64_t last, last_print;
    uint64_t now = now_ns();
    if (!force && now - last < 200000000ull) return;
    last = now;
    uint64_t done = pass < P_NUM ? pass_done(pass) : 1, total = pass < P_NUM ? pass_total(pass) : 1;
    if (total && done > total) done = total;
    double frac = total ? (double)done / (double)total : 1.0;
    double tw = 0, dw = 0;
    for (int i = 0; i < P_NUM; i++) {
        if (!pass_on[i]) continue;
        tw += pass_w[i];
        if (i < pass) dw += pass_w[i];
        else if (i == pass) dw += pass_w[i] * frac;
    }
    double overall = pass >= P_NUM ? 1.0 : tw > 0 ? dw / tw : 0.0;
    // "<phase> <overall done> <overall total> <this pass done> <this pass total>": the launcher's bar is the overall
    // fraction and its text the pass' own count (tools/launcher/launcher.cpp pc_poll; tools/gamebuild/gamebuild.py).
    FILE *f = fopen("cache/precompile_progress.tmp", "w");
    if (f) {
        fprintf(f, "%s %llu 1000 %llu %llu%c", pass_name(pass), (unsigned long long)(overall * 1000.0 + 0.5),
                (unsigned long long)done, (unsigned long long)total, 10);
        fclose(f);
        remove("cache/precompile_progress.txt");
        rename("cache/precompile_progress.tmp", "cache/precompile_progress.txt");
    }
    if (force || now - last_print > 2000000000ull) {
        last_print = now;
        double elapsed = (now - t_run) / 1e9;
        double eta = overall > 0.02 ? elapsed * (1.0 - overall) / overall : 0.0;
        printf("[precompile] %s: %llu/%llu (%.0f%%, ~%.0f s left) - %zu shaders, %zu programs; %llu base programs built, %llu optional variants built, %llu base failures, %llu optional failures, %llu skipped%c",
               pass_name(pass), (unsigned long long)done, (unsigned long long)total, overall * 100.0, eta, nfound, nprogs,
               (unsigned long long)atomic_load(&n_built), (unsigned long long)optional_built_total(),
               (unsigned long long)atomic_load(&n_failed), (unsigned long long)optional_failed_total(),
               (unsigned long long)atomic_load(&n_skipped), 10);
        fflush(stdout);
    }
}

// [XCDE] T282: the precompile is current when the done stamp's `sig` line names the same inputs (vk_precompile_sig) and
// the SPIR-V cache it describes is still there. Then nothing has to be scanned, translated or compiled.
static bool file_sig(const char *path, char out[64]) {  // the `sig <hex>` line, "" when there is none
    out[0] = 0;
    FILE *f = xcde_utf8_fopen(path, "r");
    if (!f) return false;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "sig ", 4) && sscanf(line + 4, "%63s", out) == 1) { found = true; break; }
    fclose(f);
    return found;
}
static bool stamp_sig(char out[64]) { return file_sig("cache/precompile_done.txt", out); }
static bool spirv_loose_mode(void) {
    const char *e = getenv("XCDE_SPIRV_LOOSE");
    return e && atoi(e) != 0;
}
static bool spirv_persistence_disabled(void) { return getenv("LARECOMP_VK_NO_SPIRV_CACHE") != NULL; }

// Cache packs can exceed 2 GiB, so use the shared wide/64-bit metadata helper rather than the Windows CRT's stat.
static bool cache_file_size(const char *path, uint64_t *size) {
    XcdeFileInfo fi;
    if (!xcde_utf8_file_info(path, &fi) || !fi.isreg) return false;
    *size = fi.size;
    return true;
}
static bool cache_file_nonempty(const char *path) {
    uint64_t size;
    return cache_file_size(path, &size) && size > 0;
}
static bool cache_has_loose_spv(void) {
    XcdeDir *d = xcde_opendir("cache/spirv");
    if (!d) return false;
    bool found = false;
    XcdeDirent *e;
    while (!found && (e = xcde_readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 4 || strcmp(e->d_name + n - 4, ".spv")) continue;
        char path[2048];
        int len = snprintf(path, sizeof path, "cache/spirv/%s", e->d_name);
        if (len > 0 && (size_t)len < sizeof path && cache_file_nonempty(path)) found = true;
    }
    xcde_closedir(d);
    return found;
}
static bool precompile_current(const char *sig) {
    if (spirv_persistence_disabled()) return false;  // this run cannot leave a reusable SPIR-V cache
    char old[64];
    if (!stamp_sig(old) || strcmp(old, sig)) return false;
    if (spirv_loose_mode()) {
        char loose[64];
        return file_sig("cache/precompile_loose_done.txt", loose) && !strcmp(loose, sig) && cache_has_loose_spv();
    }
    return cache_file_nonempty("cache/spirv.pack") && cache_file_nonempty("cache/spirv.idx");
}
static bool remove_precompile_file(const char *path) {
    if (xcde_utf8_remove(path) == 0 || errno == ENOENT) return true;
    printf("[precompile] cannot remove %s: %s%c", path, strerror(errno), 10);
    return false;
}
static bool write_sig_stamp(const char *path, const char *sig) {
    FILE *f = xcde_utf8_fopen(path, "w");
    if (!f) {
        printf("[precompile] cannot write %s: %s%c", path, strerror(errno), 10);
        return false;
    }
    bool ok = fprintf(f, "sig %s%c", sig, 10) >= 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        xcde_utf8_remove(path);
        printf("[precompile] cannot finish writing %s%c", path, 10);
    }
    return ok;
}
static bool write_done_stamp(const char *sig, double secs) {
    FILE *f = xcde_utf8_fopen("cache/precompile_done.txt", "w");
    if (!f) {
        printf("[precompile] cannot write cache/precompile_done.txt: %s%c", strerror(errno), 10);
        return false;
    }
    bool ok = fprintf(f, "programs %zu base_built %llu base_failed %llu optional_built %llu optional_failed %llu skipped %llu shaders %zu seconds %.0f%c",
                      nprogs, (unsigned long long)atomic_load(&n_built), (unsigned long long)atomic_load(&n_failed),
                      (unsigned long long)optional_built_total(), (unsigned long long)optional_failed_total(),
                      (unsigned long long)atomic_load(&n_skipped), nfound, secs, 10) >= 0;
    // [XCDE] T282: the signature of the inputs this run used (runtime/nvn/vulkan/precompile_sig.c). The launcher and
    // RecompLauncher's builder answer "prepared?" by recomputing it (`--precompile-status`), so a rebuild that
    // cannot translate shaders differently keeps the cache. Before T211 this was the executable's size and time,
    // and every new build precompiled again (the translator fixes that change SPIR-V made that safe, T211).
    if (ok) ok = fprintf(f, "sig %s%c", sig, 10) >= 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        xcde_utf8_remove("cache/precompile_done.txt");
        printf("[precompile] cannot finish writing cache/precompile_done.txt%c", 10);
    }
    return ok;
}

// `xcde --precompile-status <dump>`: 0 = the precompiled shaders are current for this build and these game files,
// 1 = they need preparing, 2 = the game files cannot be read, 3 = SPIR-V persistence is disabled (not applicable).
// No cache, Vulkan device or window is touched.
int vk_precompile_status(const char *root) {
    // (a runtime mode, not a property of the dump: report it before touching the game files; --precompile-status is a
    // debug/verification path, so an unreadable root in this mode is still "not applicable", not "unreadable")
    if (spirv_persistence_disabled()) {
        printf("[precompile] SPIR-V persistence is disabled (LARECOMP_VK_NO_SPIRV_CACHE): precompilation is not applicable%c", 10);
        return 3;
    }
    settings_load();  // [review] the same xcde.ini GPU pipeline policy the precompile applies is a signature input
    char sig[17], old[64];
    if (!vk_precompile_sig_policy(root, g_settings.gpu_gpl, g_settings.gpu_profile, sig)) {
        printf("[precompile] cannot read the game files under %s: cannot tell whether the precompiled shaders are current%c", root, 10);
        return 2;
    }
    if (precompile_current(sig)) {
        printf("[precompile] the precompiled shaders are current (signature %s)%c", sig, 10);
        return 0;
    }
    bool have = stamp_sig(old);
    const char *reason;
    if (spirv_persistence_disabled()) reason = "SPIR-V persistence is disabled (LARECOMP_VK_NO_SPIRV_CACHE is set)";
    else if (!have) reason = "they were never made for this build and these game files";
    else if (strcmp(old, sig)) reason = "the shader translator, its SPIR-V options, its environment switches or the game files changed";
    else reason = spirv_loose_mode() ? "cache/precompile_loose_done.txt is missing or cache/spirv has no .spv files"
                                     : "cache/spirv.pack or cache/spirv.idx is missing";
    printf("[precompile] the precompiled shaders need preparing: %s%c", reason, 10);
    return 1;
}

// Start only as many threads as the kernel accepts. A partial start can still finish the queue with less concurrency;
// no thread at all would leave the progress loops waiting forever, so that case is a clean, unstamped failure.
static int start_threads(pthread_t *threads, int count, void *(*fn)(void *), const char *phase) {
    int started = 0;
    for (int i = 0; i < count; i++) {
        int err = pthread_create(&threads[i], NULL, fn, NULL);
        if (err) {
            printf("[precompile] %s: pthread_create failed after %d of %d threads: %s%c", phase, started, count, strerror(err), 10);
            break;
        }
        started++;
    }
    if (!started) {
        printf("[precompile] %s: no worker threads could be started%c", phase, 10);
        fflush(stdout);
        _exit(2);
    }
    return started;
}

int vk_precompile_main(const char *root) {
    uint64_t t0 = now_ns();
    // [review] Load xcde.ini before LARECOMP_HIDDEN makes settings_load() ignore it: the GPU pipeline policy decides
    // whether this run warms libraries or whole pipelines, and it must be both applied and part of the signature.
    settings_load();
    char sig[17];
    if (!vk_precompile_sig_policy(root, g_settings.gpu_gpl, g_settings.gpu_profile, sig)) {
        printf("[precompile] cannot read the game files under %s%c", root, 10);
        return 2;
    }
    const char *force = getenv("XCDE_PRECOMPILE_FORCE");
    bool forced = force && atoi(force) != 0;  // ([XCDE] T282 review: the manual launcher button sets it for its child)
    if (!forced && precompile_current(sig)) {
        printf("[precompile] the precompiled shaders are current (signature %s): nothing to do (XCDE_PRECOMPILE_FORCE=1 to run anyway)%c", sig, 10);
        return 0;
    }
    char romfs[XCDE_PRECOMPILE_PATH_MAX];
    int romfs_len = snprintf(romfs, sizeof romfs, "%s/romfs", root);
    if (romfs_len < 0 || (size_t)romfs_len >= sizeof romfs) {
        printf("[precompile] cannot read the game files under %s: the path is too long%c", root, 10);
        return 2;
    }
    mkdir("cache", 0755);
    mkdir("cache/spirv", 0755);
    t_run = t0;
    pass_weights_load();  // [XCDE] T281: the bar's split between the passes (the previous run's measured times)
    vkey_guess.glsl = fkey_guess.glsl = 1;
    vkey_guess.depth_minus_one = true;  // (the game's depth mode and window origin, as exec.c sets them per draw)
    // [XCDE] T280: the dump is a folder or a .nup package read in place
    char why[768];
    if (!gf_init(root, why, sizeof why)) {
        printf("[precompile] cannot read the game files: %s\n", why);
        return 2;
    }
    if (gf_is_package()) list_package(gf_pkg_node(romfs), romfs);
    else list_dir(romfs);
    files_total = npaths;
    if (!npaths) {
        uint64_t scan_failures = atomic_load(&n_scan_failed);
        if (scan_failures) {  // (a missing/corrupt romfs is unreadable, not a stale cache the callers should retry forever)
            printf("[precompile] cannot read the game files under %s: %llu scan failures and no shader files found%c",
                   root, (unsigned long long)scan_failures, 10);
            return 2;
        }
        rt_logf("[precompile] no shader files under %s%c", romfs, 10);
        return 1;
    }
    qsort(paths, npaths, sizeof *paths, path_cmp);
    { const char *e = getenv("XCDE_PRECOMPILE_LIBS"); use_libs = !e || atoi(e) != 0; }
    setenv("XCDE_PRECOMPILING", "1", 1);  // (shaders.c: no cache-miss log lines)
    setenv("LARECOMP_HIDDEN", "1", 1);
    ui_window_init();  // (the game's own Vulkan device - same driver caches - in a hidden window)
    if (!use_libs) g_vk.gpl = false;
    // [XCDE] Issue #261: when GPL is active, keep a fresh empty VkPipelineCache during the GPL shader-library
    // passes (build + seed) and save it to cache/vk_pipelines.bin (capped <= 192 MiB) BEFORE the whole-pipeline pass
    // starts. That protects precompiled GPL shader libraries from NVIDIA's global driver cache eviction without
    // bloating cache/vk_pipelines.bin with thousands of whole pipelines (~700 MB previously).
    vkDestroyPipelineCache(g_vk.dev, g_vk.pcache, NULL);
    g_vk.pcache = VK_NULL_HANDLE;
    if (g_vk.gpl) {
        VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) g_vk.pcache = VK_NULL_HANDLE;
    }
    // Both the game files and Vulkan are usable now. Invalidate completion before any destructive cleanup, so a failed
    // spawn, root validation or device initialization keeps a valid stamp. The loose marker belongs to the old mode too.
    if (!remove_precompile_file("cache/precompile_done.txt") ||
        !remove_precompile_file("cache/precompile_loose_done.txt")) return 2;
    if (forced) {  // [XCDE] T282 review: an explicit redo retries the jobs that once crashed the driver. The launcher no
                   // longer deletes these files itself, so a spawn that never started cannot lose the skip list.
        if (!remove_precompile_file("cache/precompile_inflight.bin") ||
            !remove_precompile_file("cache/precompile_skip.txt")) return 2;
    }
    inflight_open();  // [XCDE] T240 crash-safe
    // [XCDE] T281: this is offline work (the game is not running), so use the whole CPU - the in-game worker count
    // (xcde_shader_workers: cpus-4, i.e. 4 threads on this 8-thread PC) left half the cores idle in the compiles.
    int cpus = plat_cpu_count();
    int nthreads = cpus > 2 ? cpus - 1 : 1;
    if (getenv("XCDE_PRECOMPILE_THREADS")) nthreads = atoi(getenv("XCDE_PRECOMPILE_THREADS"));
    extern int SDL_GetSystemRAM(void);
    int ram = SDL_GetSystemRAM();  // the same RAM figure bounds both driver workers and scanner admission
    {   // [XCDE] T240 (idea from SavageWindRecomp 66b110a): the driver compiles take memory per thread - measured here
        // ~1.1 GB peak for 15 threads on NVIDIA, but other drivers' largest shaders take far more: at most one thread
        // per 400 MB of a quarter of the RAM (8 GB: 5 threads; the min-spec 4-core CPU has 3 anyway)
        int cap = ram > 0 ? ram / 4 / 400 : nthreads;
        if (!getenv("XCDE_PRECOMPILE_THREADS") && cap < nthreads) {
            printf("[precompile] %d threads (memory: %d MB of RAM)%c", cap < 1 ? 1 : cap, ram, 10);
            nthreads = cap;
        }
    }
    if (!getenv("XCDE_PRECOMPILE_THREADS") && vk_amd_igpu_vendor_driver() && nthreads > 2) {  // [VEGA-FIX] (not RADV)
        printf("[precompile] AMD integrated GPU: 2 threads%c", 10);
        nthreads = 2;
    }
    printf("[precompile] GPU: %s (vendor 0x%04x), pipeline libraries %s%c", g_vk.props.deviceName, (unsigned)g_vk.props.vendorID,
           g_vk.gpl ? "on" : "off", 10);
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;
    scan_budgets_init(ram);
    // [XCDE] T281: the scan is I/O + zlib bound; 4 scanners are enough on this class of PC (more only compete with the
    // build workers), but on a many-core PC half the cores feed it. [review] A separate RAM-aware cap preserves
    // small-file overlap on 8 GB PCs; source_budget.slots limits only files larger than 64 MiB.
    int nscanners = nthreads > 4 ? (cpus >= 12 ? cpus / 2 : 4) : nthreads;
    if (nscanners < 1) nscanners = 1;
    if (nscanners > scanner_cap_for_ram(ram)) nscanners = scanner_cap_for_ram(ram);
    if (nscanners > 64) nscanners = 64;  // [review] (sc[] is 64: a 192-thread machine must not write past it)
    // [XCDE] T282: the build starts after the scan. T281 overlapped it (4 workers during the scan), but a build reads the
    // shaders' named-input masks (found[].int_in/named) and the scanners are still merging them (the same shader found
    // in a later container can add a name): which variants got built depended on the thread timing, so every run
    // translated a different set of sources, stored its own SPIR-V and the next run compiled ~7,000 stages again - the
    // cache never converged (three full runs compiled 19,878, 7,685 and 6,959 stages). The scan is I/O-bound, so the
    // overlap was worth seconds; a cache that does not converge costs minutes on every later run.
    printf("[precompile] %d worker threads + %d scanners of %d logical CPUs%c", nthreads, nscanners, cpus, 10);
    fflush(stdout);
    pthread_t th[64], sc[64];
    uint64_t t_scan0 = now_ns();
    int nscan_started = start_threads(sc, nscanners, scanner, "scan");
    while (files_done < npaths) {
        progress(P_SCAN, false);
        struct timespec ts = {0, 50000000};
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < nscan_started; i++) pthread_join(sc[i], NULL);
    pass_ms[P_SCAN] = (now_ns() - t_scan0) / 1000000;
    pthread_mutex_lock(&flock);
    scan_done = true;
    pthread_cond_broadcast(&fcond);
    pthread_mutex_unlock(&flock);
    int nbuild_started = start_threads(th, nthreads, worker, "build");
    // [XCDE] T281: the variant files are read now, not after the build pass: the bar needs their counts from the start
    // (they only depend on the scan's shader map). The workers keep building this thread's maps.
    load_seed();  // [XCDE] T211
    pass_on[P_SEED] = nseed > 0;
    bool pipelines_run = nvar > 0;  // [XCDE] T240: the whole optimized pipelines
    const char *pipe_env = getenv("XCDE_PRECOMPILE_PIPELINES");
    // [VEGA-FIX] AMD integrated GPUs: the driver compiles of whole pipelines are what crashed there - off unless =1
    // (AMD's driver only: Mesa RADV, the Steam Deck's, builds them - XCDE_PRECOMPILE_PIPELINES=0 still turns it off)
    bool amd_igpu = vk_amd_igpu_vendor_driver();
    if (pipelines_run && amd_igpu && !(pipe_env && atoi(pipe_env) == 1)) pipelines_run = false;
    if (pipelines_run && pipe_env && atoi(pipe_env) == 0) pipelines_run = false;
    pass_on[P_PIPE] = pipelines_run;
    printf("[precompile] %zu programs, %zu seed variants, %zu recorded pipelines to prepare%c", nprogs, nseed, nvar, 10);
    fflush(stdout);
    uint64_t t_build0 = now_ns();
    while (atomic_load(&n_build_done) < nprogs) {
        uint64_t done = atomic_load(&n_build_done);
        if (stalled("build", done)) give_up("build", done);
        progress(P_BUILD, false);
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < nbuild_started; i++) pthread_join(th[i], NULL);
    pass_ms[P_BUILD] = (now_ns() - t_build0) / 1000000;
    if (nseed) {
        uint64_t ts0 = now_ns();
        int nseed_started = start_threads(th, nthreads, seed_worker, "seed");
        while (atomic_load(&n_seed_done) < nseed) {
            if (stalled("seed", atomic_load(&n_seed_done))) give_up("seed", atomic_load(&n_seed_done));
            progress(P_SEED, false); struct timespec ts = {0, 100000000}; nanosleep(&ts, NULL);
        }
        for (int i = 0; i < nseed_started; i++) pthread_join(th[i], NULL);
        pass_ms[P_SEED] = (now_ns() - ts0) / 1000000;
        printf("[precompile] seed (optional): %zu variants, %llu built, %llu failed, %llu already built by the program pass, in %.1f s%c", nseed,
               (unsigned long long)n_seed_built, (unsigned long long)atomic_load(&n_seed_failed),
               (unsigned long long)atomic_load(&n_seed_reused), pass_ms[P_SEED] / 1000.0, 10);
    }
    if (g_vk.pcache) {
        extern size_t vk_pipeline_cache_save_force(void);
        size_t saved = vk_pipeline_cache_save_force();
        if (saved) printf("[precompile] saved GPL library pipeline cache (%.1f MB)%c", saved / (1024.0 * 1024.0), 10);
        vkDestroyPipelineCache(g_vk.dev, g_vk.pcache, NULL);
        g_vk.pcache = VK_NULL_HANDLE;
    }
    if (nvar) {  // [XCDE] T240: the whole optimized pipelines (XCDE_PRECOMPILE_PIPELINES=0: pieces only, as before)
        if (!pipelines_run && amd_igpu) {
            printf("[precompile] AMD integrated GPU: whole-pipeline precompile skipped (XCDE_PRECOMPILE_PIPELINES=1 to force)%c", 10);
        }
        if (pipelines_run) {
            uint64_t tv0 = now_ns();
            int npipe_started = start_threads(th, nthreads, var_worker, "pipelines");
            while (atomic_load(&next_var) < nvar || n_var_built + n_var_failed + n_var_skipped < nvar) {
                if (stalled("pipelines", n_var_built + n_var_failed + n_var_skipped)) give_up("pipelines", n_var_built + n_var_failed + n_var_skipped);
                progress(P_PIPE, false);
                struct timespec ts = {0, 100000000};
                nanosleep(&ts, NULL);
            }
            for (int i = 0; i < npipe_started; i++) pthread_join(th[i], NULL);
            pass_ms[P_PIPE] = (now_ns() - tv0) / 1000000;
            printf("[precompile] pipelines (optional): %zu recorded variants, %llu built, %llu failed, in %.1f s%c", nvar,
                   (unsigned long long)n_var_built, (unsigned long long)n_var_failed, pass_ms[P_PIPE] / 1000.0, 10);
        }
    }
    pass_save_times();  // [XCDE] T281: the next run's bar uses these pass durations
    if (inflight) { fclose(inflight); inflight = NULL; remove("cache/precompile_inflight.bin"); }  // (a clean run)
    if (n_skipped) printf("[precompile] %llu jobs skipped (they are on the driver-crash list cache/precompile_skip.txt; press Precompile shaders in the launcher to retry them)\n", (unsigned long long)n_skipped);
    double secs = (now_ns() - t0) / 1e9;
    uint64_t scan_failed = atomic_load(&n_scan_failed), seed_failed = atomic_load(&n_seed_failed);
    uint64_t pipeline_failed = atomic_load(&n_var_failed), guessed_failed = atomic_load(&n_optional_failed);
    uint64_t optional_failed = optional_failed_total(), optional_built = optional_built_total();
    printf("[precompile] %zu files, %llu control sections, %zu distinct shaders, %zu programs in %.1f s (scan %.1f s, build %.1f s, seed %.1f s, pipelines %.1f s): "
           "%llu base programs built, %llu base program failures; %llu optional variants built (%llu input-1, %llu named, %llu seed, %llu pipelines), "
           "%llu optional failures (%llu guessed, %llu seed, %llu pipelines), %llu jobs skipped; %llu SPIR-V compiled, %llu from the cache; "
           "%d workers + %d scanners; %llu scan failures%c",
           npaths, (unsigned long long)n_controls, nfound, nprogs, secs, pass_ms[P_SCAN] / 1000.0, pass_ms[P_BUILD] / 1000.0,
           pass_ms[P_SEED] / 1000.0, pass_ms[P_PIPE] / 1000.0, (unsigned long long)atomic_load(&n_built),
           (unsigned long long)atomic_load(&n_failed), (unsigned long long)optional_built, (unsigned long long)atomic_load(&n_extra),
           (unsigned long long)atomic_load(&n_named_int), (unsigned long long)atomic_load(&n_seed_built),
           (unsigned long long)atomic_load(&n_var_built), (unsigned long long)optional_failed, (unsigned long long)guessed_failed,
           (unsigned long long)seed_failed, (unsigned long long)pipeline_failed, (unsigned long long)atomic_load(&n_skipped),
           (unsigned long long)vk_spirv_misses, (unsigned long long)vk_spirv_hits, nthreads, nscanners,
           (unsigned long long)scan_failed, 10);
    if (optional_failed)
        printf("[precompile] warning: %llu optional builds failed (%llu guessed variants, %llu seed jobs, %llu recorded pipelines); optional work is best-effort and does not by itself invalidate the cache stamp%c",
               (unsigned long long)optional_failed, (unsigned long long)guessed_failed, (unsigned long long)seed_failed,
               (unsigned long long)pipeline_failed, 10);
    char list_env[1100];
    const char *list = xcde_utf8_getenv("XCDE_PRECOMPILE_LIST", list_env, sizeof list_env) ? list_env : NULL;
    if (list) {  // (tests: "S <hash> <stage>" per shader, "P <vs hash> <fs hash>" per program)
        FILE *l = xcde_utf8_fopen(list, "w");
        for (size_t i = 0; l && i < nfound; i++) fprintf(l, "S %016llx %d%c", (unsigned long long)found[i].hash, found[i].stage, 10);
        for (size_t i = 0; l && i < nprogs; i++)
            fprintf(l, "P %016llx %016llx%c", (unsigned long long)found[progs[i].vs].hash,
                    (unsigned long long)(progs[i].fs != UINT32_MAX ? found[progs[i].fs].hash : 0), 10);
        if (l) fclose(l);
    }
    // Crash-list skips are deliberate and may remain skipped. Only an incomplete scan or a failed required base program
    // may block the stamp. Extra/named guesses, seed jobs and whole pipelines are best-effort warmup.
    if (scan_failed || n_failed) {
        printf("[precompile] incomplete required work: %llu scan failures, %llu base program failures; no current cache stamp was written%c",
               (unsigned long long)scan_failed, (unsigned long long)atomic_load(&n_failed), 10);
        fflush(stdout);
        _exit(1);
    }
    progress(P_NUM, true);
    if (spirv_persistence_disabled()) {
        printf("[precompile] SPIR-V persistence is disabled (LARECOMP_VK_NO_SPIRV_CACHE is set): work finished, but no current cache stamp was written%c", 10);
        fflush(stdout);
        _exit(0);
    }
    if (!write_done_stamp(sig, secs)) { fflush(stdout); _exit(1); }
    if (spirv_loose_mode() && !write_sig_stamp("cache/precompile_loose_done.txt", sig)) {
        remove_precompile_file("cache/precompile_done.txt");
        fflush(stdout);
        _exit(1);
    }
    fflush(stdout);
    _exit(0);  // (no atexit work: the pipeline cache is saved above; the game's exit handler is for the game)
}
