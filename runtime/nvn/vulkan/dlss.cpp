#ifdef _WIN32  // [XCDE] T-LINUX: the Streamline integration below is Windows-only
#define VK_USE_PLATFORM_WIN32_KHR
// [XCDE] Streamline runtime for NVIDIA DLSS Super Resolution (T257) and NVIDIA Reflex (T272), through
// third_party/streamline (MIT): sl.interposer.dll, sl.common.dll, sl.dlss.dll + nvngx_dlss.dll, sl.reflex.dll +
// sl.pcl.dll and NvLowLatencyVk.dll next to the exe. Manual hooking mode: the Vulkan API is called directly; before
// the instance / device are created the loaded plugins' extensions, features and queues are added (device.c), the
// device is then handed to Streamline (slSetVulkanInfo), and the swapchain calls it intercepts go through its proxies
// (dlss_proxy). Loaded at run time. DLSS runs only on an RTX GPU with the upscaler chosen; Reflex only on an NVIDIA
// GPU with `reflex` enabled (xcde.ini / XCDE_REFLEX) and does not need RTX. Without the DLLs, the GPU or the option
// nothing of this runs.
//
// DLSS: per frame (dlss_evaluate): constants (camera, jitter), DLSS options (quality mode, output size), the tagged
// inputs (colour, depth, motion vectors at render size; output at window size) and the evaluation into the command
// buffer. Reflex: one Streamline frame token per game frame (shared with DLSS), PCL markers around simulation, render
// submit and present, and slReflexSleep at the frame boundary (reflex.h, framepace.c, objects.c, device.c, exec.c).
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <windows.h>
#include <vulkan/vulkan.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_hooks.h>
#include <sl_helpers_vk.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include "../../host/nvidia_signature.h"
#include "gpu_scan.h"
#include "../../dlss_policy.h"

extern "C" {
#include "temporal.h"
#include "reflex.h"
#include "../../settings.h"
#include "../../diag/diag.h"
}
#define DLOG(...) fprintf(stderr, __VA_ARGS__)

static HMODULE sl_mod;
static bool sl_initialized, sl_rt_ok, sl_surface_ready;
// requirements loaded this run (the plugin exists and answered slGetFeatureRequirements)
static bool req_dlss_ok, req_reflex_ok, req_pcl_ok;
// the device/driver actually accepts the feature
static bool dlss_ok, reflex_ok;
static bool reflex_checked;  // the device support check ran (a failure is not "restart to enable")
static char dlss_note[256] = "No inicializado; se comprueba al iniciar";
static char reflex_note[256] = "No inicializado; se comprueba al iniciar";
static PFN_vkGetInstanceProcAddr p_proxyInstanceProcAddr;
static PFun_slShutdown *p_slShutdown;
extern "C" const char *dlss_status(void) { return dlss_note; }
extern "C" const char *reflex_status(void) { return reflex_note; }
static void note_set(char *dst, const char *text) {
    snprintf(dst, 256, "%s", text);
    DLOG("%s %s\n", dst == dlss_note ? "[dlss]" : "[reflex]", text);
}
static void dlss_shutdown(void) {
    // [XCDE] T272: do NOT call slShutdown() at process exit. Streamline's teardown unloads sl.dlss/sl.common while
    // its NGX/DLSS worker threads can still be executing them; closing the window then faults on the unloaded code
    // (crash_20261118_201438: execute c0000005 at sl.dlss.dll+0x4ba50, host thread 0x2018, after 2409 frames). The
    // process is exiting, so leave the runtime mapped and let Windows reclaim it.
    sl_initialized = sl_rt_ok = sl_surface_ready = false;
    req_dlss_ok = req_reflex_ok = req_pcl_ok = dlss_ok = reflex_ok = false;
    reflex_checked = false;
}
static PFun_slInit *p_slInit;
static PFun_slGetFeatureRequirements *p_slGetFeatureRequirements;
static PFun_slSetVulkanInfo *p_slSetVulkanInfo;
static PFun_slIsFeatureSupported *p_slIsFeatureSupported;
static PFun_slGetFeatureFunction *p_slGetFeatureFunction;
static PFun_slGetNewFrameToken *p_slGetNewFrameToken;
static PFun_slSetConstants *p_slSetConstants;
static PFun_slSetTagForFrame *p_slSetTagForFrame;
static PFun_slEvaluateFeature *p_slEvaluateFeature;
static PFun_slDLSSSetOptions *p_slDLSSSetOptions;
static PFun_slReflexGetState *p_slReflexGetState;
static PFun_slReflexSetOptions *p_slReflexSetOptions;
static PFun_slReflexSleep *p_slReflexSleep;
static PFun_slPCLSetMarker *p_slPCLSetMarker;
static PFun_slPCLSetOptions *p_slPCLSetOptions;
static PFN_vkGetDeviceProcAddr p_proxyDeviceProcAddr;
static sl::FeatureRequirements req_dlss, req_reflex, req_pcl;

// ---- Reflex state (T272)
static int g_reflex_mode;  // REFLEX_OFF / ON / BOOST applied with slReflexSetOptions
static std::mutex token_lock;
#define SL_TOKEN_RING 8  // Streamline's MAX_FRAMES_IN_FLIGHT is 6; the port queues at most 2 presents
static sl::FrameToken *token_ring[SL_TOKEN_RING];
static uint64_t token_ring_frame[SL_TOKEN_RING];
static std::atomic<uint64_t> g_game_frame{0};
static std::atomic<uint64_t> g_present_frame{0};
static std::atomic<int> g_reflex_pending{-1};  // a menu change: applied on the limiter thread (slReflexSetOptions is not thread safe)
static uint32_t fallback_frame;  // DLSS-only token counter (when Reflex is not loaded)

