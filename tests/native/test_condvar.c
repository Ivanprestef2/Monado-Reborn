// T031 (tests first): Horizon semantics of WaitProcessWideKeyAtomic (condition variable wait).
// On a timeout the SVC returns WITHOUT the mutex: the caller (nn::os) takes it again itself. Taking it in the SVC
// as well left the thread waiting on its own lock forever (LASRecomp #74/#88; seen in the first XCDE boot).
// kernel.c is included so its static SVC helpers can be called directly.
#include "../../runtime/kernel.c"

// ---- stubs for what kernel.c expects from the rest of the runtime
void rt_dispatch(Cpu *c) { (void)c; }
GuestFn dispatch_lookup(uint64_t addr) { (void)addr; return NULL; }
Module g_modules[4]; int g_nmodules;
void guest_call(Cpu *c, uint64_t addr) { (void)c; (void)addr; }
void crash_handler_install(void) {}
void rt_fatal_trace(void) { abort(); }
void rt_fault_report(const char *w, uint64_t a, uint64_t p, uint64_t l) { (void)w; (void)a; (void)p; (void)l; abort(); }
bool rt_fault_watch(uint64_t a, uint64_t p) { (void)a; (void)p; return false; }
const char *rt_host_fn_name(uint64_t pc, uint64_t *o) { (void)pc; (void)o; return NULL; }
uint32_t vi_vsync_handle(void) { return 0; }
uint64_t renderer_present_count(void) { return 0; } // no renderer in this kernel-only harness
const char *loader_symbol_at(uint64_t a, uint64_t *o) { (void)a; (void)o; return NULL; }
void diag_crash_report(void *ep) { (void)ep; }

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

typedef struct {
    pthread_mutex_t *start;
    uint64_t mapped[8];
    uint64_t backing[8];
    bool ok;
} RegionWorker;

static void *map_and_unmap(void *arg) {
    RegionWorker *worker = arg;
    uint64_t mapped = (uintptr_t)worker->mapped;
    uint64_t backing = (uintptr_t)worker->backing;
    uint64_t size = sizeof worker->mapped;
    Cpu cpu = {0};
    pthread_mutex_lock(worker->start);
    pthread_mutex_unlock(worker->start);
    for (int i = 0; i < 2000; i++) {
        cpu.x[0] = mapped;
        cpu.x[1] = backing;
        cpu.x[2] = size;
        svc_impl(&cpu, 0x04, 0);  // MapMemory

        uint64_t info[5];
        cpu.x[0] = (uintptr_t)info;
        cpu.x[2] = mapped;
        svc_impl(&cpu, 0x06, 0);  // QueryMemory
        if (info[0] != mapped || info[1] != size || info[2] != 0xB) return NULL;

        cpu.x[0] = mapped;
        cpu.x[1] = backing;
        cpu.x[2] = size;
        svc_impl(&cpu, 0x05, 0);  // UnmapMemory
    }
    worker->ok = true;
    return NULL;
}

static void test_concurrent_memory_map(void) {
    enum { WORKERS = 8 };
    RegionWorker *workers = calloc(WORKERS, sizeof *workers);
    CHECK(workers != NULL);
    if (!workers) return;
    pthread_t threads[WORKERS];
    pthread_mutex_t start = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&start);
    int created = 0;
    for (; created < WORKERS; created++) {
        workers[created].start = &start;
        if (pthread_create(&threads[created], NULL, map_and_unmap, &workers[created])) break;
    }
    pthread_mutex_unlock(&start);
    CHECK(created == WORKERS);
    for (int i = 0; i < created; i++) {
        pthread_join(threads[i], NULL);
        CHECK(workers[i].ok);
    }
    CHECK(nregions == 0);
    pthread_mutex_destroy(&start);
    free(workers);
}

enum { THREAD_WORKERS = 8, THREADS_PER_WORKER = 1000 };

static void *create_guest_threads(void *arg) {
    pthread_mutex_t *start = arg;
    pthread_mutex_lock(start);
    pthread_mutex_unlock(start);
    for (int i = 0; i < THREADS_PER_WORKER; i++) thread_new(0, 0, 0, 0, 0);
    return NULL;
}

