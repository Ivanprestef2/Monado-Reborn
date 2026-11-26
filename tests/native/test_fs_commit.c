// T037: atomic save writes and commits (runtime/services/fs.c).
// fs.c is included so its static write / journal helpers are driven directly; pwrite is mocked to return short writes,
// ENOSPC and EINTR. The runtime symbols referenced by the service handlers are stubbed below so the test links on its own.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#define fs_getpid _getpid
static ssize_t real_pwrite(int fd, const void *buf, size_t n, off_t off) {
    __int64 old = _lseeki64(fd, (__int64)off, SEEK_SET);
    if (old < 0) return -1;
    int written = _write(fd, buf, (unsigned)n);
    int error = errno;
    _lseeki64(fd, old, SEEK_SET);
    errno = error;
    return written;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    __int64 old = _lseeki64(fd, (__int64)off, SEEK_SET);
    if (old < 0) return -1;
    int got = _read(fd, buf, (unsigned)n);
    int error = errno;
    _lseeki64(fd, old, SEEK_SET);
    errno = error;
    return got;
}
#else
#define fs_getpid getpid
static ssize_t real_pwrite(int fd, const void *buf, size_t n, off_t off) {
    return pwrite(fd, buf, n, off);
}
#endif

static int mock_write_mode;
enum { MOCK_NORMAL, MOCK_SHORT, MOCK_ZERO, MOCK_FAIL, MOCK_EINTR };
static int mock_eintr_once;
ssize_t mock_pwrite(int fd, const void *buf, size_t n, off_t off) {
    switch (mock_write_mode) {
    case MOCK_SHORT: if (n > 3) n = 3; break;
    case MOCK_ZERO: return 0;
    case MOCK_FAIL: errno = ENOSPC; return -1;
    case MOCK_EINTR:
        if (!mock_eintr_once) { mock_eintr_once = 1; errno = EINTR; return -1; }
        break;
    default: break;
    }
    return real_pwrite(fd, buf, n, off);
}
#define pwrite mock_pwrite
#include "../../runtime/services/fs.c"
#undef pwrite

void rt_logf(const char *fmt, ...) { (void)fmt; }
void diag_unimplemented(const char *kind, const char *fmt, ...) { (void)kind; (void)fmt; }
void plat_will_write(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

static int qs_flush_fail, qs_committed_calls;
int quickstep_save_prepare(const char *root, const char *path, const void *payload, int64_t offset, int64_t bytes,
                           QuickstepSaveWrite *plan) {
    (void)root; (void)path; (void)payload; (void)offset; (void)bytes; (void)plan; return 0;
}
int quickstep_save_stage(const char *root, const QuickstepSaveWrite *plan) { (void)root; (void)plan; return 0; }
int quickstep_save_flush(const char *root) {
    (void)root;
    if (qs_flush_fail) { errno = ENOSPC; return -1; }
    return 0;
}
void quickstep_save_committed(const char *root) { (void)root; qs_committed_calls++; }
void quickstep_save_observe(const char *root, const char *path, const void *payload, int64_t offset, int64_t bytes) {
    (void)root; (void)path; (void)payload; (void)offset; (void)bytes;
}
int quickstep_save_changed(const char *root, const char *path, const char *destination) {
    (void)root; (void)path; (void)destination; return 0;
}
void save_editor_file_read(const char *path, uint64_t addr, int64_t off, int64_t size) {
    (void)path; (void)addr; (void)off; (void)size;
}
void flags_ext_save_read(const char *path, uint64_t addr, int64_t off, int64_t size) {
    (void)path; (void)addr; (void)off; (void)size;
}
void flags_ext_save_written(const char *path, uint64_t addr, int64_t off, int64_t size) {
    (void)path; (void)addr; (void)off; (void)size;
}

// The Windows linker keeps the service-section handlers even with --gc-sections; these make the whole fs.c unit link
// without pulling in the rest of the runtime (the test never calls the service registration paths).
const char *g_romfs_dir = "targets/romfs";
bool gf_is_package(void) { return false; }
bool rando_romfs_override(const char *path, char *out, size_t cap) { (void)path; (void)out; (void)cap; return false; }
const char *rando_seed_dir(void) { return NULL; }
bool rvfs_init(const char *dir, char *why, size_t why_size) { (void)dir; (void)why; (void)why_size; return false; }
uint64_t rvfs_size(void) { return 0; }
int64_t rvfs_read(uint8_t *dst, uint64_t size, uint64_t off) { (void)dst; (void)size; (void)off; return 0; }
const char *rvfs_file_at(uint64_t off) { (void)off; return NULL; }
void romfs_file_read_hook(const char *path) { (void)path; }
Service *service_new(const char *name, ServiceHandler h, void *state) { (void)name; (void)h; (void)state; return NULL; }
void ipc_return_object(IpcResp *resp, Service *s) { (void)resp; (void)s; }
void rt_fatal_trace(void) { abort(); }

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static char base[1024], root[1100], journal[1200];

static void path_join(char *out, size_t cap, const char *a, const char *b) {
    snprintf(out, cap, "%s/%s", a, b);
}
static int write_text(const char *path, const char *text) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return -1;
    size_t n = strlen(text), at = 0;
    while (at < n) {
        ssize_t w = write(fd, text + at, n - at);
        if (w <= 0) { close(fd); return -1; }
        at += (size_t)w;
    }
    return close(fd);
}
static int read_text(const char *path, char *out, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0) return -1;
    out[n] = 0;
    return 0;
}
static bool exists(const char *path) { struct stat sb; return !stat(path, &sb); }