static bool reflex_ready(void) {
    return reflex_ok && sl_surface_ready && p_slReflexSleep && p_slPCLSetMarker;
}

static sl::FrameToken *token_for(uint64_t frame) {
    if (!frame || !p_slGetNewFrameToken) return nullptr;
    std::lock_guard<std::mutex> lock(token_lock);
    size_t i = (size_t)(frame % SL_TOKEN_RING);
    if (token_ring[i] && token_ring_frame[i] == frame) return token_ring[i];
    sl::FrameToken *t = nullptr;
    uint32_t idx = (uint32_t)frame;
    if (p_slGetNewFrameToken(t, &idx) != sl::Result::eOk || !t) return nullptr;
    token_ring[i] = t;
    token_ring_frame[i] = frame;
    return t;
}

static void marker_for(int which, uint64_t frame) {
    if (!p_slPCLSetMarker) return;
    sl::FrameToken *t = token_for(frame);
    if (t) p_slPCLSetMarker((sl::PCLMarker)(unsigned)which, *t);
}

static void reflex_apply_mode(int mode) {
    mode = mode == REFLEX_BOOST ? REFLEX_BOOST : mode == REFLEX_ON ? REFLEX_ON : REFLEX_OFF;
    g_reflex_mode = mode;
    if (!p_slReflexSetOptions) return;
    sl::ReflexOptions o{};
    o.mode = mode == REFLEX_BOOST ? sl::ReflexMode::eLowLatencyWithBoost
           : mode == REFLEX_ON ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
    sl::Result r = p_slReflexSetOptions(o);
    if (r != sl::Result::eOk) {
        snprintf(reflex_note, sizeof reflex_note, "Reflex: no se pudo aplicar el modo (SDK %d)", (int)r);
        DLOG("[streamline] %s\n", reflex_note);
    }
}

static int reflex_env_mode(void) {
    const char *e = getenv("XCDE_REFLEX");
    if (!e || !*e) return -1;
    int v = atoi(e);
    return v >= 2 ? REFLEX_BOOST : v >= 1 ? REFLEX_ON : REFLEX_OFF;
}
static bool reflex_sleep_enabled(void) {
    static int on = -1;  // XCDE_REFLEX_NOSLEEP=1: diagnostic A/B (markers + proxy stay on)
    if (on < 0) on = getenv("XCDE_REFLEX_NOSLEEP") ? 0 : 1;
    return on != 0;
}
static int reflex_setting(void) {
    int e = reflex_env_mode();
    if (e >= 0) return e;
    if (diag_deterministic()) return REFLEX_OFF;  // (replays: no driver sleeps, no markers)
    return g_settings.reflex == REFLEX_BOOST ? REFLEX_BOOST : g_settings.reflex == REFLEX_ON ? REFLEX_ON : REFLEX_OFF;
}
static bool dlss_requested(void) {
    if (getenv("XCDE_TMP_OFF")) return false;
    return g_settings.upscaler_auto || g_settings.upscaler == UPSCALE_DLSS;
}

static bool feature_reqs_supported(const sl::FeatureRequirements &r, const VkPhysicalDeviceVulkan12Features &h12,
                                   const VkPhysicalDeviceVulkan13Features &h13, const VkExtensionProperties *ext,
                                   uint32_t next) {
    for (uint32_t i = 0; i < r.vkNumDeviceExtensions; ++i) {
        bool found = false;
        for (uint32_t j = 0; j < next; ++j) found |= !strcmp(r.vkDeviceExtensions[i], ext[j].extensionName);
        if (!found) return false;
    }
    VkPhysicalDeviceVulkan12Features need12 = sl::getVkPhysicalDeviceVulkan12Features(r.vkNumFeatures12, r.vkFeatures12);
    VkPhysicalDeviceVulkan13Features need13 = sl::getVkPhysicalDeviceVulkan13Features(r.vkNumFeatures13, r.vkFeatures13);
    const VkBool32 *h = &h12.samplerMirrorClampToEdge, *need = &need12.samplerMirrorClampToEdge;
    for (size_t i = 0; i < (offsetof(VkPhysicalDeviceVulkan12Features, subgroupBroadcastDynamicId) + sizeof(VkBool32) -
                            offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge)) / sizeof(VkBool32); ++i)
        if (need[i] && !h[i]) return false;
    h = &h13.robustImageAccess;
    need = &need13.robustImageAccess;
    for (size_t i = 0; i < (offsetof(VkPhysicalDeviceVulkan13Features, maintenance4) + sizeof(VkBool32) -
                            offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess)) / sizeof(VkBool32); ++i)
        if (need[i] && !h[i]) return false;
    return true;
}

static bool feature_requirements(sl::Feature f, sl::FeatureRequirements &r) {
    return p_slGetFeatureRequirements && p_slGetFeatureRequirements(f, r) == sl::Result::eOk;
}

