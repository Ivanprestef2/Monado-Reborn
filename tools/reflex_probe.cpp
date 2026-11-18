// [XCDE] Exploration probe for NVIDIA Reflex (T272 research, docs/REFLEX_FEASIBILITY.md): not part of the
// product and not built by CMake or CI. Windows + NVIDIA driver + the SDK DLLs in sdk/sl/bin/x64 only.
//
// Loads the official Streamline runtime (manual hooking mode) with the Reflex and PC-Latency features, creates a
// minimal Vulkan device and a hidden-window swapchain through Streamline's proxies, and reports the feature
// requirements, slIsFeatureSupported(kFeatureReflex), slReflexGetState (lowLatencyAvailable), 240 frames of
// PCL markers + slReflexSleep, and the final Reflex latency report. PROBE_DLSS=1 also requests DLSS (to check
// that an unsupported DLSS does not take Reflex down with it). Run it from the repository root:
//
//   .venv\Lib\site-packages\ziglang\zig.exe c++ -target x86_64-windows-gnu -mcpu=x86_64_v2 -std=c++20
//       -I third_party/streamline/include -idirafter C:/msys64/ucrt64/include
//       -o build/reflex_probe.exe tools/reflex_probe.cpp C:/msys64/ucrt64/lib/libvulkan-1.dll.a -luser32 -lgdi32
//   build\reflex_probe.exe
//
// It opens no visible window and changes no files.
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <chrono>
#include <vulkan/vulkan.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <sl_helpers_vk.h>

#define PLOG(...) fprintf(stdout, __VA_ARGS__)

static HMODULE sl_mod;
static PFN_vkGetInstanceProcAddr p_proxyInstanceProcAddr;
static PFN_vkGetDeviceProcAddr p_proxyDeviceProcAddr;
static PFun_slInit *p_slInit;
static PFun_slShutdown *p_slShutdown;
static PFun_slGetFeatureRequirements *p_slGetFeatureRequirements;
static PFun_slSetVulkanInfo *p_slSetVulkanInfo;
static PFun_slIsFeatureSupported *p_slIsFeatureSupported;
static PFun_slGetFeatureFunction *p_slGetFeatureFunction;
static PFun_slGetNewFrameToken *p_slGetNewFrameToken;
static PFun_slReflexGetState *p_slReflexGetState;
static PFun_slReflexSetOptions *p_slReflexSetOptions;
static PFun_slReflexSleep *p_slReflexSleep;
static PFun_slPCLSetMarker *p_slPCLSetMarker;
static PFun_slPCLSetOptions *p_slPCLSetOptions;

static void sl_log(sl::LogType type, const char *msg) {
    if (type == sl::LogType::eInfo) return;  // (warnings and errors only)
    const char *tag = type == sl::LogType::eWarn ? "warn" : "ERROR";
    PLOG("[sl %s] %s", tag, msg);
    fflush(stdout);
}

static bool load_streamline(void) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetFullPathNameW(L"sdk\\sl\\bin\\x64", MAX_PATH, dir, NULL);
    if (!n || n >= MAX_PATH) { PLOG("cannot resolve sdk dir\n"); return false; }
    wchar_t path[MAX_PATH + 32];
    swprintf(path, MAX_PATH + 32, L"%ls\\sl.interposer.dll", dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) { PLOG("missing %ls\n", path); return false; }
    sl_mod = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!sl_mod) { PLOG("LoadLibraryEx failed %lu\n", GetLastError()); return false; }
#define GET(f) p_##f = (PFun_##f *)(void *)GetProcAddress(sl_mod, #f)
    GET(slInit); GET(slShutdown); GET(slGetFeatureRequirements); GET(slSetVulkanInfo); GET(slIsFeatureSupported);
    GET(slGetFeatureFunction); GET(slGetNewFrameToken);
