// The game's own files from an extracted folder or a .nup package (runtime/services/game_files.h).  [XCDE] T280
//
// A .nup holds a game's titles (the base game, an update) with their exefs/ and romfs/ already decrypted, so its files
// are read in place: no keys, nothing converted or unpacked (Constitution art. II). The newest title is the one played,
// as RecompLauncher's build-ID check reads it. Only packages that store their files as they are can be used: a file
// compressed in one piece would have to be decompressed from its start for every read the game makes.
//
// Layout (little-endian), as RecompLauncher's src-tauri/src/nup.rs reads it: "NUP\x01" at 0, the declared size at
// 0x40; a TOC at 0x80 ("TOC\x01", u32 titles, u64 dirs, u64 files, u64 names offset from 0x80, u64 names size);
// titles at 0xb0, 0x40 bytes each (u64 id, u32 root, u32 exefs dir, u32 romfs dir, u32 version); then the dirs (24
// bytes: u32 name offset, u32 name length, u32 first child, u32 children, u64), the files (80 bytes: u32 name offset,
// u32 name length, u64 size, u64 data offset, u64 stored size, 32-byte hash, u8 codec: 0 stored as is), the child
// list (u32 nodes: below the dir count a dir, else a file) and the names.
#include "game_files.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../host/utf8_path.h"
#include "../platform.h"  // (pread on Windows)

#ifdef _WIN32
#define GF_OPEN_FLAGS (O_RDONLY | 0x8000)  // _O_BINARY
#else
#define GF_OPEN_FLAGS O_RDONLY
#endif

#define NONE 0xFFFFFFFFu
#define TITLES_AT 0xb0u
#define MAX_TABLES (256u << 20)  // (a game's tables are a few MB: anything larger is a damaged file)

typedef struct {
    char *name;
    bool dir;
    uint32_t first, count;  // dirs: their children in g_children
    uint64_t size, offset;  // files: where their bytes are in the package
} Node;

static char g_root[1100];      // what gf_init was given
static bool g_ready, g_package;
static char g_prefix[1104];    // "<package>/": game paths inside the package start with it
static Node *g_nodes;
static uint32_t g_nnodes, g_ndirs, g_nchildren;
static uint32_t *g_children;
static uint32_t g_exefs = NONE, g_romfs = NONE;