static bool file_next_to(const wchar_t *dir, const wchar_t *name) {
    wchar_t p[MAX_PATH + 32];
    if (swprintf(p, MAX_PATH + 32, L"%ls\\%ls", dir, name) < 0) return false;
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static void sl_log(sl::LogType type, const char *msg) {
    if (type != sl::LogType::eInfo) DLOG("[streamline] %s", msg);
}

// Before the Vulkan instance: load Streamline with the plugins DLSS and/or Reflex need (only when requested).
extern "C" bool dlss_init_early(void) {
    bool want_d = dlss_requested();
    int rmode = reflex_setting();
    bool want_r = rmode != REFLEX_OFF;
    if (!want_d) note_set(dlss_note, "DLSS no solicitado (elige Automático o DLSS y reinicia)");
    if (!want_r) note_set(reflex_note, "Reflex no solicitado (xcde.ini reflex = off)");
    if (!want_d && !want_r) return false;
    uint32_t count = 0;
    char error[512];
    XcdeGpu *adapters = xcde_gpu_scan(&count, error, sizeof error);
    XcdeGpuInfo *infos = count ? (XcdeGpuInfo *)calloc(count, sizeof *infos) : nullptr;
    if (!adapters || !infos) { free(adapters); free(infos); note_set(dlss_note, "No se pudo comprobar el adaptador Vulkan"); return false; }
    for (uint32_t i = 0; i < count; ++i) infos[i] = adapters[i].info;
    bool fallback;
    int selected = xcde_gpu_choose(infos, count, getenv("LARECOMP_GPU"), g_settings.gpu_uuid, g_settings.gpu_index, &fallback);
    int route = selected >= 0 ? xcde_vulkan_route(infos[selected].vulkan_routes,
        10 + (int)VK_API_VERSION_MINOR(xcde_vk_loader_version()), g_settings.vulkan_api, &fallback) : 0;
    bool nvidia = selected >= 0 && infos[selected].vendor == 0x10de;
    free(adapters);
    free(infos);
    if (!nvidia) {
        if (want_d) note_set(dlss_note, "La GPU seleccionada no es NVIDIA RTX; DLSS desactivado");
        if (want_r) note_set(reflex_note, "Reflex solo está disponible en GPUs NVIDIA");
        return false;
    }
    if (route < 13) {
        if (want_d) note_set(dlss_note, "DLSS requiere la ruta Vulkan 1.3 o 1.4");
        if (want_r) note_set(reflex_note, "Reflex requiere la ruta Vulkan 1.3 o 1.4");
        return false;
    }
    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) { note_set(dlss_note, "Ruta del ejecutable no disponible"); note_set(reflex_note, "Ruta del ejecutable no disponible"); return false; }
    while (n && dir[n - 1] != L'\\' && dir[n - 1] != L'/') n--;
    dir[n] = 0;
    wchar_t path[MAX_PATH + 32];
    swprintf(path, MAX_PATH + 32, L"%ls\\sl.interposer.dll", dir);
    if (!xcde_nvidia_signature(path)) {
        if (want_d) note_set(dlss_note, "Falta el runtime oficial o su firma NVIDIA no es válida");
        if (want_r) note_set(reflex_note, "Falta el runtime oficial o su firma NVIDIA no es válida");
        return false;
    }
    // a missing plugin only drops that feature: the DLLs are deployed separately
    if (want_d && !(file_next_to(dir, L"sl.dlss.dll") && file_next_to(dir, L"nvngx_dlss.dll"))) {
        note_set(dlss_note, "Faltan sl.dlss.dll o nvngx_dlss.dll");
        want_d = false;
    }
    if (want_r && !(file_next_to(dir, L"sl.reflex.dll") && file_next_to(dir, L"sl.pcl.dll") &&
                    file_next_to(dir, L"NvLowLatencyVk.dll"))) {
        note_set(reflex_note, "Faltan sl.reflex.dll, sl.pcl.dll o NvLowLatencyVk.dll");
        want_r = false;
    }
    if (!want_d && !want_r) return false;
    sl_mod = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!sl_mod) {
        snprintf(error, sizeof error, "No se pudo cargar Streamline (Win32 %lu). Comprueba DLL y Visual C++ x64", GetLastError());
        if (want_d) note_set(dlss_note, error);
        if (want_r) note_set(reflex_note, error);
        return false;
    }
#define GET(f) p_##f = (PFun_##f *)(void *)GetProcAddress(sl_mod, #f)
    GET(slShutdown); GET(slInit); GET(slGetFeatureRequirements); GET(slSetVulkanInfo); GET(slIsFeatureSupported); GET(slGetFeatureFunction);
    GET(slGetNewFrameToken); GET(slSetConstants); GET(slSetTagForFrame); GET(slEvaluateFeature);
#undef GET
    p_proxyDeviceProcAddr = (PFN_vkGetDeviceProcAddr)(void *)GetProcAddress(sl_mod, "vkGetDeviceProcAddr");
    p_proxyInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(sl_mod, "vkGetInstanceProcAddr");
    if (!p_slInit || !p_slShutdown || !p_slGetFeatureRequirements || !p_slSetVulkanInfo || !p_slEvaluateFeature ||
        !p_slIsFeatureSupported || !p_slGetFeatureFunction || !p_slGetNewFrameToken || !p_slSetConstants ||
        !p_slSetTagForFrame || !p_proxyDeviceProcAddr || !p_proxyInstanceProcAddr) {
        note_set(dlss_note, "El runtime Streamline no contiene todas las funciones requeridas");
        note_set(reflex_note, "El runtime Streamline no contiene todas las funciones requeridas");
        return false;
    }
    static const wchar_t *paths[1];
    paths[0] = dir;
    static sl::Feature feature_list[3];
    uint32_t nfeat = 0;
    if (want_d) feature_list[nfeat++] = sl::kFeatureDLSS;
    if (want_r) { feature_list[nfeat++] = sl::kFeatureReflex; feature_list[nfeat++] = sl::kFeaturePCL; }
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = getenv("XCDE_TMP_LOG") ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.logMessageCallback = sl_log;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = nullptr;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = feature_list;
    pref.numFeaturesToLoad = nfeat;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "xcde";
    pref.projectId = "5b7c1d8e-6a4f-4e1d-9c3a-2f8e7d6b5a41";
    pref.renderAPI = sl::RenderAPI::eVulkan;
    sl::Result r = p_slInit(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        snprintf(error, sizeof error, "Streamline no pudo iniciarse (SDK %d)", (int)r);
        if (want_d) note_set(dlss_note, error);
        if (want_r) note_set(reflex_note, error);
        return false;
    }
    sl_initialized = true;
    atexit(dlss_shutdown);
    if (want_d) req_dlss_ok = feature_requirements(sl::kFeatureDLSS, req_dlss);
    if (want_r) {
        req_reflex_ok = feature_requirements(sl::kFeatureReflex, req_reflex);
        req_pcl_ok = feature_requirements(sl::kFeaturePCL, req_pcl);
        if (!req_reflex_ok || !req_pcl_ok) {
            note_set(reflex_note, "No se pudo cargar el plugin Reflex/PCL del SDK");
            req_reflex_ok = req_pcl_ok = false;
        }
    }
    if (want_d && !req_dlss_ok) note_set(dlss_note, "No se pudo cargar el plugin DLSS (SDK)");
    sl_rt_ok = req_dlss_ok || req_reflex_ok;
    DLOG("[streamline] loaded (requirements: dlss %d, reflex %d, pcl %d; %u feature(s))\n",
         (int)req_dlss_ok, (int)req_reflex_ok, (int)req_pcl_ok, nfeat);
    return true;
}