#undef GET
    p_proxyDeviceProcAddr = (PFN_vkGetDeviceProcAddr)(void *)GetProcAddress(sl_mod, "vkGetDeviceProcAddr");
    p_proxyInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(sl_mod, "vkGetInstanceProcAddr");
    if (!p_slInit || !p_slShutdown || !p_slGetFeatureRequirements || !p_slSetVulkanInfo || !p_slIsFeatureSupported ||
        !p_slGetFeatureFunction || !p_slGetNewFrameToken || !p_proxyDeviceProcAddr || !p_proxyInstanceProcAddr) {
        PLOG("interposer is missing functions\n");
        return false;
    }
    static const wchar_t *paths[1];
    paths[0] = dir;
    static sl::Feature features[] = {sl::kFeatureReflex, sl::kFeaturePCL, sl::kFeatureDLSS};
    uint32_t nfeatures = getenv("PROBE_DLSS") ? 3u : 2u;  // (PROBE_DLSS=1: also request DLSS, as the game does on RTX)
    sl::Preferences pref{};
    pref.logLevel = sl::LogLevel::eDefault;
    pref.logMessageCallback = sl_log;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = nfeatures;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "xcde-reflex-probe";
    pref.projectId = "5b7c1d8e-6a4f-4e1d-9c3a-2f8e7d6b5a41";
    pref.renderAPI = sl::RenderAPI::eVulkan;
    sl::Result r = p_slInit(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) { PLOG("slInit failed: %d\n", (int)r); return false; }
    PLOG("slInit ok\n");
    return true;
}

struct FeatureReq {
    sl::FeatureRequirements reqs;
    bool ok = false;
    const char *name = "";
};

static void print_reqs(const FeatureReq &f) {
    if (!f.ok) { PLOG("  %s: requirements unavailable\n", f.name); return; }
    const sl::FeatureRequirements &q = f.reqs;
    PLOG("  %s requirements: %u instance ext, %u device ext, %u gfx queues, %u compute queues, %u feats12, %u feats13\n",
         f.name, q.vkNumInstanceExtensions, q.vkNumDeviceExtensions, q.vkNumGraphicsQueuesRequired,
         q.vkNumComputeQueuesRequired, q.vkNumFeatures12, q.vkNumFeatures13);
    for (uint32_t i = 0; i < q.vkNumInstanceExtensions; i++) PLOG("    inst ext: %s\n", q.vkInstanceExtensions[i]);
    for (uint32_t i = 0; i < q.vkNumDeviceExtensions; i++) PLOG("    dev ext : %s\n", q.vkDeviceExtensions[i]);
    for (uint32_t i = 0; i < q.vkNumFeatures12; i++) PLOG("    feat12  : %s\n", q.vkFeatures12[i]);
    for (uint32_t i = 0; i < q.vkNumFeatures13; i++) PLOG("    feat13  : %s\n", q.vkFeatures13[i]);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static HWND make_window(void) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"xcdeReflexProbe";
    RegisterClassW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"reflex probe", WS_POPUP, 0, 0, 640, 360, NULL, NULL, wc.hInstance, NULL);
}

