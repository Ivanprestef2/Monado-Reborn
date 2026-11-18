// VRAM headroom: the next VMA block of a memory type is allocated on a background thread, not on the render thread.
//
// VMA places an allocation in an existing block of its memory type and allocates a new block (vkAllocateMemory, 256 MiB
// on heaps over 1 GiB, an eighth of the heap below) only when none has room. On the render thread that took 8-19 ms per
// block, several times in the first minute of play while VRAM grows (textures, buffers, the transient ring). A new
// VkDeviceMemory made by any other thread (VMA's pfnAllocate callback) wakes this thread for its memory type, which it
// then checks every 500 ms. A check allocates half a default block of that type (the largest size VMA still places in
// blocks, not in dedicated memory). When a block has that much room the allocation lands there and is freed at once.
// When none has, VMA allocates a new block here, and the allocation stays as that block's anchor: VMA deletes an empty
// block at the next free of its type (VmaBlockVector::Free, "we have another empty block"), so a spare block left empty
// lasted until the next texture went. The previous anchor is freed then, which gives its half block to the render
// thread, so at most half a block per memory type is held back.
// Under VRAM pressure no headroom is kept: the spare block counts against the budget that decides whether another block
// is made, so while use plus two blocks passes 85% of the budget the anchor goes and nothing new is allocated.
// XCDE_VRAM_HEADROOM=0 turns it off.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "vk.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#include "../../platform.h"

#define HR_PERIOD_MS 500
#define HR_SMALL_HEAP (1ull << 30)   // VMA_SMALL_HEAP_MAX_SIZE: smaller heaps use blocks of a heap eighth

static struct {
    _Atomic int on, stop, started, inited;
    pthread_mutex_t lock;             // held around each top-up; vk_headroom_stop takes it before the allocator goes
    pthread_t thread;                 // joined by vk_headroom_stop, so a later start cannot overlap the old thread
    _Atomic uint32_t types;           // memory types a thread other than this one allocated device memory of
    _Atomic int64_t made;             // blocks this thread brought in
    _Atomic int64_t other;            // device memory of 32 MiB or more other threads allocated (VMA blocks, large dedicated)
    VmaAllocation anchor[VK_MAX_MEMORY_TYPES];  // per memory type: the half block that keeps the newest spare block alive
    _Atomic int32_t logged;
} H;

// Set on the headroom thread only: the callback then knows the VkDeviceMemory it sees is this thread's own top-up (a
// lock here would invert against VMA's block mutex: the render thread's callback would hold VMA's lock while waiting).
static _Thread_local int in_headroom;

// ---- wake-up event: set from VMA's allocation callback on any thread, so it must not take a lock (the callback runs
// inside VMA's block allocation, and the headroom thread holds H.lock around its own VMA calls).
#ifdef _WIN32
static HANDLE hr_ev;
static void hr_ev_init(void) { hr_ev = CreateEventW(NULL, FALSE, FALSE, NULL); }
static int hr_ev_ready(void) { return hr_ev != NULL; }
static void hr_ev_set(void) { if (hr_ev) SetEvent(hr_ev); }
static void hr_ev_wait(int ms) {
    if (hr_ev) WaitForSingleObject(hr_ev, ms < 0 ? INFINITE : (DWORD)ms);
    else if (ms > 0) plat_sleep_until_ns(plat_now_ns() + (uint64_t)ms * 1000000u);
}
#else
static int hr_ev_r = -1, hr_ev_w = -1;
static void hr_ev_init(void) {
    int p[2];
    if (pipe2(p, O_NONBLOCK | O_CLOEXEC) == 0) hr_ev_r = p[0], hr_ev_w = p[1];
}
static int hr_ev_ready(void) { return hr_ev_r >= 0; }
static void hr_ev_set(void) {
    if (hr_ev_w < 0) return;
    char c = 1;
    ssize_t n = write(hr_ev_w, &c, 1);  // (a full pipe stays readable: EAGAIN loses nothing)
    (void)n;
}
static void hr_ev_wait(int ms) {
    if (hr_ev_r < 0) {
        if (ms > 0) plat_sleep_until_ns(plat_now_ns() + (uint64_t)ms * 1000000u);
        return;
    }
    struct pollfd p = {.fd = hr_ev_r, .events = POLLIN};
    if (poll(&p, 1, ms) > 0) {
        char buf[128];
        while (read(hr_ev_r, buf, sizeof buf) > 0) {}
    }
}
#endif

static void VKAPI_PTR on_allocate(VmaAllocator a, uint32_t type, VkDeviceMemory m, VkDeviceSize size, void *user) {
    (void)a, (void)m, (void)user;
    if (!atomic_load_explicit(&H.on, memory_order_relaxed) || type >= VK_MAX_MEMORY_TYPES) return;
    if (in_headroom) {
        atomic_fetch_add_explicit(&H.made, 1, memory_order_relaxed);
        return;
    }
    if (size >= 32ull << 20) atomic_fetch_add_explicit(&H.other, 1, memory_order_relaxed);
    atomic_fetch_or_explicit(&H.types, 1u << type, memory_order_relaxed);
    hr_ev_set();
}