extern "C" int dlss_instance_exts(const char **out, int max) {
    int n = 0;
    const sl::FeatureRequirements *all[3] = {&req_dlss, &req_reflex, &req_pcl};
    const bool ok[3] = {req_dlss_ok, req_reflex_ok, req_pcl_ok};
    for (int f = 0; f < 3; f++) {
        for (uint32_t i = 0; ok[f] && i < all[f]->vkNumInstanceExtensions && n < max; i++) {
            bool dup = false;
            for (int j = 0; j < n; j++) dup |= !strcmp(out[j], all[f]->vkInstanceExtensions[i]);
            if (!dup) out[n++] = all[f]->vkInstanceExtensions[i];
        }
    }
    return n;
}

// [XCDE] T250: optional feature requirements must never make an otherwise usable GPU fail.
extern "C" void dlss_validate_adapter(VkPhysicalDevice phys, uint32_t api, uint32_t qfam) {
    if (!sl_rt_ok) return;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    bool nvidia = api >= VK_API_VERSION_1_3 && props.vendorID == 0x10de;
    if (!nvidia) {
        if (req_dlss_ok) note_set(dlss_note, "La GPU elegida no es NVIDIA; DLSS desactivado");
        if (req_reflex_ok) note_set(reflex_note, "Reflex solo está disponible en GPUs NVIDIA");
        req_dlss_ok = req_reflex_ok = req_pcl_ok = false;
        dlss_ok = reflex_ok = false;
        sl_rt_ok = false;
        return;
    }
    uint32_t next = 0;
    VkResult er = vkEnumerateDeviceExtensionProperties(phys, nullptr, &next, nullptr);
    VkExtensionProperties *ext = next ? (VkExtensionProperties *)calloc(next, sizeof(VkExtensionProperties)) : nullptr;
    if (er != VK_SUCCESS || !ext || vkEnumerateDeviceExtensionProperties(phys, nullptr, &next, ext) != VK_SUCCESS) {
        free(ext);
        if (req_dlss_ok) note_set(dlss_note, "No se pudieron leer las extensiones Vulkan de la GPU");
        if (req_reflex_ok) note_set(reflex_note, "No se pudieron leer las extensiones Vulkan de la GPU");
        req_dlss_ok = req_reflex_ok = req_pcl_ok = false;
        sl_rt_ok = false;
        return;
    }
    VkPhysicalDeviceVulkan13Features h13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features h12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    h12.pNext = &h13;
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    have.pNext = &h12;
    vkGetPhysicalDeviceFeatures2(phys, &have);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
    VkQueueFamilyProperties *queues = nq ? (VkQueueFamilyProperties *)calloc(nq, sizeof(VkQueueFamilyProperties)) : nullptr;
    if (queues) vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, queues);
    uint32_t available = queues && qfam < nq ? queues[qfam].queueCount : 0;
    free(queues);
    if (req_dlss_ok && !feature_reqs_supported(req_dlss, h12, h13, ext, next)) {
        note_set(dlss_note, "GPU/controlador sin los requisitos DLSS de Vulkan; se usa escalado alternativo");
        req_dlss_ok = false;
    }
    if (req_reflex_ok && !feature_reqs_supported(req_reflex, h12, h13, ext, next)) {
        note_set(reflex_note, "GPU/controlador sin la extensión de baja latencia de Vulkan; Reflex desactivado");
        req_reflex_ok = false;
    }
    if (req_pcl_ok && !feature_reqs_supported(req_pcl, h12, h13, ext, next)) req_pcl_ok = false;
    free(ext);
    uint32_t gfx = 0, comp = 0;
    if (req_dlss_ok) { gfx += req_dlss.vkNumGraphicsQueuesRequired; comp += req_dlss.vkNumComputeQueuesRequired; }
    if (req_reflex_ok) { gfx += req_reflex.vkNumGraphicsQueuesRequired; comp += req_reflex.vkNumComputeQueuesRequired; }
    if (req_pcl_ok) { gfx += req_pcl.vkNumGraphicsQueuesRequired; comp += req_pcl.vkNumComputeQueuesRequired; }
    if (1ull + gfx + comp > available || 1ull + gfx + comp > 16) {
        if (req_dlss_ok) note_set(dlss_note, "La familia de colas elegida no tiene colas suficientes para Streamline");
        if (req_reflex_ok) note_set(reflex_note, "La familia de colas elegida no tiene colas suficientes para Streamline");
        req_dlss_ok = req_reflex_ok = req_pcl_ok = false;
    }
    if (!req_reflex_ok) req_pcl_ok = false;
    if (!req_dlss_ok && !req_reflex_ok) sl_rt_ok = false;
}

