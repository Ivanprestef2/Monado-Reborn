// [XCDE] Native FSR 4.1.2 Vulkan integration via the FidelityFX API.
// - Linux: loads amd_fidelityfx_upscaler_vk.so (auz466/fsr4up-vk); defaults to FP8 on RDNA 4 and FSR 4 Lite INT8 on
//   RDNA 2, RDNA 3, Intel Arc, and other non-FP8 GPUs (unless FSR4UP_INT8 is set).
// - Windows: loads AMD's signed amd_fidelityfx_upscaler_vk.dll (FidelityFX SDK 2.4.0).
#include "fsr4.h"
#include "vk.h"
#include "../../host/utf8_path.h"  // [XCDE] T278: UTF-8 paths through the wide APIs
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_upscale.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#if defined(__linux__) || defined(_WIN32)

#ifdef _WIN32
#include <windows.h>
#define FSR4_LIBRARY_NAME "amd_fidelityfx_upscaler_vk.dll"
static void *fsr4_open(const char *name) { return xcde_utf8_load_library(name); }
static void *fsr4_sym(void *m, const char *n) { return (void *)GetProcAddress((HMODULE)m, n); }
static void fsr4_close(void *m) { FreeLibrary((HMODULE)m); }
static const char *fsr4_error(void) {
    static char msg[64];
    snprintf(msg, sizeof msg, "LoadLibrary error %lu", (unsigned long)GetLastError());
    return msg;
}
static bool fsr4_beside_exe(char *path, size_t n, const char *leaf) {
    if (!plat_exe_path(path, n)) return false;
    char *slash = strrchr(path, '\\');
    if (!slash || slash - path + 1 + (ptrdiff_t)strlen(leaf) >= (ptrdiff_t)n) return false;
    strcpy(slash + 1, leaf);
    return true;
}
#else
#include <dlfcn.h>
#include <unistd.h>
#define FSR4_LIBRARY_NAME "amd_fidelityfx_upscaler_vk.so"
static void *fsr4_open(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
static void *fsr4_sym(void *m, const char *n) { return dlsym(m, n); }
static void fsr4_close(void *m) { dlclose(m); }
static const char *fsr4_error(void) { return dlerror(); }
static bool fsr4_beside_exe(char *path, size_t n, const char *leaf) {
    ssize_t got = readlink("/proc/self/exe", path, n - 1);
    if (got <= 0) return false;
    path[got] = 0;
    char *slash = strrchr(path, '/');
    if (!slash || slash - path + 1 + (ptrdiff_t)strlen(leaf) >= (ptrdiff_t)n) return false;
    strcpy(slash + 1, leaf);
    return true;
}
#endif

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK 0x0000003u
#ifndef FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION
#define FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION (1u << 11)
#endif

struct fsr4_vk_device_ctx {
    VkDevice vkDevice;
    VkPhysicalDevice vkPhysicalDevice;
    PFN_vkGetDeviceProcAddr vkDeviceProcAddr;
};
struct fsr4_backend_vk_desc {
    ffxCreateContextDescHeader header;
    struct fsr4_vk_device_ctx deviceContext;
};

static PFN_vkVoidFunction VKAPI_PTR fsr4_vk_proc(VkDevice d, const char *name) {
    PFN_vkVoidFunction fn = vkGetDeviceProcAddr(d, name);
    if (fn) return fn;
    char core[128];
    size_t len = strlen(name);
    if (len > 3 && len < sizeof core && !strcmp(name + len - 3, "KHR")) {
        memcpy(core, name, len - 3);
        core[len - 3] = 0;
        fn = vkGetDeviceProcAddr(d, core);
    }
    return fn;
}

static void fsr4_message(uint32_t type, const wchar_t *msg) {
    char buf[512];
    size_t n = 0;
    for (; msg && msg[n] && n < sizeof buf - 1; n++) buf[n] = (char)(msg[n] < 128 ? msg[n] : '?');
    buf[n] = 0;
    LOG("[fsr4] %s: %s\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", buf);
}

static struct FfxApiResource fsr4_res(const MonadoFsr4Image *img, uint32_t ffx_fmt, uint32_t usage) {
    struct FfxApiResource r;
    memset(&r, 0, sizeof r);
    r.resource = (void *)img->image;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffx_fmt;
    r.description.width = img->width;
    r.description.height = img->height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.usage = usage;
    r.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;  // images are in VK_IMAGE_LAYOUT_GENERAL
    return r;
}

bool fsr4_device_ok;
bool fsr4_fp8_ok;
static PfnFfxCreateContext ffx4_create;
static PfnFfxDestroyContext ffx4_destroy;
static PfnFfxDispatch ffx4_dispatch;
static PfnFfxQuery ffx4_query;
static uint64_t fsr4_ver_id;
static int load_state;

bool fsr4_available(void) {
    if (!fsr4_device_ok) return false;
    if (g_vk.sc_extent.width > 5120 || g_vk.sc_extent.height > 2880) {
        static uint32_t warned_w, warned_h;
        if (warned_w != g_vk.sc_extent.width || warned_h != g_vk.sc_extent.height) {
            warned_w = g_vk.sc_extent.width; warned_h = g_vk.sc_extent.height;
            LOG("[fsr4] %ux%u output exceeds FSR 4 limits (5120x2880); using spatial FSR\n", warned_w, warned_h);
        }
        return false;
    }
    if (load_state) return load_state > 0;
    load_state = -1;

    const char *mode_desc = fsr4_fp8_ok ? "FP8" : "INT8";
#ifdef __linux__
    // Default network policy on Linux (fsr4up-vk): FP8 on discrete AMD RDNA 4, FSR 4 Lite INT8 on RDNA 2, RDNA 3,
    // Intel Arc, and other non-FP8 GPUs. Respects an explicit user FSR4UP_INT8 override.
    const char *user_net = getenv("FSR4UP_INT8");
    if (user_net && *user_net) {
        mode_desc = user_net;
    } else if (!fsr4_fp8_ok) {
        setenv("FSR4UP_INT8", "lite", 0);
        mode_desc = "Lite INT8 (default for RDNA 2/3, Intel Arc, non-FP8)";
    } else {
        mode_desc = "FP8 (RDNA 4 default)";
    }
#endif

    char module_env[1100];
    const char *lib = xcde_utf8_getenv("MONADO_FSR4_LIBRARY", module_env, sizeof module_env) ? module_env : NULL;
    void *module = fsr4_open(lib ? lib : FSR4_LIBRARY_NAME);
    if (!module && !lib) {
        char path[4096];
        if (fsr4_beside_exe(path, sizeof path, FSR4_LIBRARY_NAME))
            module = fsr4_open(path);
    }
    if (!module) {
        LOG("[fsr4] %s unavailable: %s\n", FSR4_LIBRARY_NAME, fsr4_error());
        return false;
    }
    ffx4_create = (PfnFfxCreateContext)fsr4_sym(module, "ffxCreateContext");
    ffx4_destroy = (PfnFfxDestroyContext)fsr4_sym(module, "ffxDestroyContext");
    ffx4_dispatch = (PfnFfxDispatch)fsr4_sym(module, "ffxDispatch");
    ffx4_query = (PfnFfxQuery)fsr4_sym(module, "ffxQuery");
    if (!ffx4_create || !ffx4_destroy || !ffx4_dispatch || !ffx4_query) {
        LOG("[fsr4] %s: missing FFX API entry points\n", FSR4_LIBRARY_NAME);
        fsr4_close(module);
        return false;
    }
    // AMD's Windows DLL bundles FSR 4.1.2, 3.1.6 and 2.3.4; select an FSR 4.x provider explicitly so unsupported GPUs
    // do not silently fall back to FSR 3.1.6 under the FSR 4 option.
    struct fsr4_vk_device_ctx dctx = {g_vk.dev, g_vk.phys, fsr4_vk_proc};
    uint64_t ids[8] = {0};
    const char *names[8] = {0};
    uint64_t count = 8;
    struct ffxQueryDescGetVersions qv = {
        .header = {.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS},
        .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
        .device = &dctx,
        .outputCount = &count,
        .versionIds = ids,
        .versionNames = names,
    };
    const char *ver_name = NULL;
    fsr4_ver_id = 0;
    if (ffx4_query(NULL, &qv.header) == FFX_API_RETURN_OK) {
        for (uint64_t i = 0; i < count && i < 8; i++) {
            if (names[i] && names[i][0] == '4' && names[i][1] == '.') {
                fsr4_ver_id = ids[i];
                ver_name = names[i];
                break;
            }
        }
    }
    if (!fsr4_ver_id) {
        LOG("[fsr4] %s reports no compatible FSR 4 provider on this GPU (%s)\n", FSR4_LIBRARY_NAME, mode_desc);
        fsr4_close(module);
        return false;
    }
    load_state = 1;
    LOG("[fsr4] native Vulkan FSR %s ready (%s)\n", ver_name ? ver_name : "4.1.2", mode_desc);
    return true;
}

typedef struct Retired { ffxContext ctx; uint64_t seq; struct Retired *next; } Retired;
static ffxContext ctx;
static Retired *retired;
static uint32_t width, height, ultra, max_rw, slot;
static uint64_t ring[8], last_use;

bool fsr4_record(const MonadoFsr4Frame *f) {
    if (!fsr4_available() || !f->output.width || !f->output.height || f->output.width > 5120 || f->output.height > 2880)
        return false;
    uint64_t complete = vk_completed_seq();
    Retired **at = &retired;
    while (*at) {
        Retired *r = *at;
        if (r->seq <= complete) {
            *at = r->next;
            ffx4_destroy(&r->ctx, NULL);
            free(r);
        } else {
            at = &r->next;
        }
    }
    uint32_t want_max_rw = f->ultra ? f->color.width : f->output.width;
    uint32_t want_max_rh = f->ultra ? f->color.height : f->output.height;
    bool changed = !ctx || width != f->output.width || height != f->output.height ||
                   ultra != f->ultra || (f->ultra && max_rw != want_max_rw);
    if (changed) {
        LOG("[fsr4-reset] context rebuild: initial=%d output-size=%d model=%d; %ux%u -> %ux%u\n",
            !ctx, ctx && (width != f->output.width || height != f->output.height), ctx && ultra != f->ultra,
            width, height, f->output.width, f->output.height);
        static int fsr4_inv = -1;
        if (fsr4_inv < 0) { const char *e = getenv("XCDE_FSR4_DEPTH_INVERT"); fsr4_inv = !(e && *e == '0'); }
        static int fsr4_dbg = -1;
        if (fsr4_dbg < 0) { const char *e = getenv("XCDE_TMP_DEBUG"); fsr4_dbg = e && atoi(e) != 0; }
        struct fsr4_backend_vk_desc be = {
            .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK},
            .deviceContext = {g_vk.dev, g_vk.phys, fsr4_vk_proc},
        };
        struct ffxOverrideVersion ov = {
            .header = {.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION, .pNext = &be.header},
            .versionId = fsr4_ver_id,
        };
        uint32_t cflags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION |
                          FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
        if (fsr4_inv) cflags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        if (fsr4_dbg) cflags |= FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
        struct ffxCreateContextDescUpscale cd = {
            .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, .pNext = &ov.header},
            .flags = cflags,
            .maxRenderSize = {want_max_rw, want_max_rh},
            .maxUpscaleSize = {f->output.width, f->output.height},
            .fpMessage = fsr4_message,
        };
        ffxContext fresh = NULL;
        ffxReturnCode_t rc = ffx4_create(&fresh, &cd.header, NULL);
        if (rc != FFX_API_RETURN_OK || !fresh) {
            LOG("[fsr4] ffxCreateContext failed: %u\n", rc);
            load_state = -1;
            return false;
        }
        if (ctx) {
            Retired *r = malloc(sizeof(*r));
            if (!r) { ffx4_destroy(&fresh, NULL); return false; }
            *r = (Retired){ctx, last_use, retired};
            retired = r;
        }
        ctx = fresh;
        width = f->output.width;
        height = f->output.height;
        ultra = f->ultra;
        max_rw = want_max_rw;
        slot = 0;
        memset(ring, 0, sizeof(ring));
        LOG("[fsr4] FSR 4.1.2 %ux%u, %s preset\n", width, height, ultra ? "ultra" : "standard");
    }
    if (ring[slot] > complete) {
        if (ring[slot] >= vk_pending_seq()) return false;
        vk_wait_seq(ring[slot]);
    }
    struct ffxDispatchDescUpscale d;
    memset(&d, 0, sizeof d);
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = (void *)f->cmd;
    d.color = fsr4_res(&f->color, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.depth = fsr4_res(&f->depth, FFX_API_SURFACE_FORMAT_R32_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.motionVectors = fsr4_res(&f->motion, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.output = fsr4_res(&f->output, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset.x = f->jitter[0];
    d.jitterOffset.y = f->jitter[1];
    d.motionVectorScale.x = (float)f->color.width;
    d.motionVectorScale.y = (float)f->color.height;
    d.renderSize.width = f->color.width;
    d.renderSize.height = f->color.height;
    d.upscaleSize.width = f->output.width;
    d.upscaleSize.height = f->output.height;
    d.enableSharpening = f->sharpness > 0.0f;
    d.sharpness = f->sharpness;
    d.frameTimeDelta = 16.7f;
    d.preExposure = 1.0f;
    d.reset = (f->reset || changed) != 0;
    {
        static int fsr4_dbg = -1;
        if (fsr4_dbg < 0) { const char *e = getenv("XCDE_TMP_DEBUG"); fsr4_dbg = e && atoi(e) != 0; }
        d.flags = fsr4_dbg ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0;
    }
    ffxReturnCode_t rc = ffx4_dispatch(&ctx, &d.header);
    if (rc != FFX_API_RETURN_OK) {
        LOG("[fsr4] ffxDispatch failed: %u\n", rc);
        load_state = -1;
        return false;
    }
    ring[slot] = last_use = vk_pending_seq();
    slot = (slot + 1) % 8;
    return true;
}
#endif