const VmaDeviceMemoryCallbacks *vk_headroom_callbacks(void) {
    static const VmaDeviceMemoryCallbacks cb = {.pfnAllocate = on_allocate};
    const char *e = getenv("XCDE_VRAM_HEADROOM");
    if (e && atoi(e) == 0) return NULL;
    if (!atomic_load(&H.inited)) {
        pthread_mutex_init(&H.lock, NULL);
        hr_ev_init();
        atomic_store(&H.inited, 1);
    }
    atomic_store(&H.on, 1);
    return &cb;
}

static VkDeviceSize block_size(const VkPhysicalDeviceMemoryProperties *mp, uint32_t type) {
    VkDeviceSize heap = mp->memoryHeaps[mp->memoryTypes[type].heapIndex].size;
    return heap <= HR_SMALL_HEAP ? heap / 8 : 256ull << 20;   // VMA's default preferredLargeHeapBlockSize
}

static bool room(uint32_t type, VkDeviceSize block) {
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(g_vk.vma, b);
    const VmaBudget *h = &b[g_vk.memory_props.memoryTypes[type].heapIndex];
    return h->budget && h->usage + 2 * block <= h->budget / 100 * 85;
}

static void drop_anchor(uint32_t type) {
    if (H.anchor[type]) vmaFreeMemory(g_vk.vma, H.anchor[type]);
    H.anchor[type] = NULL;
}

static void top_up(uint32_t type) {
    VkDeviceSize block = block_size(&g_vk.memory_props, type);
    if (!room(type, block)) {
        drop_anchor(type);
        return;
    }
    VkMemoryRequirements req = {.size = block / 2, .alignment = 256, .memoryTypeBits = 1u << type};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_UNKNOWN};
    VmaAllocation a = NULL;
    int64_t before = atomic_load_explicit(&H.made, memory_order_relaxed);
    if (vmaAllocateMemory(g_vk.vma, &req, &ai, &a, NULL) != VK_SUCCESS) return;
    if (atomic_load_explicit(&H.made, memory_order_relaxed) == before) {  // there was room: don't keep it
        vmaFreeMemory(g_vk.vma, a);
        return;
    }
    drop_anchor(type);
    H.anchor[type] = a;
    if (atomic_fetch_add_explicit(&H.logged, 1, memory_order_relaxed) < 32)
        LOG("[vk] headroom: a %.0f MiB block of memory type %u allocated ahead (%lld so far; %lld allocations of 32 MiB "
            "or more on other threads)\n", (double)block / 1048576.0, type,
            (long long)atomic_load_explicit(&H.made, memory_order_relaxed),
            (long long)atomic_load_explicit(&H.other, memory_order_relaxed));
}

static void *headroom_main(void *arg) {
    (void)arg;
    in_headroom = 1;
    plat_thread_name("vk headroom");
    plat_thread_background();
    uint32_t watch = 0;  // memory types seen so far
    for (;;) {
        hr_ev_wait(watch ? HR_PERIOD_MS : (hr_ev_ready() ? -1 : HR_PERIOD_MS));
        watch |= atomic_exchange_explicit(&H.types, 0, memory_order_relaxed);
        pthread_mutex_lock(&H.lock);
        int stop = atomic_load_explicit(&H.stop, memory_order_relaxed);
        if (!stop) {
            uint32_t n = g_vk.memory_props.memoryTypeCount;
            if (n > VK_MAX_MEMORY_TYPES) n = VK_MAX_MEMORY_TYPES;
            for (uint32_t t = 0; t < n; t++)
                if (watch >> t & 1) top_up(t);
        } else {
            for (uint32_t t = 0; t < VK_MAX_MEMORY_TYPES; t++) drop_anchor(t);
        }
        pthread_mutex_unlock(&H.lock);
        if (stop) return NULL;
    }
}

// Before the allocator is destroyed: waits for a running top-up, frees the anchors, and the thread allocates no more.
// The thread is joined, so a start after this one gets a thread of its own.
void vk_headroom_stop(void) {
    if (!atomic_load(&H.started)) return;
    pthread_mutex_lock(&H.lock);
    atomic_store(&H.stop, 1);
    for (uint32_t t = 0; t < VK_MAX_MEMORY_TYPES; t++) drop_anchor(t);
    pthread_mutex_unlock(&H.lock);
    hr_ev_set();
    pthread_join(H.thread, NULL);
    atomic_store(&H.started, 0);
}

void vk_headroom_start(void) {
    if (!atomic_load(&H.on) || atomic_load(&H.started)) return;
    atomic_store(&H.stop, 0);
    atomic_store(&H.types, 0);
    if (pthread_create(&H.thread, NULL, headroom_main, NULL) != 0) {
        atomic_store(&H.on, 0);
        LOG("[vk] headroom: no thread, new VMA blocks stay on the allocating thread\n");
        return;
    }
    atomic_store(&H.started, 1);
    LOG("[vk] headroom: the next VMA block of each memory type in use is allocated on a background thread "
        "(XCDE_VRAM_HEADROOM=0 turns it off)\n");
}