// The device's extensions (appended to ext[*n]) and features (OR-ed into f12 / f13); extra queues it asks for.
extern "C" void dlss_device_requirements(const char **ext, uint32_t *n, uint32_t max, VkPhysicalDeviceVulkan12Features *f12,
                                         VkPhysicalDeviceVulkan13Features *f13, uint32_t *extra_gfx, uint32_t *extra_compute) {
    *extra_gfx = *extra_compute = 0;
    if (!sl_rt_ok) return;
    const sl::FeatureRequirements *all[3] = {&req_dlss, &req_reflex, &req_pcl};
    const bool ok[3] = {req_dlss_ok, req_reflex_ok, req_pcl_ok};
    uint32_t additional = 0;
    for (int f = 0; f < 3; f++)
        for (uint32_t i = 0; ok[f] && i < all[f]->vkNumDeviceExtensions; ++i) {
            bool present = false;
            for (uint32_t j = 0; j < *n; ++j) present |= !strcmp(ext[j], all[f]->vkDeviceExtensions[i]);
            if (!present) ++additional;
        }
    if (*n > max || additional > max - *n) {
        note_set(dlss_note, "Demasiadas extensiones opcionales de Streamline; se usa escalado alternativo");
        note_set(reflex_note, "Demasiadas extensiones opcionales de Streamline; Reflex desactivado");
        sl_rt_ok = false;
        return;
    }
    for (int f = 0; f < 3; f++)
        for (uint32_t i = 0; ok[f] && i < all[f]->vkNumDeviceExtensions && *n < max; i++) {
            bool dup = false;
            for (uint32_t j = 0; j < *n; j++) dup |= !strcmp(ext[j], all[f]->vkDeviceExtensions[i]);
            if (!dup) ext[(*n)++] = all[f]->vkDeviceExtensions[i];
        }
    for (int f = 0; f < 3; f++) {
        if (!ok[f]) continue;
        VkPhysicalDeviceVulkan12Features a = sl::getVkPhysicalDeviceVulkan12Features(all[f]->vkNumFeatures12, all[f]->vkFeatures12);
        VkPhysicalDeviceVulkan13Features b = sl::getVkPhysicalDeviceVulkan13Features(all[f]->vkNumFeatures13, all[f]->vkFeatures13);
        VkBool32 *d = &f12->samplerMirrorClampToEdge, *s = &a.samplerMirrorClampToEdge;
        for (size_t i = 0; i < (offsetof(VkPhysicalDeviceVulkan12Features, subgroupBroadcastDynamicId) + sizeof(VkBool32) -
                                offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge)) / sizeof(VkBool32); i++) d[i] |= s[i];
        d = &f13->robustImageAccess;
        s = &b.robustImageAccess;
        for (size_t i = 0; i < (offsetof(VkPhysicalDeviceVulkan13Features, maintenance4) + sizeof(VkBool32) -
                                offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess)) / sizeof(VkBool32); i++) d[i] |= s[i];
        *extra_gfx += all[f]->vkNumGraphicsQueuesRequired;
        *extra_compute += all[f]->vkNumComputeQueuesRequired;
    }
}

// After vkCreateDevice: the device for Streamline (its queues start at index 1 of the host's family), then the
// per-feature support checks. A feature that is not supported only disables that feature.
extern "C" void dlss_device_created(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t qfam) {
    if (!sl_rt_ok) return;
    uint32_t gfx = 0, comp = 0;
    if (req_dlss_ok) { gfx += req_dlss.vkNumGraphicsQueuesRequired; comp += req_dlss.vkNumComputeQueuesRequired; }
    if (req_reflex_ok) { gfx += req_reflex.vkNumGraphicsQueuesRequired; comp += req_reflex.vkNumComputeQueuesRequired; }
    if (req_pcl_ok) { gfx += req_pcl.vkNumGraphicsQueuesRequired; comp += req_pcl.vkNumComputeQueuesRequired; }
    sl::VulkanInfo info{};
    info.device = dev;
    info.instance = inst;
    info.physicalDevice = phys;
    info.graphicsQueueFamily = qfam;
    uint32_t graphics_index, compute_index;
    xcde_dlss_queue_indices(gfx, comp, &graphics_index, &compute_index);
    info.graphicsQueueIndex = graphics_index;
    info.computeQueueFamily = qfam;
    info.computeQueueIndex = compute_index;
    sl::Result r = p_slSetVulkanInfo(info);
    if (r != sl::Result::eOk) {
        snprintf(dlss_note, sizeof dlss_note, "No se pudo preparar Streamline en Vulkan (SDK %d)", (int)r);
        note_set(reflex_note, dlss_note);
        sl_rt_ok = false;
        return;
    }
    sl::AdapterInfo ai{};
    ai.vkPhysicalDevice = phys;
    if (req_dlss_ok) {
        if (p_slIsFeatureSupported(sl::kFeatureDLSS, ai) != sl::Result::eOk) {
            note_set(dlss_note, "DLSS no compatible con esta GPU/controlador");
            req_dlss_ok = false;
        } else {
            void *f = nullptr;
            if (p_slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", f) != sl::Result::eOk || !f) {
                note_set(dlss_note, "El runtime no expone slDLSSSetOptions");
                req_dlss_ok = false;
            } else {
                p_slDLSSSetOptions = (PFun_slDLSSSetOptions *)f;
                dlss_ok = true;
                note_set(dlss_note, "DLSS Super Resolution disponible en la GPU activa");
            }
        }
    }
    if (req_reflex_ok) {
        reflex_checked = true;
        if (p_slIsFeatureSupported(sl::kFeatureReflex, ai) != sl::Result::eOk) {
            note_set(reflex_note, "Reflex no compatible con esta GPU/controlador");
            req_reflex_ok = false;
        } else {
            void *f = nullptr;
            bool all = p_slGetFeatureFunction(sl::kFeatureReflex, "slReflexGetState", f) == sl::Result::eOk && f;
            if (all) p_slReflexGetState = (PFun_slReflexGetState *)f;
            f = nullptr;
            all = all && p_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", f) == sl::Result::eOk && f;
            if (all) p_slReflexSetOptions = (PFun_slReflexSetOptions *)f;
            f = nullptr;
            all = all && p_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", f) == sl::Result::eOk && f;
            if (all) p_slReflexSleep = (PFun_slReflexSleep *)f;
            bool pcl = req_pcl_ok && p_slIsFeatureSupported(sl::kFeaturePCL, ai) == sl::Result::eOk;
            if (pcl) {
                f = nullptr;
                pcl = p_slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", f) == sl::Result::eOk && f;
                if (pcl) p_slPCLSetMarker = (PFun_slPCLSetMarker *)f;
                f = nullptr;
                pcl = pcl && p_slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetOptions", f) == sl::Result::eOk && f;
                if (pcl) p_slPCLSetOptions = (PFun_slPCLSetOptions *)f;
            } else {
                req_pcl_ok = false;
            }
            if (!all || !pcl || !p_slReflexGetState || !p_slReflexSetOptions || !p_slReflexSleep || !p_slPCLSetMarker) {
                note_set(reflex_note, "El runtime Streamline no expone las funciones de Reflex/PCL");
                req_reflex_ok = false;
            } else {
                sl::PCLOptions po{};
                p_slPCLSetOptions(po);
                reflex_apply_mode(reflex_setting());
                sl::ReflexState state{};
                sl::Result sr = p_slReflexGetState(state);
                if (sr == sl::Result::eOk && state.lowLatencyAvailable) {
                    reflex_ok = true;
                    note_set(reflex_note, "Reflex disponible (baja latencia) en la GPU activa");
                } else {
                    snprintf(reflex_note, sizeof reflex_note, "El controlador no ofrece baja latencia (SDK %d)", (int)sr);
                    DLOG("[streamline] %s\n", reflex_note);
                }
            }
        }
    }
    if (!dlss_ok && !reflex_ok) sl_rt_ok = false;
}