static void reset_tree(void) {
    snprintf(base, sizeof base, "build/test-fs-commit-%ld", (long)fs_getpid());
    snprintf(root, sizeof root, "%s/save_0", base);
    snprintf(journal, sizeof journal, "%s.journal", root);
    rm_rf(base, false);
    xcde_utf8_mkdir("build");
    xcde_utf8_mkdir(base);
    xcde_utf8_mkdir(root);
}

static void test_pwrite_all(void) {
    char p[1200], got[64] = {0};
    path_join(p, sizeof p, root, "short.bin");
    int fd = open(p, O_CREAT | O_TRUNC | O_RDWR, 0644);
    CHECK(fd >= 0);
    const char payload[] = "abcdefghij";
    mock_write_mode = MOCK_SHORT;
    CHECK(pwrite_all(fd, payload, sizeof payload - 1, 0) == 0);
    mock_write_mode = MOCK_NORMAL;
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, got, sizeof got - 1) == (ssize_t)(sizeof payload - 1));
    CHECK(!strcmp(got, payload));
    close(fd);

    fd = open(p, O_CREAT | O_TRUNC | O_RDWR, 0644);
    CHECK(fd >= 0);
    mock_write_mode = MOCK_ZERO;
    CHECK(pwrite_all(fd, payload, sizeof payload - 1, 0) == -1);
    CHECK(errno == EIO);
    mock_write_mode = MOCK_FAIL;
    CHECK(pwrite_all(fd, payload, sizeof payload - 1, 0) == -1);
    CHECK(errno == ENOSPC);
    mock_write_mode = MOCK_EINTR;
    mock_eintr_once = 0;
    CHECK(pwrite_all(fd, payload, sizeof payload - 1, 0) == 0);
    mock_write_mode = MOCK_NORMAL;
    close(fd);
}