static uint32_t u32le(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64le(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static bool fail(char *why, size_t n, const char *text) {
    snprintf(why, n, "%s: %s", g_root, text);
    return false;
}

// A package is 15 GB, so offsets and sizes are 64-bit here. The Windows build's CRT stat size is 32-bit; folder
// sources use xcde_utf8_file_info (GetFileInformationByHandle), and package reads use ReadFile with a 64-bit OVERLAPPED
// offset.
int64_t gf_pread_at(int fd, void *dst, uint64_t n, uint64_t off) {
    uint64_t done = 0;
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    while (done < n) {
        OVERLAPPED ov = {0};
        uint64_t at = off + done;
        ov.Offset = (DWORD)at, ov.OffsetHigh = (DWORD)(at >> 32);
        DWORD chunk = n - done > 0x40000000 ? 0x40000000 : (DWORD)(n - done), got = 0;
        if (!ReadFile(h, (uint8_t *)dst + done, chunk, &got, &ov) || !got) break;
        done += got;
    }
#else
    while (done < n) {
        ssize_t got = pread(fd, (uint8_t *)dst + done, (size_t)(n - done), (off_t)(off + done));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        done += (uint64_t)got;
    }
#endif
    return done || !n ? (int64_t)done : -1;
}

static bool fd_size(int fd, uint64_t *size) {
#ifdef _WIN32
    LARGE_INTEGER li;
    if (!GetFileSizeEx((HANDLE)_get_osfhandle(fd), &li)) return false;
    *size = (uint64_t)li.QuadPart;
    return true;
#else
    struct stat st;
    if (fstat(fd, &st) != 0) return false;
    *size = (uint64_t)st.st_size;
    return true;
#endif
}

// By its first bytes, never its name (a folder fails to open on Windows, or to read elsewhere).
static bool is_package_file(const char *path) {
    int fd = xcde_utf8_open(path, GF_OPEN_FLAGS, 0);
    if (fd < 0) return false;
    uint8_t magic[4] = {0};
    bool yes = gf_pread_at(fd, magic, 4, 0) == 4 && !memcmp(magic, "NUP\x01", 4);
    close(fd);
    return yes;
}

// The node's children, bounds-checked against the child list.
static bool children_ok(const Node *d) {
    return d->first <= g_nchildren && d->count <= g_nchildren - d->first;
}

static bool parse(const uint8_t *t, uint64_t tables_size, uint64_t file_size, char *why, size_t why_size) {
    uint32_t ntitles = u32le(t + 0x84);
    uint64_t ndirs = u64le(t + 0x88), nfiles = u64le(t + 0x90), names_off = 0x80 + u64le(t + 0x98);
    if (!ntitles || ntitles > 64 || !ndirs || ndirs > (1u << 24) || nfiles > (1u << 24))
        return fail(why, why_size, "its table of contents can't be read (a damaged or incomplete .nup)");
    uint64_t dirs_at = TITLES_AT + 0x40ull * ntitles, files_at = dirs_at + 24 * ndirs, kids_at = files_at + 80 * nfiles;
    if (kids_at > names_off || names_off > tables_size)
        return fail(why, why_size, "its table of contents can't be read (a damaged or incomplete .nup)");
    g_ndirs = (uint32_t)ndirs;
    g_nnodes = (uint32_t)(ndirs + nfiles);
    g_nchildren = (uint32_t)((names_off - kids_at) / 4);
    g_nodes = calloc(g_nnodes, sizeof *g_nodes);
    g_children = malloc(sizeof *g_children * (g_nchildren ? g_nchildren : 1));
    if (!g_nodes || !g_children) return fail(why, why_size, "not enough memory for its table of contents");
    for (uint32_t i = 0; i < g_nchildren; i++) g_children[i] = u32le(t + kids_at + 4ull * i);
    for (uint32_t i = 0; i < g_nnodes; i++) {
        bool dir = i < g_ndirs;
        const uint8_t *e = dir ? t + dirs_at + 24ull * i : t + files_at + 80ull * (i - g_ndirs);
        uint64_t name_at = names_off + u32le(e), name_len = u32le(e + 4);
        if (name_at + name_len > tables_size) return fail(why, why_size, "a name in its table is out of range");
        Node *n = &g_nodes[i];
        n->dir = dir;
        n->name = malloc(name_len + 1);
        if (!n->name) return fail(why, why_size, "not enough memory for its table of contents");
        memcpy(n->name, t + name_at, name_len);
        n->name[name_len] = 0;
        if (dir) {
            n->first = u32le(e + 8), n->count = u32le(e + 12);
            if (!children_ok(n)) return fail(why, why_size, "a folder in its table is out of range");
            continue;
        }
        n->size = u64le(e + 8), n->offset = u64le(e + 16);
        uint64_t stored = u64le(e + 24);
        if (e[64] != 0 || stored != n->size) {
            char text[512];
            snprintf(text, sizeof text, "%s is stored compressed. Monado reads .nup packages whose files are stored as "
                     "they are: make the package again without compression", n->name);
            return fail(why, why_size, text);
        }
        if (n->offset > file_size || n->size > file_size - n->offset)
            return fail(why, why_size, "it is incomplete (a file lies past its end): copy or make it again");
    }
    // Only the children the folders list are checked: the list may be padded before the names.
    for (uint32_t d = 0; d < g_ndirs; d++)
        for (uint32_t i = 0; i < g_nodes[d].count; i++)
            if (g_children[g_nodes[d].first + i] >= g_nnodes) return fail(why, why_size, "its folder list is out of range");
    // The newest title (an update over the base game; the first of equals), as the launcher's build-ID check reads it.
    uint32_t best_version = 0;
    bool found = false;
    for (uint32_t i = 0; i < ntitles; i++) {
        const uint8_t *e = t + TITLES_AT + 0x40ull * i;
        uint32_t exefs = u32le(e + 12), romfs = u32le(e + 16), version = u32le(e + 20);
        if (found && version <= best_version) continue;
        found = true, best_version = version;
        g_exefs = exefs < g_ndirs ? exefs : NONE;
        g_romfs = romfs < g_ndirs ? romfs : NONE;
    }
    if (g_exefs == NONE) return fail(why, why_size, "its newest title has no exefs");
    if (g_romfs == NONE) return fail(why, why_size, "its newest title has no romfs");
    return true;
}

static bool open_package(char *why, size_t why_size) {
    int fd = xcde_utf8_open(g_root, GF_OPEN_FLAGS, 0);
    if (fd < 0) return fail(why, why_size, "can't be opened");
    uint8_t head[0xb0];
    uint64_t file_size = 0;
    bool ok = fd_size(fd, &file_size) && gf_pread_at(fd, head, sizeof head, 0) == (int64_t)sizeof head;
    if (!ok || memcmp(head + 0x80, "TOC\x01", 4)) {
        close(fd);
        return fail(why, why_size, "isn't a .nup package Monado can read");
    }
    uint64_t declared = u64le(head + 0x40);
    if (declared && file_size < declared) {
        close(fd);
        return fail(why, why_size, "is incomplete (smaller than its header says): copy or make it again");
    }
    uint64_t tables_size = 0x80 + u64le(head + 0x98) + u64le(head + 0xa0);
    if (tables_size < sizeof head || tables_size > MAX_TABLES || tables_size > file_size) {
        close(fd);
        return fail(why, why_size, "its table of contents can't be read (a damaged or incomplete .nup)");
    }
    uint8_t *tables = malloc(tables_size);
    ok = tables && gf_pread_at(fd, tables, tables_size, 0) == (int64_t)tables_size;
    close(fd);
    if (!ok) {
        free(tables);
        return fail(why, why_size, "its table of contents can't be read");
    }
    ok = parse(tables, tables_size, file_size, why, why_size);
    free(tables);
    return ok;
}

bool gf_init(const char *root, char *why, size_t why_size) {
    if (g_ready && !strcmp(root, g_root)) return true;
    // (another root: the last one's tables are dropped; the game opens one, the tests several)
    g_ready = g_package = false;
    for (uint32_t i = 0; i < g_nnodes && g_nodes; i++) free(g_nodes[i].name);
    free(g_nodes), free(g_children);
    g_nodes = NULL, g_children = NULL, g_nnodes = g_ndirs = g_nchildren = 0;
    g_exefs = g_romfs = NONE;
    snprintf(g_root, sizeof g_root, "%s", root);
    g_package = is_package_file(root);  // (no stat: its size is 32-bit on Windows, and a package is 15 GB)
    if (g_package) {
        snprintf(g_prefix, sizeof g_prefix, "%s/", root);
        if (!open_package(why, why_size)) return false;
    }
    g_ready = true;
    return true;
}

bool gf_is_package(void) { return g_package; }

// The node of a path inside the package ("exefs/main", "romfs/menu"), or -1.
static int find(const char *rel) {
    char part[512];
    int node = -1;
    for (const char *p = rel; *p;) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (!len || len >= sizeof part) return -1;
        memcpy(part, p, len);
        part[len] = 0;
        p += len + (slash ? 1 : 0);
        if (node < 0) {  // the first part names the title's folder
            uint32_t top = !strcmp(part, "exefs") ? g_exefs : !strcmp(part, "romfs") ? g_romfs : NONE;
            if (top == NONE) return -1;
            node = (int)top;
            continue;
        }
        const Node *d = &g_nodes[node];
        if (!d->dir) return -1;
        int hit = -1;
        for (uint32_t i = 0; i < d->count && hit < 0; i++)
            if (!strcmp(g_nodes[g_children[d->first + i]].name, part)) hit = (int)g_children[d->first + i];
        if (hit < 0) return -1;
        node = hit;
    }
    return node;
}

// The part of a game path after "<package>/", or NULL when the path isn't inside the package.
static const char *inside(const char *path) {
    if (!g_package) return NULL;
    size_t n = strlen(g_prefix);
    return strncmp(path, g_prefix, n) ? NULL : path + n;
}

bool gf_source(const char *path, GfSource *out) {
    const char *rel = inside(path);
    if (rel) {
        int node = find(rel);
        if (node < 0 || g_nodes[node].dir) return false;
        *out = (GfSource){g_root, g_nodes[node].offset, g_nodes[node].size};
        return true;
    }
    XcdeFileInfo fi;
    if (!xcde_utf8_file_info(path, &fi) || !fi.isreg) return false;
    *out = (GfSource){path, 0, fi.size};
    return true;
}

int gf_open(const GfSource *src) { return xcde_utf8_open(src->path, GF_OPEN_FLAGS, 0); }

int64_t gf_pread(const GfSource *src, void *dst, uint64_t n, uint64_t off) {
    if (off >= src->size) return 0;
    if (n > src->size - off) n = src->size - off;
    int fd = gf_open(src);
    if (fd < 0) return -1;
    int64_t done = gf_pread_at(fd, dst, n, src->base + off);
    close(fd);
    return done;
}

uint8_t *gf_read(const char *path, size_t *len) {
    GfSource src;
    if (!gf_source(path, &src)) return NULL;
    uint8_t *buf = malloc(src.size ? (size_t)src.size : 1);
    if (!buf) return NULL;
    if (gf_pread(&src, buf, src.size, 0) != (int64_t)src.size) {
        free(buf);
        return NULL;
    }
    *len = (size_t)src.size;
    return buf;
}

int gf_pkg_node(const char *path) {
    const char *rel = inside(path);
    return rel ? find(rel) : -1;
}

int gf_pkg_children(int node, const uint32_t **children) {
    if (node < 0 || !g_nodes[node].dir) return 0;
    *children = g_children + g_nodes[node].first;
    return (int)g_nodes[node].count;
}

bool gf_pkg_is_dir(int node) { return node >= 0 && g_nodes[node].dir; }
const char *gf_pkg_name(int node) { return g_nodes[node].name; }
GfSource gf_pkg_source(int node) { return (GfSource){g_root, g_nodes[node].offset, g_nodes[node].size}; }