// T263: Streamline's surface wrapper uses the instance dispatch set up by slSetVulkanInfo. Created whenever the
// runtime is loaded with a usable feature: Reflex needs the swapchain hooks without DLSS.
extern "C" bool dlss_create_surface(void *sdl_window, VkInstance instance, VkPhysicalDevice phys,
                                    uint32_t qfam, VkSurfaceKHR *surface) {
    if (!sl_rt_ok || (!dlss_ok && !reflex_ok) || !p_proxyInstanceProcAddr) return false;
    auto create = (PFN_vkCreateWin32SurfaceKHR)p_proxyInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR");
    auto destroy = (PFN_vkDestroySurfaceKHR)p_proxyInstanceProcAddr(instance, "vkDestroySurfaceKHR");
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties((SDL_Window *)sdl_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!create || !destroy || !hwnd) {
        note_set(dlss_note, "Falta el hook o la ventana para preparar la superficie de Streamline");
        note_set(reflex_note, "Falta el hook o la ventana para preparar la superficie de Streamline");
        return false;
    }
    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    if (!info.hinstance) info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = hwnd;
    VkSurfaceKHR hooked = VK_NULL_HANDLE;
    VkResult r = create(instance, &info, nullptr, &hooked);
    if (r != VK_SUCCESS || !hooked) {
        snprintf(dlss_note, sizeof dlss_note, "Falló la superficie Streamline (Vulkan %d)", (int)r);
        note_set(reflex_note, dlss_note);
        return false;
    }
    VkBool32 present = VK_FALSE;
    r = vkGetPhysicalDeviceSurfaceSupportKHR(phys, qfam, hooked, &present);
    if (r != VK_SUCCESS || !present) {
        destroy(instance, hooked, nullptr);
        snprintf(dlss_note, sizeof dlss_note, "La cola no puede presentar la superficie Streamline (Vulkan %d)", (int)r);
        note_set(reflex_note, dlss_note);
        return false;
    }
    *surface = hooked;
    sl_surface_ready = true;
    if (dlss_ok) note_set(dlss_note, "DLSS Super Resolution disponible en la GPU activa");
    if (reflex_ok) note_set(reflex_note, "Reflex disponible (baja latencia) en la GPU activa");
    return true;
}

// Streamline's proxies for the swapchain calls it intercepts (NULL: call Vulkan directly).
extern "C" PFN_vkVoidFunction dlss_proxy(VkDevice dev, const char *name) {
    if (!sl_surface_ready || !p_proxyDeviceProcAddr) return nullptr;
    return p_proxyDeviceProcAddr(dev, name);
}

extern "C" bool dlss_available(void) { return dlss_ok && sl_surface_ready; }