static double now_ms(void) {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double, std::milli>(steady_clock::now() - t0).count();
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);  // (crash triage: keep every line)
    PLOG("=== Reflex probe on this machine ===\n");
    double t_init = now_ms();
    if (!load_streamline()) return 1;
    PLOG("Streamline load + slInit took %.0f ms\n", now_ms() - t_init);

    uint32_t nfeat = getenv("PROBE_DLSS") ? 3u : 2u;
    FeatureReq fr[3];
    fr[0].name = "reflex"; fr[1].name = "pcl"; fr[2].name = "dlss";
    static sl::Feature feats[3] = {sl::kFeatureReflex, sl::kFeaturePCL, sl::kFeatureDLSS};
    for (uint32_t i = 0; i < nfeat; i++)
        fr[i].ok = p_slGetFeatureRequirements(feats[i], fr[i].reqs) == sl::Result::eOk;
    for (uint32_t i = 0; i < nfeat; i++) print_reqs(fr[i]);

    HWND hwnd = make_window();
    if (!hwnd) { PLOG("no window\n"); return 1; }

    // ---- instance: surface + everything Streamline asked for
    const char *iext[16];
    uint32_t nie = 0;
    iext[nie++] = VK_KHR_SURFACE_EXTENSION_NAME;
    iext[nie++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    for (uint32_t i = 0; i < nfeat; i++)
        for (uint32_t k = 0; fr[i].ok && k < fr[i].reqs.vkNumInstanceExtensions && nie < 16; k++) {
            bool dup = false;
            for (uint32_t j = 0; j < nie; j++) dup |= !strcmp(iext[j], fr[i].reqs.vkInstanceExtensions[k]);
            if (!dup) iext[nie++] = fr[i].reqs.vkInstanceExtensions[k];
        }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "xcde-reflex-probe",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = nie, .ppEnabledExtensionNames = iext};
    VkInstance inst = VK_NULL_HANDLE;
    VkResult vr = vkCreateInstance(&ici, NULL, &inst);
    if (vr != VK_SUCCESS) { PLOG("vkCreateInstance %d\n", (int)vr); return 1; }

    uint32_t npd = 0;
    vkEnumeratePhysicalDevices(inst, &npd, NULL);
    VkPhysicalDevice pd[4] = {0};
    if (npd > 4) npd = 4;
    vkEnumeratePhysicalDevices(inst, &npd, pd);
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < npd; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        if (p.vendorID == 0x10de) phys = pd[i];
        PLOG("adapter %u: %s (vendor 0x%04x)\n", i, p.deviceName, p.vendorID);
    }
    if (!phys) phys = pd[0];

    // ---- a plain surface now (queue-family probe only).  The surface Streamline tracks is created through its
    // proxy AFTER slSetVulkanInfo, as the game does (device.c ui_window_init: surface -> device -> proxy surface).
    PLOG("step: surface\n"); fflush(stdout);
    VkWin32SurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                       .hinstance = GetModuleHandleW(NULL), .hwnd = hwnd};
    VkSurfaceKHR probe_surf = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(inst, &sci, NULL, &probe_surf) != VK_SUCCESS) { PLOG("probe surface failed\n"); return 1; }
    PLOG("probe surface ok\n");

    PLOG("step: queues\n"); fflush(stdout);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties q[8];
    if (nq > 8) nq = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, q);
    uint32_t qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, probe_surf, &present);
        if (present && (q[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                           (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) && qfam == UINT32_MAX)
            qfam = i;
    }
    if (qfam == UINT32_MAX) { PLOG("no graphics+compute+present family\n"); return 1; }

    // ---- device extensions/features from both features
    const char *dext[32];
    uint32_t nd = 0;
    dext[nd++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    for (uint32_t i = 0; i < nfeat; i++)
        for (uint32_t k = 0; fr[i].ok && k < fr[i].reqs.vkNumDeviceExtensions && nd < 32; k++) {
            bool dup = false;
            for (uint32_t j = 0; j < nd; j++) dup |= !strcmp(dext[j], fr[i].reqs.vkDeviceExtensions[k]);
            if (!dup) dext[nd++] = fr[i].reqs.vkDeviceExtensions[k];
        }
    uint32_t extra_gfx = 0, extra_comp = 0;
    for (uint32_t i = 0; i < nfeat; i++) if (fr[i].ok) { extra_gfx += fr[i].reqs.vkNumGraphicsQueuesRequired; extra_comp += fr[i].reqs.vkNumComputeQueuesRequired; }
    PLOG("device: %u extensions, %u extra gfx queues, %u extra compute queues\n", nd, extra_gfx, extra_comp);

    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    for (uint32_t i = 0; i < nfeat; i++) {
        if (!fr[i].ok) continue;
        VkPhysicalDeviceVulkan12Features a = sl::getVkPhysicalDeviceVulkan12Features(fr[i].reqs.vkNumFeatures12, fr[i].reqs.vkFeatures12);
        VkPhysicalDeviceVulkan13Features b = sl::getVkPhysicalDeviceVulkan13Features(fr[i].reqs.vkNumFeatures13, fr[i].reqs.vkFeatures13);
        VkBool32 *d = &f12.samplerMirrorClampToEdge, *s = &a.samplerMirrorClampToEdge;
        for (size_t k = 0; k < (offsetof(VkPhysicalDeviceVulkan12Features, subgroupBroadcastDynamicId) + sizeof(VkBool32) -
                                offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge)) / sizeof(VkBool32); k++) d[k] |= s[k];
        d = &f13.robustImageAccess; s = &b.robustImageAccess;
        for (size_t k = 0; k < (offsetof(VkPhysicalDeviceVulkan13Features, maintenance4) + sizeof(VkBool32) -
                                offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess)) / sizeof(VkBool32); k++) d[k] |= s[k];
    }
    uint32_t qcount = 1 + extra_gfx + extra_comp;
    float prio[16];
    for (uint32_t i = 0; i < qcount && i < 16; i++) prio[i] = 1.0f;
    VkDeviceQueueCreateInfo dq = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam,
                                  .queueCount = qcount, .pQueuePriorities = prio};
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &dq, .enabledExtensionCount = nd, .ppEnabledExtensionNames = dext};
    PLOG("step: create device (%u queues, %u exts)\n", qcount, nd); fflush(stdout);
    VkDevice dev = VK_NULL_HANDLE;
    vr = vkCreateDevice(phys, &dci, NULL, &dev);
    if (vr != VK_SUCCESS) { PLOG("vkCreateDevice %d\n", (int)vr); return 1; }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, qfam, 0, &queue);

    PLOG("step: slSetVulkanInfo\n"); fflush(stdout);
    sl::VulkanInfo vi{};
    vi.device = dev;
    vi.instance = inst;
    vi.physicalDevice = phys;
    vi.graphicsQueueFamily = qfam;
    vi.computeQueueFamily = qfam;
    vi.graphicsQueueIndex = extra_gfx ? 1 : 0;
    vi.computeQueueIndex = extra_comp ? 1 + extra_gfx : 0;
    sl::Result r = p_slSetVulkanInfo(vi);
    PLOG("slSetVulkanInfo: %d\n", (int)r);
    if (r != sl::Result::eOk) return 1;

    PLOG("step: slIsFeatureSupported\n"); fflush(stdout);
    sl::AdapterInfo ai{};
    ai.vkPhysicalDevice = phys;
    PLOG("slIsFeatureSupported(kFeatureReflex): %d\n", (int)p_slIsFeatureSupported(sl::kFeatureReflex, ai));
    PLOG("slIsFeatureSupported(kFeaturePCL): %d\n", (int)p_slIsFeatureSupported(sl::kFeaturePCL, ai));

    void *f = nullptr;