static void test_concurrent_thread_allocation(void) {
    pthread_t workers[THREAD_WORKERS];
    pthread_mutex_t start = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&start);
    int created = 0;
    for (; created < THREAD_WORKERS; created++)
        if (pthread_create(&workers[created], NULL, create_guest_threads, &start)) break;
    pthread_mutex_unlock(&start);
    for (int i = 0; i < created; i++) pthread_join(workers[i], NULL);
    pthread_mutex_destroy(&start);
    CHECK(created == THREAD_WORKERS);

    uint8_t seen_ids[THREAD_WORKERS * THREADS_PER_WORKER] = {0};
    uint8_t seen_tls[THREAD_WORKERS * THREADS_PER_WORKER] = {0};
    int count = 0;
    for (KThread *t = g_threads; t; t = t->anext, count++) {
        uint64_t id = t->id - 0x50, slot = (t->tlr - TLS_BASE) / 0x200;
        CHECK(id < sizeof seen_ids && !seen_ids[id]);
        CHECK(slot < sizeof seen_tls && !seen_tls[slot]);
        CHECK(t->cpu.tpidrro == t->tlr);
        if (id < sizeof seen_ids) seen_ids[id] = 1;
        if (slot < sizeof seen_tls) seen_tls[slot] = 1;
    }
    CHECK(count == created * THREADS_PER_WORKER);
    kernel_reset();
}

static int session_closes;

static void session_close_callback(Service *service) {
    (void)service;
    int rc = pthread_mutex_trylock(&g_klock);
    CHECK(rc == 0);
    if (!rc) pthread_mutex_unlock(&g_klock);
    session_closes++;
}

static uint32_t test_session(void) {
    Service *service = service_new("test", NULL, NULL);
    service->on_close = session_close_callback;
    KSession *session = calloc(1, sizeof *session);
    session->obj.type = KOBJ_SESSION;
    session->svc = service;
    return kernel_new_handle(&session->obj);
}

static void test_session_lifetime(void) {
    _Alignas(16) uint32_t msg[64] = {0};
    Cpu cpu = {.tpidrro = (uintptr_t)msg};
    uint32_t handle = test_session();
    cpu.x[0] = handle;
    svc_impl(&cpu, 0x16, 0);
    CHECK(session_closes == 1);

    handle = test_session();
    KObject *active = kernel_session_acquire(handle);
    CHECK(active != NULL);
    handle_close(handle);
    CHECK(session_closes == 1);
    if (active) kernel_session_release(active);
    CHECK(session_closes == 2);

    handle = test_session();
    msg[0] = 2;
    CHECK(ipc_handle_request(&cpu, handle) == R_OK);
    CHECK(session_closes == 3);
    handle_close(handle);
    CHECK(session_closes == 3);

    handle = test_session();
    msg[0] = 5; msg[6] = 0;  // ConvertCurrentObjectToDomain
    CHECK(ipc_handle_request(&cpu, handle) == R_OK);
    msg[0] = 5; msg[1] = 0; msg[6] = 2;  // CloneCurrentObject
    CHECK(ipc_handle_request(&cpu, handle) == R_OK);
    uint32_t clone = msg[3];
    CHECK(clone != 0 && clone != handle);
    handle_close(handle);
    CHECK(session_closes == 3);
    handle_close(clone);
    CHECK(session_closes == 4);
}

int main(void) {
    static uint32_t mutex_word, key_word;
    KThread th; memset(&th, 0, sizeof th);
    pthread_cond_init(&th.cond, NULL);
    th.handle = 0x77;
    t_cur = &th;

    // 1. timed wait that runs out: returns R_TIMEOUT and the mutex is NOT held afterwards
    mutex_word = th.handle | 0;           // the caller owns the mutex (value = owner handle, no waiters)
    uint32_t r = svc_wait_process_wide_key((uint64_t)(uintptr_t)&mutex_word, (uint64_t)(uintptr_t)&key_word, th.handle, 20 * 1000000ll);
    CHECK(r == R_TIMEOUT);
    CHECK(mutex_word == 0);               // released, and not re-taken by the SVC
    CHECK(th.wait_kind == WAIT_NONE);     // not left waiting on its own lock

    // 2. the caller can take the mutex itself afterwards (what nn::os does): ArbitrateLock must not block
    mutex_word = th.handle | WAITERS_BIT; // simulate "mutex contended by others"? no: nobody owns it
    mutex_word = 0;
    CHECK(svc_arbitrate_lock(th.handle, (uint64_t)(uintptr_t)&mutex_word, th.handle) == R_OK);

    // 3. zero timeout behaves the same way
    mutex_word = th.handle;
    r = svc_wait_process_wide_key((uint64_t)(uintptr_t)&mutex_word, (uint64_t)(uintptr_t)&key_word, th.handle, 0);
    CHECK(r == R_TIMEOUT);
    CHECK(mutex_word == 0);

    test_concurrent_memory_map();
    test_concurrent_thread_allocation();
    test_session_lifetime();

    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