// ---- Reflex (T272): all functions are no-ops unless the feature is loaded, supported and hooked
extern "C" bool reflex_available(void) { return reflex_ready(); }
extern "C" bool reflex_loaded(void) { return req_reflex_ok; }
extern "C" int reflex_mode(void) { return reflex_ok ? g_reflex_mode : REFLEX_OFF; }
extern "C" void reflex_set_mode(int mode) {
    if (!reflex_ok) {
        if (mode != REFLEX_OFF && !reflex_checked)
            note_set(reflex_note, "Reflex se carga al reiniciar (xcde.ini reflex = on|boost)");
        return;
    }
    g_reflex_pending.store(mode);  // the limiter thread applies it before the next sleep (same thread as the markers)
}
extern "C" uint64_t reflex_frame_begin(void) {
    if (!reflex_ready()) return 0;
    int pending = g_reflex_pending.exchange(-1);
    if (pending >= 0) reflex_apply_mode(pending);
    uint64_t frame = g_game_frame.fetch_add(1) + 1;
    sl::FrameToken *t = token_for(frame);
    if (!t) return 0;
    sl::Result sr = sl::Result::eOk;
    if (reflex_sleep_enabled()) sr = p_slReflexSleep(*t);  // the driver decides when the next frame may start
    if (sr != sl::Result::eOk) {
        static int shown;
        if (shown++ < 10) DLOG("[streamline] slReflexSleep failed: %d\n", (int)sr);
    }
    static bool announced;
    if (!announced) {
        announced = true;
        DLOG("[reflex] frame loop active (frame %llu)\n", (unsigned long long)frame);
    }
    // eSimulationStart is marked when the limiter actually returns (framepace.c xcde_limiter_done): this call only
    // sleeps and allocates the frame, which happens while the game is still waiting for the next frame to begin.
    return frame;
}
extern "C" uint64_t reflex_current_frame(void) { return reflex_ready() ? g_game_frame.load() : 0; }
extern "C" void reflex_marker(int marker) {
    uint64_t frame = g_game_frame.load();
    if (frame) marker_for(marker, frame);
}
extern "C" void reflex_set_present_frame(uint64_t frame) { g_present_frame.store(frame); }
extern "C" void reflex_clear_present_frame(void) { g_present_frame.store(0); }
extern "C" uint64_t reflex_present_frame_value(void) { return g_present_frame.load(); }
extern "C" void reflex_present_marker(int marker) {
    uint64_t frame = g_present_frame.load();
    if (!frame) return;
    marker_for(marker, frame);
    if (marker == REFLEX_PRESENT_END) g_present_frame.store(0);  // repeat presents: no second set of markers
}