#define FEATFN(feature, name, var)                                                                 \
    f = nullptr;                                                                                  \
    if (p_slGetFeatureFunction(feature, #name, f) == sl::Result::eOk && f) var = (PFun_##name *)f; \
    PLOG("  %s: %s\n", #name, var ? "ok" : "MISSING")
    FEATFN(sl::kFeatureReflex, slReflexGetState, p_slReflexGetState);
    FEATFN(sl::kFeatureReflex, slReflexSetOptions, p_slReflexSetOptions);
    FEATFN(sl::kFeatureReflex, slReflexSleep, p_slReflexSleep);
    FEATFN(sl::kFeaturePCL, slPCLSetMarker, p_slPCLSetMarker);
    FEATFN(sl::kFeaturePCL, slPCLSetOptions, p_slPCLSetOptions);
#undef FEATFN
    if (!p_slReflexGetState || !p_slReflexSetOptions || !p_slReflexSleep || !p_slPCLSetMarker) return 1;

    sl::PCLOptions po{};
    if (p_slPCLSetOptions) PLOG("slPCLSetOptions: %d\n", (int)p_slPCLSetOptions(po));

    PLOG("step: slReflexGetState\n"); fflush(stdout);
    sl::ReflexState st{};
    r = p_slReflexGetState(st);
    PLOG("slReflexGetState: %d  lowLatencyAvailable=%d latencyReportAvailable=%d flashDriver=%d\n", (int)r,
         (int)st.lowLatencyAvailable, (int)st.latencyReportAvailable, (int)st.flashIndicatorDriverControlled);

    sl::ReflexOptions ro{};
    ro.mode = sl::ReflexMode::eLowLatency;
    r = p_slReflexSetOptions(ro);
    PLOG("slReflexSetOptions(eLowLatency): %d\n", (int)r);

    PLOG("step: swapchain\n"); fflush(stdout);
    // ---- the surface Streamline tracks, through its proxy (this is what the game does after slSetVulkanInfo)
    auto createSurf = (PFN_vkCreateWin32SurfaceKHR)p_proxyInstanceProcAddr(inst, "vkCreateWin32SurfaceKHR");
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    if (!createSurf || createSurf(inst, &sci, NULL, &surf) != VK_SUCCESS) { PLOG("proxy surface failed\n"); return 1; }
    vkDestroySurfaceKHR(inst, probe_surf, NULL);
    PLOG("proxy surface ok\n");
    // ---- swapchain through Streamline proxies
    auto sc_create = (PFN_vkCreateSwapchainKHR)p_proxyDeviceProcAddr(dev, "vkCreateSwapchainKHR");
    auto sc_images = (PFN_vkGetSwapchainImagesKHR)p_proxyDeviceProcAddr(dev, "vkGetSwapchainImagesKHR");
    auto sc_acquire = (PFN_vkAcquireNextImageKHR)p_proxyDeviceProcAddr(dev, "vkAcquireNextImageKHR");
    auto sc_present = (PFN_vkQueuePresentKHR)p_proxyDeviceProcAddr(dev, "vkQueuePresentKHR");
    PLOG("proxies: create=%d images=%d acquire=%d present=%d\n", !!sc_create, !!sc_images, !!sc_acquire, !!sc_present);
    if (!sc_create || !sc_images || !sc_acquire || !sc_present) return 1;

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps);
    VkExtent2D ext = {640, 360};
    if (caps.currentExtent.width != UINT32_MAX) ext = caps.currentExtent;
    VkSwapchainCreateInfoKHR scci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf,
                                     .minImageCount = caps.minImageCount + 1, .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
                                     .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = ext,
                                     .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
                                     .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                     .clipped = VK_TRUE};
    if (caps.maxImageCount && scci.minImageCount > caps.maxImageCount) scci.minImageCount = caps.maxImageCount;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    vr = sc_create(dev, &scci, NULL, &sc);
    PLOG("swapchain: %d (%ux%u, %u images)\n", (int)vr, ext.width, ext.height, scci.minImageCount);
    if (vr != VK_SUCCESS) return 1;
    uint32_t nimg = 0;
    sc_images(dev, sc, &nimg, NULL);
    VkImage img[8] = {0};
    if (nimg > 8) nimg = 8;
    sc_images(dev, sc, &nimg, img);
    VkImageView view[8] = {0};
    for (uint32_t i = 0; i < nimg; i++) {
        VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img[i],
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCreateImageView(dev, &vci, NULL, &view[i]);
    }

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam};
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &cbai, &cb);
    VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acq = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
    vkCreateSemaphore(dev, &semi, NULL, &acq);
    vkCreateSemaphore(dev, &semi, NULL, &done);
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(dev, &fci, NULL, &fence);

    const int FRAMES = 240;
    double sleep_sum = 0, sleep_max = 0, frame_sum = 0;
    double last_frame = now_ms();
    uint32_t token_index = 0;
    int sleep_errors = 0, marker_errors = 0;
    for (int i = 0; i < FRAMES; i++) {
        sl::FrameToken *token = nullptr;
        if (p_slGetNewFrameToken(token, &token_index) != sl::Result::eOk || !token) { PLOG("no frame token\n"); break; }
        token_index++;
        double a = now_ms();
        sl::Result sr = p_slReflexSleep(*token);
        double b = now_ms();
        if (sr != sl::Result::eOk) sleep_errors++;
        sleep_sum += b - a;
        if (b - a > sleep_max) sleep_max = b - a;
        #define MARK(m) if (p_slPCLSetMarker(sl::PCLMarker::m, *token) != sl::Result::eOk) marker_errors++
        MARK(eSimulationStart);
        MARK(eSimulationEnd);
        MARK(eRenderSubmitStart);

        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fence);
        uint32_t index = 0;
        if (sc_acquire(dev, sc, UINT64_MAX, acq, VK_NULL_HANDLE, &index) != VK_SUCCESS) { PLOG("acquire failed\n"); break; }
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier b1 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
                                   .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = img[index], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
        VkClearColorValue col = {{0.05f, 0.08f, 0.12f, 1.0f}};
        VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, img[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &rng);
        VkImageMemoryBarrier b2 = b1;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b2.dstAccessMask = 0;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
        vkEndCommandBuffer(cb);
        MARK(eRenderSubmitEnd);
        MARK(ePresentStart);
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acq,
                           .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1, .pCommandBuffers = &cb,
                           .signalSemaphoreCount = 1, .pSignalSemaphores = &done};
        vkQueueSubmit(queue, 1, &si, fence);
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                               .pWaitSemaphores = &done, .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &index};
        sc_present(queue, &pi);
        MARK(ePresentEnd);
        double e = now_ms();
        if (i > 10) { frame_sum += e - last_frame; }
        last_frame = e;
    }
    PLOG("frames %d: sleep avg %.3f ms, max %.3f ms (errors %d); frame avg %.3f ms; marker errors %d\n", FRAMES,
         sleep_sum / FRAMES, sleep_max, sleep_errors, frame_sum / (FRAMES - 11 > 0 ? FRAMES - 11 : 1), marker_errors);

    r = p_slReflexGetState(st);
    PLOG("after frames: slReflexGetState %d lowLatency=%d reportAvailable=%d\n", (int)r, (int)st.lowLatencyAvailable,
         (int)st.latencyReportAvailable);
    int shown = 0;
    for (int i = sl::kReflexFrameReportCount - 1; i >= 0 && shown < 4; i--) {
        const sl::ReflexReport &frp = st.frameReport[i];
        if (!frp.frameID || !frp.simStartTime) continue;
        PLOG("report frame %llu: simStart %llu simEnd %llu submit %llu..%llu present %llu..%llu gpu %llu..%llu "
             "gpuActive %u us gpuFrame %u us\n",
             (unsigned long long)frp.frameID, (unsigned long long)frp.simStartTime, (unsigned long long)frp.simEndTime,
             (unsigned long long)frp.renderSubmitStartTime, (unsigned long long)frp.renderSubmitEndTime,
             (unsigned long long)frp.presentStartTime, (unsigned long long)frp.presentEndTime,
             (unsigned long long)frp.gpuRenderStartTime, (unsigned long long)frp.gpuRenderEndTime,
             frp.gpuActiveRenderTimeUs, frp.gpuFrameTimeUs);
        shown++;
    }
    if (!shown) PLOG("no frame report data\n");

    vkDeviceWaitIdle(dev);
    p_slShutdown();
    vkDestroyFence(dev, fence, NULL);
    vkDestroySemaphore(dev, acq, NULL);
    vkDestroySemaphore(dev, done, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    for (uint32_t i = 0; i < nimg; i++) vkDestroyImageView(dev, view[i], NULL);
    vkDestroySwapchainKHR(dev, sc, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroySurfaceKHR(inst, surf, NULL);
    vkDestroyInstance(inst, NULL);
    FreeLibrary(sl_mod);
    return 0;
}