static void test_file_write(void) {
    char p[1200], got[8] = {0};
    path_join(p, sizeof p, root, "plain.bin");
    int fd = open(p, O_CREAT | O_TRUNC | O_RDWR, 0644);
    CHECK(fd >= 0);
    HostFile f = {0};
    f.fd = fd;
    strlcpy(f.path, p, sizeof f.path);
    strlcpy(f.root, root, sizeof f.root);
    f.journal = -1;
    Service svc = {.name = "IFile", .state = &f};
    char payload[] = "xyz";
    uint8_t data[32] = {0};
    IpcReq req = {.cmd = 1, .data = data};
    IpcResp resp = {0};
    req.a[0] = (IpcBuf){(uint64_t)(uintptr_t)payload, sizeof payload - 1};
    req.na = 1;

    // Offset / size are validated for every file, not only save mounts.
    *(int64_t *)(data + 8) = -1;
    *(int64_t *)(data + 16) = 1;
    file(&svc, &req, &resp);
    CHECK(resp.result != 0);

    memset(&resp, 0, sizeof resp);
    *(int64_t *)(data + 8) = 0;
    *(int64_t *)(data + 16) = -1;
    file(&svc, &req, &resp);
    CHECK(resp.result != 0);

    memset(&resp, 0, sizeof resp);
    *(int64_t *)(data + 8) = 0;
    *(int64_t *)(data + 16) = (int64_t)sizeof payload;  // one past the IPC buffer
    file(&svc, &req, &resp);
    CHECK(resp.result != 0);

    memset(&resp, 0, sizeof resp);
    *(int64_t *)(data + 8) = INT64_MAX;
    *(int64_t *)(data + 16) = 1;  // offset + size would wrap
    file(&svc, &req, &resp);
    CHECK(resp.result != 0);

    lseek(fd, 0, SEEK_SET);
    CHECK(read(fd, got, sizeof got) == 0);

    memset(&resp, 0, sizeof resp);
    *(int64_t *)(data + 8) = 1;
    *(int64_t *)(data + 16) = 2;
    mock_write_mode = MOCK_FAIL;
    file(&svc, &req, &resp);
    mock_write_mode = MOCK_NORMAL;
    CHECK(resp.result != 0);

    memset(&resp, 0, sizeof resp);
    *(int64_t *)(data + 8) = 1;
    *(int64_t *)(data + 16) = 2;
    mock_write_mode = MOCK_SHORT;
    file(&svc, &req, &resp);
    mock_write_mode = MOCK_NORMAL;
    CHECK(resp.result == 0);
    lseek(fd, 0, SEEK_SET);
    CHECK(read(fd, got, sizeof got) == 3);
    CHECK(got[0] == 0 && got[1] == 'x' && got[2] == 'y');

    memset(&resp, 0, sizeof resp);
    req.cmd = 2;
    file(&svc, &req, &resp);
    CHECK(resp.result == 0);       // a real fsync succeeds
    f.fd = -1;
    memset(&resp, 0, sizeof resp);
    file(&svc, &req, &resp);
    CHECK(resp.result != 0);       // ... and a failed one is reported
    f.fd = fd;
    close(fd);
}

static void test_journal_commit(void) {
    char file[1200], text[8] = {0};
    reset_tree();
    path_join(file, sizeof file, root, "old.bin");
    CHECK(write_text(file, "old") == 0);

    // The first change snapshots the committed save.
    snprintf(g_journals[0].root, sizeof g_journals[0].root, "%s", root);
    g_journals[0].dirty = false;
    CHECK(journal_touch(0) == 0);
    CHECK(exists(journal));
    CHECK(g_journals[0].dirty);
    path_join(file, sizeof file, journal, "old.bin");
    CHECK(read_text(file, text, sizeof text) == 0 && !strcmp(text, "old"));

    // A failed flush / fsync keeps the journal and leaves the commit dirty.
    qs_flush_fail = 1;
    qs_committed_calls = 0;
    CHECK(journal_commit(0) == -1);
    CHECK(exists(journal));
    CHECK(g_journals[0].dirty);

    qs_flush_fail = 0;
    CHECK(journal_commit(0) == 0);
    CHECK(!exists(journal));
    CHECK(!g_journals[0].dirty);
    CHECK(qs_committed_calls == 1);

    // A tree that cannot be flushed (here: it is gone) must not drop the journal either.
    reset_tree();
    path_join(file, sizeof file, root, "old.bin");
    CHECK(write_text(file, "old") == 0);
    snprintf(g_journals[1].root, sizeof g_journals[1].root, "%s", root);
    g_journals[1].dirty = false;
    CHECK(journal_touch(1) == 0);
    CHECK(exists(journal));
    rm_rf(root, false);
    CHECK(journal_commit(1) == -1);
    CHECK(exists(journal));
    CHECK(g_journals[1].dirty);

    // A missing source tree cannot create a journal, so the change is refused before it starts.
    g_journals[1].dirty = false;
    CHECK(journal_touch(1) == -1);
    CHECK(!g_journals[1].dirty);
}

int main(void) {
    reset_tree();
    test_pwrite_all();
    test_file_write();
    test_journal_commit();
    rm_rf(base, false);
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