static sl::Resource tex(const TexBackend *b, VkImageUsageFlags usage) {
    static const VkComponentMapping id = {};
    VkImageView v = vk_image_view((TexBackend *)b, VK_IMAGE_VIEW_TYPE_2D, b->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    sl::Resource r(sl::ResourceType::eTex2d, (void *)b->image, nullptr, (void *)v, (uint32_t)VK_IMAGE_LAYOUT_GENERAL);
    r.width = b->width;
    r.height = b->height;
    r.nativeFormat = (uint32_t)b->format;
    r.mipLevels = 1;
    r.arrayLayers = 1;
    r.flags = 0;
    r.usage = usage;
    return r;
}

static void rows_to_sl(const double m[16], sl::float4x4 &o) {  // column-vector matrix -> Streamline's row-vector one
    for (int r = 0; r < 4; r++) o.row[r] = sl::float4((float)m[0 * 4 + r], (float)m[1 * 4 + r], (float)m[2 * 4 + r], (float)m[3 * 4 + r]);
}
static bool inv4(const double m[16], double o[16]) {
    double a[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return false;
        if (p != c) for (int j = 0; j < 8; j++) { double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        double d = a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int r = 0; r < 4; r++) if (r != c) { double f = a[r][c]; for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) o[i * 4 + j] = a[i][j + 4];
    return true;
}

// reproj: the previous clip position from this frame's (column vectors), as temporal.c computed it.
extern "C" bool dlss_evaluate(VkCommandBuffer cb, const TexBackend *color, const TexBackend *mv, const TexBackend *depth,
                              const TexBackend *out, const TexBackend *hud, const float jitter[2], bool reset, const TmpCamera *cam,
                              const double reproj[16]) {
    if (!dlss_ok) return false;
    // T272: with Reflex loaded, the frame token is the one the frame limiter obtained (shared with the PCL markers).
    // Without Reflex this keeps the old one-token-per-evaluation behaviour (the exact DLSS path from before).
    sl::FrameToken *token = nullptr;
    uint64_t present = reflex_ready() ? g_present_frame.load() : 0;
    if (present) token = token_for(present);
    if (!token) {
        uint32_t fi = fallback_frame++;
        if (p_slGetNewFrameToken(token, &fi) != sl::Result::eOk || !token) return false;
    }
    sl::ViewportHandle vp(0);
    sl::Constants c{};
    double proj[16], iproj[16];
    for (int i = 0; i < 16; i++) proj[i] = cam->proj[i];
    if (!inv4(proj, iproj)) return false;
    rows_to_sl(proj, c.cameraViewToClip);
    rows_to_sl(iproj, c.clipToCameraView);
    double idm[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, back[16];
    rows_to_sl(idm, c.clipToLensClip);
    rows_to_sl(reproj, c.clipToPrevClip);
    if (!inv4(reproj, back)) memcpy(back, idm, sizeof back);
    rows_to_sl(back, c.prevClipToClip);
    c.jitterOffset = sl::float2(jitter[0], jitter[1]);
    c.mvecScale = sl::float2(1.0f, 1.0f);  // (motion vectors in UV units)
    c.cameraPinholeOffset = sl::float2(0, 0);
    c.cameraPos = sl::float3(0, 0, 0);
    c.cameraUp = sl::float3(cam->view[4], cam->view[5], cam->view[6]);
    c.cameraRight = sl::float3(cam->view[0], cam->view[1], cam->view[2]);
    c.cameraFwd = sl::float3(-cam->view[8], -cam->view[9], -cam->view[10]);
    c.cameraNear = fmaxf(fabsf(cam->proj[11]) * 0.5f, 0.001f);
    c.cameraFar = 100000.0f;
    c.cameraFOV = cam->proj[5] != 0 ? 2.0f * atanf(1.0f / fabsf(cam->proj[5])) : 0.785f;
    c.cameraAspectRatio = cam->proj[0] != 0 ? fabsf(cam->proj[5] / cam->proj[0]) : 1.778f;
    c.motionVectorsInvalidValue = sl::INVALID_FLOAT; // zero is a valid stationary motion vector
    c.motionVectorsJittered = sl::Boolean::eFalse;
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    if (p_slSetConstants(c, *token, vp) != sl::Result::eOk) return false;
    sl::DLSSOptions o{};
    static const sl::DLSSMode modes[] = {sl::DLSSMode::eDLAA, sl::DLSSMode::eMaxQuality, sl::DLSSMode::eBalanced,
                                         sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eUltraPerformance};
    int q = g_settings.upscale_quality;
    o.mode = modes[q >= 0 && q <= 4 ? q : 1];
    o.outputWidth = out->width;
    o.outputHeight = out->height;
    o.useAutoExposure = sl::Boolean::eTrue;
    o.colorBuffersHDR = sl::Boolean::eFalse;  // (the game image: gamma-encoded LDR)
    if (p_slDLSSSetOptions(vp, o) != sl::Result::eOk) return false;
    sl::Resource rc = tex(color, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    sl::Resource rd = tex(depth, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    sl::Resource rm = tex(mv, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    sl::Resource ro = tex(out, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    sl::Extent ein{0, 0, color->width, color->height}, eout{0, 0, out->width, out->height};
    sl::ResourceTag tags[5] = {sl::ResourceTag(&rc, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &ein),
                               sl::ResourceTag(&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &ein),
                               sl::ResourceTag(&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &ein),
                               sl::ResourceTag(&ro, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &eout),
                               sl::ResourceTag()};
    uint32_t ntags = 4;
    sl::Resource rh;
    if (hud) {  // [XCDE] T213: the HUD's pixels (temporal.c tmp_hud_snapshot): DLSS takes the current colour there (no trails)
        rh = tex(hud, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        tags[ntags++] = sl::ResourceTag(&rh, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eOnlyValidNow, &ein);
    }
    if (p_slSetTagForFrame(*token, vp, tags, ntags, (sl::CommandBuffer *)cb) != sl::Result::eOk) { vk_external_state_changed(); return false; }
    const sl::BaseStructure *inputs[] = {&vp};
    sl::Result r = p_slEvaluateFeature(sl::kFeatureDLSS, *token, inputs, 1, (sl::CommandBuffer *)cb);
    vk_external_state_changed(); // SDK may change pipeline, descriptors and dynamic state
    if (r != sl::Result::eOk) { snprintf(dlss_note, sizeof dlss_note, "DLSS dispatch falló (SDK %d); frame con escalado alternativo", (int)r); static int n; if (n++ < 5) DLOG("[streamline] %s\n", dlss_note); return false; }
    return true;
}
#else
// [XCDE] T-LINUX: NVIDIA ships Streamline/DLSS only as signed Windows DLLs. This build reports DLSS and Reflex as
// unavailable (temporal.c / the frame loop then do without them) and adds no extensions, queues or features.
#include <stdio.h>
#include <vulkan/vulkan.h>
extern "C" {
#include "temporal.h"
#include "reflex.h"
#include "../../settings.h"
const char *dlss_status(void) { return "DLSS is only available on Windows"; }
const char *reflex_status(void) { return "Reflex is only available on Windows"; }
bool dlss_init_early(void) { return false; }
int dlss_instance_exts(const char **out, int max) { (void)out; (void)max; return 0; }
void dlss_validate_adapter(VkPhysicalDevice phys, uint32_t api, uint32_t qfam) { (void)phys; (void)api; (void)qfam; }
void dlss_device_requirements(const char **ext, uint32_t *n, uint32_t max, VkPhysicalDeviceVulkan12Features *f12,
                              VkPhysicalDeviceVulkan13Features *f13, uint32_t *extra_gfx, uint32_t *extra_compute) {
    (void)ext; (void)n; (void)max; (void)f12; (void)f13;
    *extra_gfx = *extra_compute = 0;
}
void dlss_device_created(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t qfam) { (void)inst; (void)phys; (void)dev; (void)qfam; }
bool dlss_create_surface(void *sdl_window, VkInstance instance, VkPhysicalDevice phys, uint32_t qfam, VkSurfaceKHR *surface) {
    (void)sdl_window; (void)instance; (void)phys; (void)qfam; (void)surface;
    return false;
}
PFN_vkVoidFunction dlss_proxy(VkDevice dev, const char *name) { (void)dev; (void)name; return NULL; }
bool dlss_available(void) { return false; }
bool dlss_evaluate(VkCommandBuffer cb, const TexBackend *color, const TexBackend *mv, const TexBackend *depth,
                   const TexBackend *out, const TexBackend *hud, const float jitter[2], bool reset, const TmpCamera *cam,
                   const double reproj[16]) {
    (void)cb; (void)color; (void)mv; (void)depth; (void)out; (void)hud; (void)jitter; (void)reset; (void)cam; (void)reproj;
    return false;
}
bool reflex_available(void) { return false; }
bool reflex_loaded(void) { return false; }
void reflex_set_mode(int mode) { (void)mode; }
int reflex_mode(void) { return REFLEX_OFF; }
uint64_t reflex_frame_begin(void) { return 0; }
uint64_t reflex_current_frame(void) { return 0; }
void reflex_marker(int marker) { (void)marker; }
void reflex_set_present_frame(uint64_t frame) { (void)frame; }
void reflex_clear_present_frame(void) {}
void reflex_present_marker(int marker) { (void)marker; }
uint64_t reflex_present_frame_value(void) { return 0; }
}
#endif  // _WIN32
