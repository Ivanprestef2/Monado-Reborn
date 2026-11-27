// [XCDE] Native C replacements for the hot static map collision-BVH traversal and AABB tests
// (generated/main/main_172.c: f_7000734320, f_7000732cd0, f_7000733930).
// Disable at runtime with XCDE_COL_BVH=0; verify against lifted code with XCDE_COL_BVH_VERIFY=1.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// [XCDE] Wave 12: On x86-64-v2 (no hardware FMA3), libm fmaf() is a slow PLT call that
// spills all XMM registers. Since float*float (48b) is exact in double (53b), computing
// (float)((double)a * (double)b + (double)c) stays inline in 4 SSE2 instructions with
// effectively identical single-rounding precision.
#ifndef __FMA__
static inline float xcde_fast_fmaf(float a, float b, float c) {
    return (float)((double)a * (double)b + (double)c);
}
#undef fmaf
#define fmaf(a, b, c) xcde_fast_fmaf((a), (b), (c))
#endif

#include "../hle.h"
#include "../platform.h"
#include "../rt.h"

extern GuestFn rt_resolve_fn(Cpu *c, uint64_t pc);

#define ADDR_AABB_REJECT     0x7000734320ull
#define ADDR_SEG_AABB_REJECT 0x7000732cd0ull
#define ADDR_BVH_TRAVERSE    0x7000733930ull
#define ADDR_RAY_POLY        0x700072f7b0ull
#define ADDR_SWEEP_POLY      0x7000730d80ull
#define ADDR_CAPSULE_POLY    0x7000730490ull
#define ADDR_SEG_SEG         0x7000726950ull
#define ADDR_T_RANGE_01      0x7000731960ull
#define ADDR_T_RANGE_LOHI    0x7000731980ull
#define ADDR_T_RANGE_ABS     0x70007319b0ull
#define ADDR_T_RANGE_0HI     0x70007319d0ull

static int g_verify = -1;

static uint32_t col_aabb_reject(uint64_t ctx, uint64_t p_max, uint64_t p_min) {
    const float *q = (const float *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(ctx + 0x18));
    const float *n_max = (const float *)(uintptr_t)p_max;
    const float *n_min = (const float *)(uintptr_t)p_min;
    return (n_min[0] > q[0] || n_min[1] > q[1] || n_min[2] > q[2] ||
            q[4] > n_max[0] || q[5] > n_max[1] || q[6] > n_max[2]) ? 1u : 0u;
}

static uint32_t col_seg_aabb_reject(uint64_t ctx, uint64_t p_max, uint64_t p_min) {
    const float *q = (const float *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(ctx + 0x18));
    const float *n_max = (const float *)(uintptr_t)p_max;
    const float *n_min = (const float *)(uintptr_t)p_min;
    if (n_min[0] > q[0] || n_min[1] > q[1] || n_min[2] > q[2] ||
        q[4] > n_max[0] || q[5] > n_max[1] || q[6] > n_max[2]) {
        return 1u;
    }

    float hx = (n_max[0] - n_min[0]) * 0.5f;
    float hy = (n_max[1] - n_min[1]) * 0.5f;
    float hz = (n_max[2] - n_min[2]) * 0.5f;

    float dx = q[16] - (n_min[0] + hx);
    float dy = q[17] - (n_min[1] + hy);
    float dz = (-n_min[2] - hz) + q[18];

    float dir_x = q[20], dir_y = q[21], dir_z = q[22];
    float abs_x = q[24], abs_y = q[25], abs_z = q[26];

    if (!(fabsf(dir_z * dy - dz * dir_y) <= fmaf(abs_y, hz, abs_z * hy))) return 1u;
    if (!(fabsf(dir_x * dz - dir_z * dx) <= fmaf(abs_x, hz, abs_z * hx))) return 1u;
    return (fabsf(dx * dir_y - dir_x * dy) <= fmaf(abs_x, hy, abs_y * hx)) ? 0u : 1u;
}

static inline uint64_t resolve_pmf(uint64_t self, uint64_t off, uint64_t *out_this) {
    uint64_t ptr = *(const uint64_t *)(uintptr_t)(self + off);
    int64_t adj = *(const int64_t *)(uintptr_t)(self + off + 8);
    uint64_t adj_this = self + (uint64_t)(adj >> 1);
    *out_this = adj_this;
    if (adj & 1) {
        uint64_t vtable = *(const uint64_t *)(uintptr_t)adj_this;
        return *(const uint64_t *)(uintptr_t)(vtable + ptr);
    }
    return ptr;
}

static inline uint32_t call_guest_1(Cpu *c, uint64_t target, uint64_t x0, uint32_t x1) {
    c->x[0] = x0;
    c->x[1] = x1;
    GuestFn fn = dispatch_lookup(target);
    if (__builtin_expect(fn != NULL, 1)) {
        c->x[30] = 0;
        fn(c);
    } else {
        c->pc = target;
        c->x[30] = 0;
        rt_dispatch(c);
    }
    return (uint32_t)c->x[0];
}

static void verify_against_guest(const Cpu *orig_in, uint64_t addr, uint32_t native_res, const char *name) {
    Cpu tmp = *orig_in;
    GuestFn fn = dispatch_lookup(addr);
    if (!fn) return;
    tmp.x[30] = 0;
    fn(&tmp);
    uint32_t guest_res = (uint32_t)(tmp.x[0] & 1u);
    if (guest_res != (native_res & 1u)) {
        FATAL("[col_bvh] VERIFY MISMATCH in %s (%#llx): native=%u guest=%u (x0=%llx x1=%llx x2=%llx)\n",
              name, (unsigned long long)addr, native_res & 1u, guest_res,
              (unsigned long long)orig_in->x[0], (unsigned long long)orig_in->x[1],
              (unsigned long long)orig_in->x[2]);
    }
}

static inline int col_edge_hit(float px, float py, float pz, const float *a, const float *b, float eps_sq) {
    float ex = b[0] - a[0], ey = b[1] - a[1], ez = b[2] - a[2];
    float apx = px - a[0], apy = py - a[1], apz = pz - a[2];
    float ee = fmaf(ez, ez, ex * ex) + ey * ey;
    float ae = fmaf(ex, apx, apz * ez) + ey * apy;
    float s = (ee > 0.0f) ? (ae / ee) : 0.0f;
    if (s < 0.0f || !(s == s)) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    float dx = ex * s - apx, dy = ey * s - apy, dz = ez * s - apz;
    float dist_sq = fmaf(dz, dz, dx * dx) + dy * dy;
    return dist_sq <= eps_sq;
}

static inline uint32_t col_check_pmf(Cpu *c, uint64_t self, uint64_t pmf_off, float val, float nx, float ny, float nz) {
    uint64_t t_this = 0;
    uint64_t t_fn = resolve_pmf(self, pmf_off, &t_this);
    if (t_fn == ADDR_T_RANGE_01) {
        return (val >= 0.0f && val <= 1.0f) ? 1u : 0u;
    }
    if (t_fn == ADDR_T_RANGE_LOHI) {
        uint64_t q = *(const uint64_t *)(uintptr_t)(t_this + 0x18);
        float lo = *(const float *)(uintptr_t)(q + 0x94);
        float hi = *(const float *)(uintptr_t)(q + 0x90);
        return (val >= lo && val <= hi) ? 1u : 0u;
    }
    if (t_fn == ADDR_T_RANGE_ABS) {
        uint64_t q = *(const uint64_t *)(uintptr_t)(t_this + 0x18);
        float hi = *(const float *)(uintptr_t)(q + 0x90);
        return !(fabsf(val) > hi) ? 1u : 0u;
    }
    if (t_fn == ADDR_T_RANGE_0HI) {
        uint64_t q = *(const uint64_t *)(uintptr_t)(t_this + 0x18);
        float hi = *(const float *)(uintptr_t)(q + 0x90);
        return (val >= 0.0f && val < hi) ? 1u : 0u;
    }
    c->sp -= 0x20;
    uint64_t buf = c->sp;
    uint32_t tbits, nbits[4];
    memcpy(&tbits, &val, 4);
    memcpy(&nbits[0], &nx, 4);
    memcpy(&nbits[1], &ny, 4);
    memcpy(&nbits[2], &nz, 4);
    nbits[3] = 0;
    *(uint32_t *)(uintptr_t)buf = tbits;
    memcpy((void *)(uintptr_t)(buf + 0x10), nbits, 16);
    c->x[0] = t_this;
    c->x[1] = buf;
    c->x[2] = buf + 0x10;
    c->pc = t_fn;
    c->x[30] = 0;
    rt_dispatch(c);
    c->sp += 0x20;
    return (uint32_t)(c->x[0] & 1u);
}

static inline void col_closest_on_seg(float out[3], const float *a, const float *b, const float *p) {
    float ex = b[0] - a[0], ey = b[1] - a[1], ez = b[2] - a[2];
    float apx = p[0] - a[0], apy = p[1] - a[1], apz = p[2] - a[2];
    float ee = fmaf(ez, ez, ex * ex) + ey * ey;
    float ae = fmaf(apz, ez, apx * ex) + apy * ey;
    float s = (ee > 0.0f) ? (ae / ee) : 0.0f;
    if (s < 0.0f || !(s == s)) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    out[0] = ex * s + a[0];
    out[1] = ey * s + a[1];
    out[2] = ez * s + a[2];
}

static inline void col_mark_bit(uint64_t filter, uint64_t tbl_off) {
    uint64_t tbl = *(const uint64_t *)(uintptr_t)(filter + tbl_off);
    if (tbl != 0) {
        uint32_t bit = *(const uint32_t *)(uintptr_t)(filter + 0xf0);
        uint32_t *w = (uint32_t *)(uintptr_t)(tbl + ((uint64_t)(bit >> 5) << 2));
        *w |= (1u << (bit & 31u));
    }
}

static inline int col_invoke_hit(Cpu *c, uint64_t self, uint64_t poly, uint64_t mat,
                                 float dist, const float pos[3], const float nrm[3], uint64_t bitset_off) {
    uint64_t hptr = *(const uint64_t *)(uintptr_t)(self + 56);
    int64_t hadj = *(const int64_t *)(uintptr_t)(self + 64);
    if (((hadj & 1) | hptr) != 0) {
        if (bitset_off != 0) {
            col_mark_bit(*(const uint64_t *)(uintptr_t)(self + 0x08), bitset_off);
        }
        uint64_t hit_this = 0;
        uint64_t hit_fn = resolve_pmf(self, 56, &hit_this);
        c->sp -= 0x40;
        uint64_t buf = c->sp;
        float hp[4] = {pos[0], pos[1], pos[2], 0.0f};
        float hn[4] = {nrm[0], nrm[1], nrm[2], 0.0f};
        memcpy((void *)(uintptr_t)buf, hp, 16);
        memcpy((void *)(uintptr_t)(buf + 0x10), hn, 16);
        *(uint64_t *)(uintptr_t)(buf + 0x30) = mat;
        uint32_t dbits = 0;
        memcpy(&dbits, &dist, 4);
        c->v[0] = dbits;
        c->x[0] = hit_this;
        c->x[1] = buf;
        c->x[2] = poly;
        c->pc = hit_fn;
        c->x[30] = 0;
        rt_dispatch(c);
        c->sp += 0x40;
        return 0;
    } else {
        uint64_t fobj = *(const uint64_t *)(uintptr_t)(self + 0x08);
        uint64_t cb_obj = *(const uint64_t *)(uintptr_t)(fobj + 0x110);
        if (cb_obj != 0) {
            uint64_t vt = *(const uint64_t *)(uintptr_t)cb_obj;
            uint64_t fn = *(const uint64_t *)(uintptr_t)vt;
            c->x[0] = cb_obj;
            c->x[1] = *(const uint64_t *)(uintptr_t)(fobj + 0x100);
            c->pc = fn;
            c->x[30] = 0;
            rt_dispatch(c);
        }
        return 1;
    }
}

static uint32_t col_ray_poly(Cpu *c, uint64_t self, uint32_t poly_idx) {
    uint64_t mesh = *(const uint64_t *)(uintptr_t)self;
    uint64_t poly = *(const uint64_t *)(uintptr_t)(mesh + 0x10) + (uint64_t)poly_idx * 12u;
    uint16_t mat_idx = *(const uint16_t *)(uintptr_t)(poly + 0x0a);
    uint64_t mat = *(const uint64_t *)(uintptr_t)(mesh + 0x40) + (uint64_t)mat_idx * 0x28u;
    uint32_t mat_flags = *(const uint32_t *)(uintptr_t)(mat + 0x04);
    uint64_t filter = *(const uint64_t *)(uintptr_t)(self + 0x08);
    if ((*(const uint32_t *)(uintptr_t)(filter + 0xf8) & mat_flags) != 0) return 0u;
    if ((*(const uint32_t *)(uintptr_t)(filter + 0xfc) & mat_flags) == 0) return 0u;

    uint32_t num_tris = *(const uint8_t *)(uintptr_t)(poly + 0x04);
    if (num_tris == 0) return 0u;

    uint64_t vtx_tbl = *(const uint64_t *)(uintptr_t)(mesh + 0x18);
    uint64_t nrm_tbl = *(const uint64_t *)(uintptr_t)(mesh + 0x20);
    uint64_t vtx_base = *(const uint64_t *)(uintptr_t)(vtx_tbl + ((uint64_t)*(const uint8_t *)(uintptr_t)(poly + 5) << 3));
    uint64_t nrm_base = *(const uint64_t *)(uintptr_t)(nrm_tbl + ((uint64_t)*(const uint8_t *)(uintptr_t)(poly + 6) << 3));

    uint64_t idx_ptr = *(const uint64_t *)(uintptr_t)mesh + *(const uint32_t *)(uintptr_t)poly;
    uint16_t v0_idx = *(const uint16_t *)(uintptr_t)(idx_ptr + ((uint64_t)num_tris << 1));
    const float *v0 = (const float *)(uintptr_t)(vtx_base + ((uint64_t)v0_idx << 4));
    uint64_t v_off = ((uint64_t)num_tris << 1) + 2u;

    float norm_scale = *(const float *)(uintptr_t)0x70009d3f7cull;
    float edge_eps_sq = *(const float *)(uintptr_t)0x70009d3aa8ull;

    // [XCDE] Wave 12: hoist query pointer and PMF-88 resolution out of the per-triangle loop.
    uint64_t q = *(const uint64_t *)(uintptr_t)(self + 0x18);
    const float *orig = (const float *)(uintptr_t)(q + 0x20);
    const float *dir = (const float *)(uintptr_t)(q + 0x30);
    float o0 = orig[0], o1 = orig[1], o2 = orig[2];
    float d0 = dir[0], d1 = dir[1], d2 = dir[2];
    uint64_t t_this = 0;
    uint64_t t_fn = resolve_pmf(self, 88, &t_this);
    const bool t_is_01 = (t_fn == ADDR_T_RANGE_01);
    const bool t_is_lohi = (t_fn == ADDR_T_RANGE_LOHI);
    const bool t_is_abs = (t_fn == ADDR_T_RANGE_ABS);
    const bool t_is_0hi = (t_fn == ADDR_T_RANGE_0HI);
    float t_lo = 0.0f, t_hi = 1.0f;
    if (t_is_lohi) {
        uint64_t tq = *(const uint64_t *)(uintptr_t)(t_this + 0x18);
        t_lo = *(const float *)(uintptr_t)(tq + 0x94);
        t_hi = *(const float *)(uintptr_t)(tq + 0x90);
    } else if (t_is_abs || t_is_0hi) {
        uint64_t tq = *(const uint64_t *)(uintptr_t)(t_this + 0x18);
        t_hi = *(const float *)(uintptr_t)(tq + 0x90);
    }

    for (uint32_t i = 0; i < num_tris; i++, idx_ptr += 2u) {
        uint16_t n_idx = *(const uint16_t *)(uintptr_t)idx_ptr;
        const int16_t *n16 = (const int16_t *)(uintptr_t)(nrm_base + (uint64_t)n_idx * 6u);
        float nx = (float)n16[0] * norm_scale;
        float ny = (float)n16[1] * norm_scale;
        float nz = (float)n16[2] * norm_scale;

        float denom = fmaf(d2, nz, d0 * nx) + d1 * ny;
        if (denom == 0.0f) continue;

        float d_v0 = fmaf(v0[2], nz, v0[0] * nx) + v0[1] * ny;
        float d_orig = fmaf(o2, nz, o0 * nx) + o1 * ny;
        float t = (d_v0 - d_orig) / denom;
        if (__builtin_expect(t_is_01, 1)) {
            if (!(t >= 0.0f && t <= 1.0f)) continue;
        } else if (t_is_lohi) {
            if (!(t >= t_lo && t <= t_hi)) continue;
        } else if (t_is_abs) {
            if (fabsf(t) > t_hi) continue;
        } else if (t_is_0hi) {
            if (!(t >= 0.0f && t <= t_hi)) continue;
        } else {
            if (!col_check_pmf(c, self, 88, t, nx, ny, nz)) continue;
            q = *(const uint64_t *)(uintptr_t)(self + 0x18);
            orig = (const float *)(uintptr_t)(q + 0x20);
            dir = (const float *)(uintptr_t)(q + 0x30);
            o0 = orig[0]; o1 = orig[1]; o2 = orig[2];
            d0 = dir[0]; d1 = dir[1]; d2 = dir[2];
        }

        float px = o0 + d0 * t;
        float py = o1 + d1 * t;
        float pz = o2 + d2 * t;

        uint16_t v1_idx = *(const uint16_t *)(uintptr_t)(idx_ptr + v_off);
        uint16_t v2_idx = *(const uint16_t *)(uintptr_t)(idx_ptr + v_off + 2u);
        const float *v1 = (const float *)(uintptr_t)(vtx_base + ((uint64_t)v1_idx << 4));
        const float *v2 = (const float *)(uintptr_t)(vtx_base + ((uint64_t)v2_idx << 4));

        float ax = v0[0] - px, ay = v0[1] - py, az = v0[2] - pz;
        float bx = v1[0] - px, by = v1[1] - py, bz = v1[2] - pz;
        float cx = v2[0] - px, cy = v2[1] - py, cz = v2[2] - pz;

        float ux = fmaf(-bz, cy, cz * by), uy = fmaf(-bx, cz, cx * bz), uz = fmaf(-by, cx, cy * bx);
        float vx = fmaf(-cz, ay, az * cy), vy = fmaf(-cx, az, ax * cz), vz = fmaf(-cy, ax, ay * cx);

        int hit = 0;
        if ((fmaf(vz, uz, vx * ux) + vy * uy) >= 0.0f) {
            float wx = fmaf(-az, by, bz * ay), wy = fmaf(-ax, bz, bx * az), wz = fmaf(-ay, bx, by * ax);
            if ((fmaf(wz, uz, wx * ux) + wy * uy) >= 0.0f) {
                hit = 1;
            }
        }
        if (!hit) {
            if (col_edge_hit(px, py, pz, v0, v1, edge_eps_sq) ||
                col_edge_hit(px, py, pz, v1, v2, edge_eps_sq) ||
                col_edge_hit(px, py, pz, v2, v0, edge_eps_sq)) {
                hit = 1;
            }
        }
        if (!hit) continue;

        float hp[3] = {px, py, pz};
        float hn[3] = {nx, ny, nz};
        float dist = (*(const float *)(uintptr_t)(q + 0x98)) * t;
        col_invoke_hit(c, self, poly, mat, dist, hp, hn, 0);
        return 1u;
    }
    return 0u;
}

static uint32_t col_sweep_poly(Cpu *c, uint64_t self, uint32_t poly_idx) {
    uint64_t mesh = *(const uint64_t *)(uintptr_t)self;
    uint64_t poly = *(const uint64_t *)(uintptr_t)(mesh + 0x10) + (uint64_t)poly_idx * 12u;
    uint16_t mat_idx = *(const uint16_t *)(uintptr_t)(poly + 0x0a);
    uint64_t mat = *(const uint64_t *)(uintptr_t)(mesh + 0x40) + (uint64_t)mat_idx * 0x28u;
    uint32_t mat_flags = *(const uint32_t *)(uintptr_t)(mat + 0x04);
    uint64_t filter = *(const uint64_t *)(uintptr_t)(self + 0x08);
    if ((*(const uint32_t *)(uintptr_t)(filter + 0xf8) & mat_flags) != 0) return 0u;
    if ((*(const uint32_t *)(uintptr_t)(filter + 0xfc) & mat_flags) == 0) return 0u;

    uint32_t num_tris = *(const uint8_t *)(uintptr_t)(poly + 0x04);
    uint64_t vtx_tbl = *(const uint64_t *)(uintptr_t)(mesh + 0x18);
    uint64_t vtx_base = *(const uint64_t *)(uintptr_t)(vtx_tbl + ((uint64_t)*(const uint8_t *)(uintptr_t)(poly + 5) << 3));
    uint64_t idx_ptr = *(const uint64_t *)(uintptr_t)mesh + *(const uint32_t *)(uintptr_t)poly;
    uint64_t vtx_ptr = idx_ptr + ((uint64_t)num_tris << 1);

    float norm_scale = *(const float *)(uintptr_t)0x70009d3f7cull;
    float edge_eps_sq = *(const float *)(uintptr_t)0x70009d3aa8ull;

    uint16_t v0_idx = *(const uint16_t *)(uintptr_t)vtx_ptr;
    const float *v0 = (const float *)(uintptr_t)(vtx_base + ((uint64_t)v0_idx << 4));

    uint32_t any_hit = 0;
    uint32_t last_flag = 0;

    if (num_tris != 0) {
        uint64_t nrm_tbl = *(const uint64_t *)(uintptr_t)(mesh + 0x20);
        uint64_t nrm_base = *(const uint64_t *)(uintptr_t)(nrm_tbl + ((uint64_t)*(const uint8_t *)(uintptr_t)(poly + 6) << 3));

        // [XCDE] Wave 13: hoist query pointer and PMF-88 / PMF-104 resolution out of per-triangle loop.
        uint64_t q = *(const uint64_t *)(uintptr_t)(self + 0x18);
        const float *orig70 = (const float *)(uintptr_t)(q + 0x70);
        const float *dir = (const float *)(uintptr_t)(q + 0x30);
        const float *center = (const float *)(uintptr_t)(q + 0x20);

        uint64_t t88_this = 0, t104_this = 0;
        uint64_t t88_fn = resolve_pmf(self, 88, &t88_this);
        uint64_t t104_fn = resolve_pmf(self, 104, &t104_this);
        const int k88 = (t88_fn == ADDR_T_RANGE_01) ? 1 : (t88_fn == ADDR_T_RANGE_LOHI) ? 2
                      : (t88_fn == ADDR_T_RANGE_ABS) ? 3 : (t88_fn == ADDR_T_RANGE_0HI) ? 4 : 0;
        const int k104 = (t104_fn == ADDR_T_RANGE_01) ? 1 : (t104_fn == ADDR_T_RANGE_LOHI) ? 2
                       : (t104_fn == ADDR_T_RANGE_ABS) ? 3 : (t104_fn == ADDR_T_RANGE_0HI) ? 4 : 0;
        float t88_lo = 0.0f, t88_hi = 1.0f, t104_lo = 0.0f, t104_hi = 1.0f;
        if (k88 >= 2) {
            uint64_t tq = *(const uint64_t *)(uintptr_t)(t88_this + 0x18);
            t88_hi = *(const float *)(uintptr_t)(tq + 0x90);
            if (k88 == 2) t88_lo = *(const float *)(uintptr_t)(tq + 0x94);
        }
        if (k104 >= 2) {
            uint64_t tq = *(const uint64_t *)(uintptr_t)(t104_this + 0x18);
            t104_hi = *(const float *)(uintptr_t)(tq + 0x90);
            if (k104 == 2) t104_lo = *(const float *)(uintptr_t)(tq + 0x94);
        }

        for (uint32_t i = 0; i < num_tris; i++, idx_ptr += 2u) {
            uint16_t n_idx = *(const uint16_t *)(uintptr_t)idx_ptr;
            const int16_t *n16 = (const int16_t *)(uintptr_t)(nrm_base + (uint64_t)n_idx * 6u);
            float nx = (float)n16[0] * norm_scale;
            float ny = (float)n16[1] * norm_scale;
            float nz = (float)n16[2] * norm_scale;

            uint16_t v1_idx = *(const uint16_t *)(uintptr_t)(vtx_ptr + ((uint64_t)(i + 1u) << 1));
            uint16_t v2_idx = *(const uint16_t *)(uintptr_t)(vtx_ptr + ((uint64_t)(i + 2u) << 1));
            const float *v1 = (const float *)(uintptr_t)(vtx_base + ((uint64_t)v1_idx << 4));
            const float *v2 = (const float *)(uintptr_t)(vtx_base + ((uint64_t)v2_idx << 4));

            float d_v0 = fmaf(v0[2], nz, v0[0] * nx) + v0[1] * ny;

            // Part 1: ray from q + 0x70, dir q + 0x30
            float denom = fmaf(dir[2], nz, dir[0] * nx) + dir[1] * ny;
            if (denom != 0.0f) {
                float d_o70 = fmaf(orig70[2], nz, orig70[0] * nx) + orig70[1] * ny;
                float t = (d_v0 - d_o70) / denom;
                int ok1;
                if (__builtin_expect(k88 == 1, 1)) ok1 = (t >= 0.0f && t <= 1.0f);
                else if (k88 == 2) ok1 = (t >= t88_lo && t <= t88_hi);
                else if (k88 == 3) ok1 = !(fabsf(t) > t88_hi);
                else if (k88 == 4) ok1 = (t >= 0.0f && t <= t88_hi);
                else {
                    ok1 = (int)col_check_pmf(c, self, 88, t, nx, ny, nz);
                    q = *(const uint64_t *)(uintptr_t)(self + 0x18);
                    orig70 = (const float *)(uintptr_t)(q + 0x70);
                    dir = (const float *)(uintptr_t)(q + 0x30);
                    center = (const float *)(uintptr_t)(q + 0x20);
                }
                if (ok1) {
                    float px = orig70[0] + dir[0] * t;
                    float py = orig70[1] + dir[1] * t;
                    float pz = orig70[2] + dir[2] * t;

                    float ax = v0[0] - px, ay = v0[1] - py, az = v0[2] - pz;
                    float bx = v1[0] - px, by = v1[1] - py, bz = v1[2] - pz;
                    float cx = v2[0] - px, cy = v2[1] - py, cz = v2[2] - pz;
                    float ux = fmaf(-bz, cy, cz * by), uy = fmaf(-bx, cz, cx * bz), uz = fmaf(-by, cx, cy * bx);
                    float vx = fmaf(-cz, ay, az * cy), vy = fmaf(-cx, az, ax * cz), vz = fmaf(-cy, ax, ay * cx);

                    int hit1 = 0;
                    if ((fmaf(vz, uz, vx * ux) + vy * uy) >= 0.0f) {
                        float wx = fmaf(-az, by, bz * ay), wy = fmaf(-ax, bz, bx * az), wz = fmaf(-ay, bx, by * ax);
                        if ((fmaf(wz, uz, wx * ux) + wy * uy) >= 0.0f) hit1 = 1;
                    }
                    if (!hit1) {
                        if (col_edge_hit(px, py, pz, v0, v1, edge_eps_sq) ||
                            col_edge_hit(px, py, pz, v1, v2, edge_eps_sq) ||
                            col_edge_hit(px, py, pz, v2, v0, edge_eps_sq)) {
                            hit1 = 1;
                        }
                    }
                    if (hit1) {
                        float hp1[3] = {px, py, pz};
                        float hn1[3] = {nx, ny, nz};
                        float dist1 = (*(const float *)(uintptr_t)(q + 0x98)) * t;
                        col_invoke_hit(c, self, poly, mat, dist1, hp1, hn1, 0xe0);
                        return 1u;
                    }
                }
            }

            // Part 2: sphere at q + 0x20, radius^2 at q + 0x94
            float d_c = fmaf(center[2], nz, center[0] * nx) + center[1] * ny;
            float d = d_c - d_v0;
            int ok2;
            if (k104 == 3) ok2 = !(fabsf(d) > t104_hi);
            else if (k104 == 4) ok2 = (d >= 0.0f && d <= t104_hi);
            else if (k104 == 2) ok2 = (d >= t104_lo && d <= t104_hi);
            else if (k104 == 1) ok2 = (d >= 0.0f && d <= 1.0f);
            else {
                ok2 = (int)col_check_pmf(c, self, 104, d, nx, ny, nz);
                q = *(const uint64_t *)(uintptr_t)(self + 0x18);
                orig70 = (const float *)(uintptr_t)(q + 0x70);
                dir = (const float *)(uintptr_t)(q + 0x30);
                center = (const float *)(uintptr_t)(q + 0x20);
            }
            if (!ok2) {
                last_flag = 1;
                continue;
            }

            float px = center[0] - nx * d;
            float py = center[1] - ny * d;
            float pz = center[2] - nz * d;

            float ax = v0[0] - px, ay = v0[1] - py, az = v0[2] - pz;
            float bx = v1[0] - px, by = v1[1] - py, bz = v1[2] - pz;
            float cx = v2[0] - px, cy = v2[1] - py, cz = v2[2] - pz;
            float ux = fmaf(-bz, cy, cz * by), uy = fmaf(-bx, cz, cx * bz), uz = fmaf(-by, cx, cy * bx);
            float vx = fmaf(-cz, ay, az * cy), vy = fmaf(-cx, az, ax * cz), vz = fmaf(-cy, ax, ay * cx);

            if ((fmaf(vz, uz, vx * ux) + vy * uy) >= 0.0f) {
                float wx = fmaf(-az, by, bz * ay), wy = fmaf(-ax, bz, bx * az), wz = fmaf(-ay, bx, by * ax);
                if ((fmaf(wz, uz, wx * ux) + wy * uy) >= 0.0f) {
                    float hp2[3] = {px, py, pz};
                    float hn2[3] = {nx, ny, nz};
                    if (col_invoke_hit(c, self, poly, mat, fabsf(d), hp2, hn2, 0)) {
                        return 1u;
                    }
                    any_hit = 1;
                    last_flag = 1;
                    continue;
                }
            }

            float cp[3];
            col_closest_on_seg(cp, v1, v2, center);
            float dx = center[0] - cp[0], dy = center[1] - cp[1], dz = center[2] - cp[2];
            float dsq = fmaf(dz, dz, dx * dx) + dy * dy;
            float rad_sq = *(const float *)(uintptr_t)(q + 0x94);
            if (!(dsq < rad_sq)) {
                last_flag = 0;
                continue;
            }

            float hn[3] = {nx, ny, nz};
            float dist = 0.0f;
            if (!(dsq < edge_eps_sq)) {
                dist = sqrtf(dsq);
                float inv = 1.0f / dist;
                hn[0] = dx * inv;
                hn[1] = dy * inv;
                hn[2] = dz * inv;
            }
            if (col_invoke_hit(c, self, poly, mat, dist, cp, hn, 0xd8)) {
                return 1u;
            }

            q = *(const uint64_t *)(uintptr_t)(self + 0x18);
            center = (const float *)(uintptr_t)(q + 0x20);
            rad_sq = *(const float *)(uintptr_t)(q + 0x94);
            float cp2[3];
            col_closest_on_seg(cp2, v0, v1, center);
            float dx2 = center[0] - cp2[0], dy2 = center[1] - cp2[1], dz2 = center[2] - cp2[2];
            float dsq2 = fmaf(dz2, dz2, dx2 * dx2) + dy2 * dy2;
            if (dsq2 < rad_sq) {
                float hn2[3] = {hn[0], hn[1], hn[2]};
                float dist2 = 0.0f;
                if (!(dsq2 < edge_eps_sq)) {
                    dist2 = sqrtf(dsq2);
                    float inv2 = 1.0f / dist2;
                    hn2[0] = dx2 * inv2;
                    hn2[1] = dy2 * inv2;
                    hn2[2] = dz2 * inv2;
                }
                if (col_invoke_hit(c, self, poly, mat, dist2, cp2, hn2, 0xd8)) {
                    return 1u;
                }
            }
            any_hit = 1;
            last_flag = 1;
        }
    }

    // Part 3: closing edge (v0, v_last) when num_tris == 0 || last_flag == 0
    if (!last_flag) {
        uint16_t v_last_idx = *(const uint16_t *)(uintptr_t)(vtx_ptr + ((uint64_t)num_tris << 1) + 2u);
        uint16_t v1_idx = *(const uint16_t *)(uintptr_t)(vtx_ptr + 2u);
        if (v_last_idx != v1_idx) {
            const float *v_last = (const float *)(uintptr_t)(vtx_base + ((uint64_t)v_last_idx << 4));
            uint64_t q = *(const uint64_t *)(uintptr_t)(self + 0x18);
            const float *center = (const float *)(uintptr_t)(q + 0x20);
            float cp3[3];
            col_closest_on_seg(cp3, v0, v_last, center);
            float dx3 = center[0] - cp3[0], dy3 = center[1] - cp3[1], dz3 = center[2] - cp3[2];
            float dsq3 = fmaf(dz3, dz3, dx3 * dx3) + dy3 * dy3;
            if (dsq3 < *(const float *)(uintptr_t)(q + 0x94)) {
                float dist3 = sqrtf(dsq3);
                float hn3[3];
                if (dist3 < edge_eps_sq) {
                    const float *def_n = (const float *)(uintptr_t)0x70009cfc98ull;
                    hn3[0] = def_n[0];
                    hn3[1] = def_n[1];
                    hn3[2] = 0.0f;
                } else {
                    float inv3 = 1.0f / dist3;
                    hn3[0] = dx3 * inv3;
                    hn3[1] = dy3 * inv3;
                    hn3[2] = dz3 * inv3;
                }
                col_invoke_hit(c, self, poly, mat, dist3, cp3, hn3, 0xd8);
                any_hit = 1;
            }
        }
    }

    return any_hit & 1u;
}

static inline float clamp01_col(float x) {
    float r = fmaxf(x, 0.0f);
    return fminf(r, 1.0f);
}

static void col_seg_seg_native(float out1[3], float out2[3],
                               const float *p1, const float *q1,
                               const float *p2, const float *q2) {
    float d1x = q1[0] - p1[0], d1y = q1[1] - p1[1], d1z = q1[2] - p1[2];
    float d2x = q2[0] - p2[0], d2y = q2[1] - p2[1], d2z = q2[2] - p2[2];
    float nd2x = p2[0] - q2[0], nd2y = p2[1] - q2[1], nd2z = -d2z;
    float rx = p2[0] - p1[0], ry = p2[1] - p1[1], rz = p2[2] - p1[2];

    float eps = *(const float *)(uintptr_t)0x70009d3aa8ull;
    float a = fmaf(d1z, d1z, d1x * d1x) + d1y * d1y;
    float e = fmaf(d2z, d2z, d2x * d2x) + d2y * d2y;
    float neg_b = fmaf(nd2z, d1z, d1x * nd2x) + d1y * nd2y;
    float denom_b = neg_b * neg_b;
    float denom_ae = e * a;

    if (fabsf(denom_ae - denom_b) <= eps) {
        float inv_a = 1.0f / a;
        float t_p2 = inv_a * (fmaf(rz, d1z, rx * d1x) + ry * d1y);
        if (t_p2 >= 0.0f && t_p2 <= 1.0f) {
            out1[0] = p1[0] + d1x * t_p2;
            out1[1] = p1[1] + d1y * t_p2;
            out1[2] = p1[2] + d1z * t_p2;
            out2[0] = p2[0]; out2[1] = p2[1]; out2[2] = p2[2];
            return;
        }
        float inv_e = 1.0f / e;
        float s_p1 = inv_e * (fmaf(-rx, d2x, (-rz) * d2z) + (-ry) * d2y);
        if (s_p1 >= 0.0f && s_p1 <= 1.0f) {
            out2[0] = p2[0] + d2x * s_p1;
            out2[1] = p2[1] + d2y * s_p1;
            out2[2] = p2[2] + d2z * s_p1;
            out1[0] = p1[0]; out1[1] = p1[1]; out1[2] = p1[2];
            return;
        }
        float q2rx = q2[0] - p1[0], q2ry = q2[1] - p1[1], q2rz = q2[2] - p1[2];
        float t_q2 = inv_a * (fmaf(q2rz, d1z, q2rx * d1x) + q2ry * d1y);
        if (t_q2 >= 0.0f && t_q2 <= 1.0f) {
            out1[0] = p1[0] + d1x * t_q2;
            out1[1] = p1[1] + d1y * t_q2;
            out1[2] = p1[2] + d1z * t_q2;
            out2[0] = q2[0]; out2[1] = q2[1]; out2[2] = q2[2];
            return;
        }
        float q1rx = q1[0] - p2[0], q1ry = q1[1] - p2[1], q1rz = q1[2] - p2[2];
        float s_q1 = inv_e * (fmaf(q1rz, d2z, q1rx * d2x) + q1ry * d2y);
        if (s_q1 >= 0.0f && s_q1 <= 1.0f) {
            out2[0] = p2[0] + d2x * s_q1;
            out2[1] = p2[1] + d2y * s_q1;
            out2[2] = p2[2] + d2z * s_q1;
            out1[0] = q1[0]; out1[1] = q1[1]; out1[2] = q1[2];
            return;
        }
        float v18x = (t_p2 < 0.0f) ? 0.0f : d1x;
        float v18y = (t_p2 < 0.0f) ? 0.0f : d1y;
        float v18z = (t_p2 < 0.0f) ? 0.0f : d1z;
        float v4x  = (s_p1 < 0.0f) ? 0.0f : d2x;
        float v4y  = (s_p1 < 0.0f) ? 0.0f : d2y;
        float v4z  = (s_p1 < 0.0f) ? 0.0f : d2z;
        out1[0] = p1[0] + v18x; out1[1] = p1[1] + v18y; out1[2] = p1[2] + v18z;
        out2[0] = p2[0] + v4x;  out2[1] = p2[1] + v4y;  out2[2] = p2[2] + v4z;
        return;
    }

    float dot_r_nd2 = fmaf(nd2z, rz, rx * nd2x) + ry * nd2y;
    float dot_r_d1 = fmaf(rz, d1z, rx * d1x) + ry * d1y;
    float s_num = fmaf(-neg_b, dot_r_d1, a * dot_r_nd2);
    float s = s_num / (denom_ae - denom_b);
    float t = fmaf(-s, neg_b, dot_r_d1) / a;

    int s_oob = !(s >= 0.0f && s <= 1.0f);
    if (!(t >= 0.0f && t <= 1.0f)) {
        float tc = clamp01_col(t);
        float c1x = p1[0] + d1x * tc, c1y = p1[1] + d1y * tc, c1z = p1[2] + d1z * tc;
        out1[0] = c1x; out1[1] = c1y; out1[2] = c1z;
        float dx = c1x - p2[0], dy = c1y - p2[1], dz = c1z - p2[2];
        float u = (fmaf(dz, d2z, dx * d2x) + dy * d2y) / e;
        if (!s_oob || (u >= 0.0f && u <= 1.0f)) {
            float uc = clamp01_col(u);
            out2[0] = p2[0] + d2x * uc;
            out2[1] = p2[1] + d2y * uc;
            out2[2] = p2[2] + d2z * uc;
            return;
        }
        float uc = clamp01_col(u);
        float c2x = p2[0] + d2x * uc, c2y = p2[1] + d2y * uc, c2z = p2[2] + d2z * uc;
        out2[0] = c2x; out2[1] = c2y; out2[2] = c2z;
        float t2 = clamp01_col((fmaf(c2z - p1[2], d1z, (c2x - p1[0]) * d1x) + (c2y - p1[1]) * d1y) / a);
        c1x = p1[0] + d1x * t2; c1y = p1[1] + d1y * t2; c1z = p1[2] + d1z * t2;
        out1[0] = c1x; out1[1] = c1y; out1[2] = c1z;
        float u2 = clamp01_col((fmaf(c1z - p2[2], d2z, (c1x - p2[0]) * d2x) + (c1y - p2[1]) * d2y) / e);
        out2[0] = p2[0] + d2x * u2;
        out2[1] = p2[1] + d2y * u2;
        out2[2] = p2[2] + d2z * u2;
        return;
    }

    if (!s_oob) {
        out1[0] = p1[0] + d1x * t;
        out1[1] = p1[1] + d1y * t;
        out1[2] = p1[2] + d1z * t;
        out2[0] = p2[0] + d2x * s;
        out2[1] = p2[1] + d2y * s;
        out2[2] = p2[2] + d2z * s;
        return;
    }

    float sc = clamp01_col(s);
    float c2x = p2[0] + d2x * sc, c2y = p2[1] + d2y * sc, c2z = p2[2] + d2z * sc;
    out2[0] = c2x; out2[1] = c2y; out2[2] = c2z;
    float t2 = clamp01_col((fmaf(c2z - p1[2], d1z, (c2x - p1[0]) * d1x) + (c2y - p1[1]) * d1y) / a);
    out1[0] = p1[0] + d1x * t2;
    out1[1] = p1[1] + d1y * t2;
    out1[2] = p1[2] + d1z * t2;
}

static inline void col_seg_seg(Cpu *c, float out_a[3], float out_b[3],
                               uint64_t s1a, uint64_t s1b, uint64_t s2a, uint64_t s2b) {
    (void)c;
    col_seg_seg_native(out_a, out_b,
                       (const float *)(uintptr_t)s1a, (const float *)(uintptr_t)s1b,
                       (const float *)(uintptr_t)s2a, (const float *)(uintptr_t)s2b);
}

void xcde_col_seg_seg(Cpu *c) {
    float out1[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float out2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    col_seg_seg_native(out1, out2,
                       (const float *)(uintptr_t)c->x[2], (const float *)(uintptr_t)c->x[3],
                       (const float *)(uintptr_t)c->x[4], (const float *)(uintptr_t)c->x[5]);
    memcpy((void *)(uintptr_t)c->x[0], out1, 16);
    memcpy((void *)(uintptr_t)c->x[1], out2, 16);
}

static uint32_t col_capsule_poly(Cpu *c, uint64_t self, uint32_t poly_idx) {
    uint64_t mesh = *(const uint64_t *)(uintptr_t)self;
    uint64_t poly = *(const uint64_t *)(uintptr_t)(mesh + 0x10) + (uint64_t)poly_idx * 12u;
    uint16_t mat_idx = *(const uint16_t *)(uintptr_t)(poly + 0x0a);
    uint64_t mat = *(const uint64_t *)(uintptr_t)(mesh + 0x40) + (uint64_t)mat_idx * 0x28u;
    uint32_t mat_flags = *(const uint32_t *)(uintptr_t)(mat + 0x04);
    uint64_t filter = *(const uint64_t *)(uintptr_t)(self + 0x08);
    if ((*(const uint32_t *)(uintptr_t)(filter + 0xf8) & mat_flags) != 0) return 0u;
    if ((*(const uint32_t *)(uintptr_t)(filter + 0xfc) & mat_flags) == 0) return 0u;

    uint32_t num_tris = *(const uint8_t *)(uintptr_t)(poly + 0x04);
    if (num_tris == 0) return 0u;

    uint64_t vtx_tbl = *(const uint64_t *)(uintptr_t)(mesh + 0x18);
    uint64_t nrm_tbl = *(const uint64_t *)(uintptr_t)(mesh + 0x20);
    uint64_t vtx_base = *(const uint64_t *)(uintptr_t)(vtx_tbl + ((uint64_t)*(const uint8_t *)(uintptr_t)(poly + 5) << 3));
    uint64_t nrm_base = *(const uint64_t *)(uintptr_t)(nrm_tbl + ((uint64_t)*(const uint8_t *)(uintptr_t)(poly + 6) << 3));

    uint64_t idx_ptr = *(const uint64_t *)(uintptr_t)mesh + *(const uint32_t *)(uintptr_t)poly;
    uint64_t vtx_ptr = idx_ptr + ((uint64_t)num_tris << 1);

    float norm_scale = *(const float *)(uintptr_t)0x70009d3f7cull;
    uint16_t v0_idx = *(const uint16_t *)(uintptr_t)vtx_ptr;
    uint64_t v0_addr = vtx_base + ((uint64_t)v0_idx << 4);
    const float *v0 = (const float *)(uintptr_t)v0_addr;

    uint32_t any_hit = 0;
    uint32_t last_flag = 0;
    uint32_t need_closing = 0;
    float last_nrm[3] = {0.0f, 0.0f, 0.0f};

    for (uint32_t i = 0; i < num_tris; i++, idx_ptr += 2u) {
        uint16_t n_idx = *(const uint16_t *)(uintptr_t)idx_ptr;
        const int16_t *n16 = (const int16_t *)(uintptr_t)(nrm_base + (uint64_t)n_idx * 6u);
        float nx = (float)n16[0] * norm_scale;
        float ny = (float)n16[1] * norm_scale;
        float nz = (float)n16[2] * norm_scale;
        last_nrm[0] = nx;
        last_nrm[1] = ny;
        last_nrm[2] = nz;

        uint32_t prev_need_closing = need_closing;

        uint16_t v1_idx = *(const uint16_t *)(uintptr_t)(vtx_ptr + ((uint64_t)(i + 1u) << 1));
        uint16_t v2_idx = *(const uint16_t *)(uintptr_t)(vtx_ptr + ((uint64_t)(i + 2u) << 1));
        uint64_t v1_addr = vtx_base + ((uint64_t)v1_idx << 4);
        uint64_t v2_addr = vtx_base + ((uint64_t)v2_idx << 4);
        const float *v1 = (const float *)(uintptr_t)v1_addr;
        const float *v2 = (const float *)(uintptr_t)v2_addr;

        // Part 1: ray from q + 0x20, dir q + 0x30
        uint64_t q = *(const uint64_t *)(uintptr_t)(self + 0x18);
        const float *orig = (const float *)(uintptr_t)(q + 0x20);
        const float *dir = (const float *)(uintptr_t)(q + 0x30);
        float denom = fmaf(dir[2], nz, dir[0] * nx) + dir[1] * ny;
        if (denom == 0.0f) {
            need_closing = 0;
        } else {
            float d_v0 = fmaf(v0[2], nz, v0[0] * nx) + v0[1] * ny;
            float d_orig = fmaf(orig[2], nz, orig[0] * nx) + orig[1] * ny;
            float t = (d_v0 - d_orig) / denom;
            if (!col_check_pmf(c, self, 88, t, nx, ny, nz)) {
                need_closing = 0;
            } else {
                q = *(const uint64_t *)(uintptr_t)(self + 0x18);
                orig = (const float *)(uintptr_t)(q + 0x20);
                dir = (const float *)(uintptr_t)(q + 0x30);
                float px = orig[0] + dir[0] * t;
                float py = orig[1] + dir[1] * t;
                float pz = orig[2] + dir[2] * t;

                float ax = v0[0] - px, ay = v0[1] - py, az = v0[2] - pz;
                float bx = v1[0] - px, by = v1[1] - py, bz = v1[2] - pz;
                float cx = v2[0] - px, cy = v2[1] - py, cz = v2[2] - pz;
                float ux = fmaf(-bz, cy, cz * by), uy = fmaf(-bx, cz, cx * bz), uz = fmaf(-by, cx, cy * bx);
                float vx = fmaf(-cz, ay, az * cy), vy = fmaf(-cx, az, ax * cz), vz = fmaf(-cy, ax, ay * cx);

                if ((fmaf(vz, uz, vx * ux) + vy * uy) >= 0.0f) {
                    float wx = fmaf(-az, by, bz * ay), wy = fmaf(-ax, bz, bx * az), wz = fmaf(-ay, bx, by * ax);
                    if ((fmaf(wz, uz, wx * ux) + wy * uy) >= 0.0f) {
                        float hp1[3] = {px, py, pz};
                        float dist1 = (*(const float *)(uintptr_t)(q + 0x98)) * t;
                        col_invoke_hit(c, self, poly, mat, dist1, hp1, last_nrm, 0);
                        return 1u;
                    }
                }
                need_closing = 1;
            }
        }

        // Part 2: 2 capsule endpoints at q + 0x70 and q + 0x80
        float d_v0 = fmaf(v0[2], nz, v0[0] * nx) + v0[1] * ny;
        int hit_ep = 0;
        for (int ep = 0; ep < 2; ep++) {
            q = *(const uint64_t *)(uintptr_t)(self + 0x18);
            const float *ep_pos = (const float *)(uintptr_t)(q + 0x70 + (uint64_t)ep * 0x10u);
            float d = (fmaf(ep_pos[2], nz, ep_pos[0] * nx) + ep_pos[1] * ny) - d_v0;
            if (!col_check_pmf(c, self, 104, d, nx, ny, nz)) continue;

            q = *(const uint64_t *)(uintptr_t)(self + 0x18);
            ep_pos = (const float *)(uintptr_t)(q + 0x70 + (uint64_t)ep * 0x10u);
            float px = ep_pos[0] - nx * d;
            float py = ep_pos[1] - ny * d;
            float pz = ep_pos[2] - nz * d;

            float ax = v0[0] - px, ay = v0[1] - py, az = v0[2] - pz;
            float bx = v1[0] - px, by = v1[1] - py, bz = v1[2] - pz;
            float cx = v2[0] - px, cy = v2[1] - py, cz = v2[2] - pz;
            float ux = fmaf(-bz, cy, cz * by), uy = fmaf(-bx, cz, cx * bz), uz = fmaf(-by, cx, cy * bx);
            float vx = fmaf(-cz, ay, az * cy), vy = fmaf(-cx, az, ax * cz), vz = fmaf(-cy, ax, ay * cx);

            if ((fmaf(vz, uz, vx * ux) + vy * uy) >= 0.0f) {
                float wx = fmaf(-az, by, bz * ay), wy = fmaf(-ax, bz, bx * az), wz = fmaf(-ay, bx, by * ax);
                if ((fmaf(wz, uz, wx * ux) + wy * uy) >= 0.0f) {
                    float hp2[3] = {px, py, pz};
                    if (col_invoke_hit(c, self, poly, mat, fabsf(d), hp2, last_nrm, 0)) {
                        return 1u;
                    }
                    any_hit = 1;
                    last_flag = 1;
                    need_closing = 1;
                    hit_ep = 1;
                    break;
                }
            }
            need_closing = 1;
        }
        if (hit_ep) continue;

        // Part 3: capsule segment (q+0x70, q+0x80) vs edges (v1, v2) and (v0, v1)
        if (!need_closing) {
            need_closing = prev_need_closing | need_closing;
            last_flag = 0;
            continue;
        }

        q = *(const uint64_t *)(uintptr_t)(self + 0x18);
        float rad_sq = *(const float *)(uintptr_t)(q + 0x94);
        float out_a[3], out_b[3];
        col_seg_seg(c, out_a, out_b, q + 0x70, q + 0x80, v1_addr, v2_addr);
        float dx = out_a[0] - out_b[0], dy = out_a[1] - out_b[1], dz = out_a[2] - out_b[2];
        float dsq = fmaf(dz, dz, dx * dx) + dy * dy;
        if (dsq < rad_sq) {
            if (col_invoke_hit(c, self, poly, mat, sqrtf(dsq), out_b, last_nrm, 0xd8)) {
                return 1u;
            }
            any_hit = 1;
            last_flag = 1;
            need_closing = 1;
            continue;
        }

        q = *(const uint64_t *)(uintptr_t)(self + 0x18);
        rad_sq = *(const float *)(uintptr_t)(q + 0x94);
        col_seg_seg(c, out_a, out_b, q + 0x70, q + 0x80, v0_addr, v1_addr);
        float dx2 = out_a[0] - out_b[0], dy2 = out_a[1] - out_b[1], dz2 = out_a[2] - out_b[2];
        float dsq2 = fmaf(dz2, dz2, dx2 * dx2) + dy2 * dy2;
        if (dsq2 < rad_sq) {
            if (col_invoke_hit(c, self, poly, mat, sqrtf(dsq2), out_b, last_nrm, 0xd8)) {
                return 1u;
            }
            any_hit = 1;
            last_flag = 1;
            need_closing = 1;
            continue;
        }

        last_flag = 0;
        need_closing = 1;
    }

    // Part 4: closing edge (v0, v_last) when !last_flag && need_closing
    if (!last_flag && need_closing) {
        uint16_t v_last_idx = *(const uint16_t *)(uintptr_t)(vtx_ptr + ((uint64_t)num_tris << 1) + 2u);
        uint64_t v_last_addr = vtx_base + ((uint64_t)v_last_idx << 4);
        uint64_t q = *(const uint64_t *)(uintptr_t)(self + 0x18);
        float rad_sq = *(const float *)(uintptr_t)(q + 0x94);
        float out_a[3], out_b[3];
        col_seg_seg(c, out_a, out_b, q + 0x70, q + 0x80, v0_addr, v_last_addr);
        float dx3 = out_a[0] - out_b[0], dy3 = out_a[1] - out_b[1], dz3 = out_a[2] - out_b[2];
        float dsq3 = fmaf(dz3, dz3, dx3 * dx3) + dy3 * dy3;
        if (dsq3 < rad_sq) {
            if (col_invoke_hit(c, self, poly, mat, sqrtf(dsq3), out_b, last_nrm, 0xd8)) {
                return 1u;
            }
        }
    }

    return any_hit & 1u;
}

__attribute__((noinline, cold))
static void verify_trav_cold(const Cpu *c, uint64_t adj_this, uint64_t p_max, uint64_t p_min,
                             uint64_t addr, uint32_t rej, const char *name) {
    Cpu chk = *c;
    chk.x[0] = adj_this;
    chk.x[1] = p_max;
    chk.x[2] = p_min;
    verify_against_guest(&chk, addr, rej, name);
}

static uint32_t col_bvh_traverse_slow(Cpu *c, uint64_t self, uint32_t node_idx) {
    uint64_t mesh = *(const uint64_t *)(uintptr_t)self;
    uint64_t aabbs = *(const uint64_t *)(uintptr_t)(mesh + 0x60);
    uint64_t p_max = aabbs + ((uint64_t)node_idx << 5);
    uint64_t p_min = p_max + 0x10;

    uint64_t adj_this = 0;
    uint64_t target = resolve_pmf(self, 184, &adj_this);
    uint32_t rej;
    if (target == ADDR_SEG_AABB_REJECT) {
        rej = col_seg_aabb_reject(adj_this, p_max, p_min);
        if (__builtin_expect(g_verify == 1, 0))
            verify_trav_cold(c, adj_this, p_max, p_min, ADDR_SEG_AABB_REJECT, rej, "col_seg_aabb_reject(trav)");
    } else if (target == ADDR_AABB_REJECT) {
        rej = col_aabb_reject(adj_this, p_max, p_min);
        if (__builtin_expect(g_verify == 1, 0))
            verify_trav_cold(c, adj_this, p_max, p_min, ADDR_AABB_REJECT, rej, "col_aabb_reject(trav)");
    } else {
        c->x[0] = adj_this;
        c->x[1] = p_max;
        c->x[2] = p_min;
        c->pc = target;
        c->x[30] = 0;
        rt_dispatch(c);
        rej = (uint32_t)(c->x[0] & 1u);
    }
    if (rej & 1u) return 0u;

    uint64_t nodes = *(const uint64_t *)(uintptr_t)(mesh + 0x68);
    uint64_t node = nodes + (uint64_t)node_idx * 12u;
    uint64_t data_base = *(const uint64_t *)(uintptr_t)mesh;
    uint64_t cur = data_base + *(const uint32_t *)(uintptr_t)node;

    uint32_t hit = 0;

    uint32_t n1 = *(const uint8_t *)(uintptr_t)(node + 5);
    if (n1 != 0) {
        uint32_t idx16 = *(const uint16_t *)(uintptr_t)cur;
        cur += 2;
        uint64_t tbl1 = *(const uint64_t *)(uintptr_t)(mesh + 0x48);
        uint32_t base1 = *(const uint32_t *)(uintptr_t)(tbl1 + ((uint64_t)*(const uint8_t *)(uintptr_t)(node + 8) << 2));
        uint32_t prim = base1 + idx16;
        uint32_t any1 = 0;
        for (uint32_t i = 0; i < n1; i++, prim++) {
            uint64_t leaf_this = 0;
            uint64_t leaf_fn = resolve_pmf(self, 40, &leaf_this);
            if (leaf_fn == ADDR_RAY_POLY) {
                any1 |= col_ray_poly(c, leaf_this, prim);
            } else if (leaf_fn == ADDR_SWEEP_POLY) {
                any1 |= col_sweep_poly(c, leaf_this, prim);
            } else if (leaf_fn == ADDR_CAPSULE_POLY) {
                any1 |= col_capsule_poly(c, leaf_this, prim);
            } else {
                any1 |= (call_guest_1(c, leaf_fn, leaf_this, prim) & 1u);
            }
        }
        hit = any1 ? 1u : 0u;
    }

    uint32_t n2 = *(const uint8_t *)(uintptr_t)(node + 6);
    if (n2 != 0) {
        uint32_t idx16 = *(const uint16_t *)(uintptr_t)cur;
        cur += 2;
        uint64_t tbl2 = *(const uint64_t *)(uintptr_t)(mesh + 0x50);
        uint32_t base2 = *(const uint32_t *)(uintptr_t)(tbl2 + ((uint64_t)*(const uint8_t *)(uintptr_t)(node + 9) << 2));
        uint32_t child = base2 + idx16;
        for (uint32_t i = 0; i < n2; i++, child++) {
            uint64_t ch_this = 0;
            uint64_t ch_fn = resolve_pmf(self, 168, &ch_this);
            if (ch_fn == ADDR_BVH_TRAVERSE) {
                hit |= col_bvh_traverse_slow(c, ch_this, child);
            } else {
                hit |= call_guest_1(c, ch_fn, ch_this, child);
            }
        }
    }

    uint32_t n3 = *(const uint8_t *)(uintptr_t)(node + 4);
    if (n3 != 0) {
        uint32_t idx16 = *(const uint16_t *)(uintptr_t)cur;
        uint64_t tbl3 = *(const uint64_t *)(uintptr_t)(mesh + 0x58);
        uint32_t base3 = *(const uint32_t *)(uintptr_t)(tbl3 + ((uint64_t)*(const uint8_t *)(uintptr_t)(node + 7) << 2));
        uint32_t child = base3 + idx16;
        for (uint32_t i = 0; i < n3; i++, child++) {
            uint64_t ch_this = 0;
            uint64_t ch_fn = resolve_pmf(self, 152, &ch_this);
            if (ch_fn == ADDR_BVH_TRAVERSE) {
                hit |= col_bvh_traverse_slow(c, ch_this, child);
            } else {
                hit |= call_guest_1(c, ch_fn, ch_this, child);
            }
        }
    }

    return hit & 1u;
}

// [XCDE] Wave 13: iterative BVH traversal supporting arbitrary n2/n3 delegate mixes,
// with hoisted mesh pointers, root-resolved PMFs, and inlined AABB/segment rejection.
static uint32_t col_bvh_traverse(Cpu *c, uint64_t self, uint32_t root_idx) {
    uint64_t adj_this = 0, leaf_this = 0, ch2_this = 0, ch3_this = 0;
    uint64_t rej_fn = resolve_pmf(self, 184, &adj_this);
    uint64_t leaf_fn = resolve_pmf(self, 40, &leaf_this);
    uint64_t ch2_fn = resolve_pmf(self, 168, &ch2_this);
    uint64_t ch3_fn = resolve_pmf(self, 152, &ch3_this);

    const bool ch2_self = (ch2_fn == ADDR_BVH_TRAVERSE && ch2_this == self);
    const bool ch3_self = (ch3_fn == ADDR_BVH_TRAVERSE && ch3_this == self);
    if (__builtin_expect((!ch2_self && !ch3_self) ||
                         (rej_fn != ADDR_SEG_AABB_REJECT && rej_fn != ADDR_AABB_REJECT), 0)) {
        return col_bvh_traverse_slow(c, self, root_idx);
    }

    const uint64_t mesh = *(const uint64_t *)(uintptr_t)self;
    const uint64_t aabbs = *(const uint64_t *)(uintptr_t)(mesh + 0x60);
    const uint64_t nodes = *(const uint64_t *)(uintptr_t)(mesh + 0x68);
    const uint64_t data_base = *(const uint64_t *)(uintptr_t)mesh;
    const uint64_t tbl1 = *(const uint64_t *)(uintptr_t)(mesh + 0x48);
    const uint64_t tbl2 = *(const uint64_t *)(uintptr_t)(mesh + 0x50);
    const uint64_t tbl3 = *(const uint64_t *)(uintptr_t)(mesh + 0x58);
    const bool rej_is_seg = (rej_fn == ADDR_SEG_AABB_REJECT);
    const bool verify = (g_verify == 1);
    const int leaf_kind = (leaf_fn == ADDR_RAY_POLY) ? 1
                        : (leaf_fn == ADDR_SWEEP_POLY) ? 2
                        : (leaf_fn == ADDR_CAPSULE_POLY) ? 3 : 0;

    // Cache query AABB / segment floats in locals; reload only on leaf hit or non-self child call.
    const float *q = (const float *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(adj_this + 0x18));
    float q0 = q[0], q1 = q[1], q2 = q[2], q4 = q[4], q5 = q[5], q6 = q[6];
    float q16 = 0.0f, q17 = 0.0f, q18 = 0.0f;
    float q20 = 0.0f, q21 = 0.0f, q22 = 0.0f;
    float q24 = 0.0f, q25 = 0.0f, q26 = 0.0f;
    if (rej_is_seg) {
        q16 = q[16]; q17 = q[17]; q18 = q[18];
        q20 = q[20]; q21 = q[21]; q22 = q[22];
        q24 = q[24]; q25 = q[25]; q26 = q[26];
    }

    GuestFn ch2_gfn = (!ch2_self && ch2_fn != ADDR_BVH_TRAVERSE) ? rt_resolve_fn(c, ch2_fn) : NULL;
    GuestFn ch3_gfn = (!ch3_self && ch3_fn != ADDR_BVH_TRAVERSE) ? rt_resolve_fn(c, ch3_fn) : NULL;

    uint32_t stk[128];
    uint32_t sp = 0;
    stk[sp++] = root_idx;
    uint32_t hit = 0;

    while (sp > 0) {
        uint32_t node_idx = stk[--sp];
        uint64_t p_max = aabbs + ((uint64_t)node_idx << 5);
        const float *n_max = (const float *)(uintptr_t)p_max;
        const float *n_min = n_max + 4;

        uint32_t rej = (n_min[0] > q0 || n_min[1] > q1 || n_min[2] > q2 ||
                        q4 > n_max[0] || q5 > n_max[1] || q6 > n_max[2]) ? 1u : 0u;
        if (!rej && rej_is_seg) {
            float hx = (n_max[0] - n_min[0]) * 0.5f;
            float hy = (n_max[1] - n_min[1]) * 0.5f;
            float hz = (n_max[2] - n_min[2]) * 0.5f;
            float dx = q16 - (n_min[0] + hx);
            float dy = q17 - (n_min[1] + hy);
            float dz = (-n_min[2] - hz) + q18;
            if (!(fabsf(q22 * dy - dz * q21) <= fmaf(q25, hz, q26 * hy)) ||
                !(fabsf(q20 * dz - q22 * dx) <= fmaf(q24, hz, q26 * hx)) ||
                !(fabsf(dx * q21 - q20 * dy) <= fmaf(q24, hy, q25 * hx))) {
                rej = 1u;
            }
        }
        if (__builtin_expect(verify, 0)) {
            verify_trav_cold(c, adj_this, p_max, p_max + 0x10, rej_fn, rej,
                             rej_is_seg ? "col_seg_aabb_reject(trav)" : "col_aabb_reject(trav)");
        }
        if (rej) continue;

        uint64_t node = nodes + (uint64_t)node_idx * 12u;
        uint64_t cur = data_base + *(const uint32_t *)(uintptr_t)node;
        bool reload_q = false;

        uint32_t n1 = *(const uint8_t *)(uintptr_t)(node + 5);
        if (n1 != 0) {
            uint32_t idx16 = *(const uint16_t *)(uintptr_t)cur;
            cur += 2;
            uint32_t base1 = *(const uint32_t *)(uintptr_t)(tbl1 + ((uint64_t)*(const uint8_t *)(uintptr_t)(node + 8) << 2));
            uint32_t prim = base1 + idx16;
            uint32_t any1 = 0;
            if (leaf_kind == 1) {
                for (uint32_t i = 0; i < n1; i++, prim++) any1 |= col_ray_poly(c, leaf_this, prim);
            } else if (leaf_kind == 2) {
                for (uint32_t i = 0; i < n1; i++, prim++) any1 |= col_sweep_poly(c, leaf_this, prim);
            } else if (leaf_kind == 3) {
                for (uint32_t i = 0; i < n1; i++, prim++) any1 |= col_capsule_poly(c, leaf_this, prim);
            } else {
                for (uint32_t i = 0; i < n1; i++, prim++) any1 |= (call_guest_1(c, leaf_fn, leaf_this, prim) & 1u);
                reload_q = true;
            }
            if (any1) { hit = 1u; reload_q = true; }
        }

        uint32_t n2 = *(const uint8_t *)(uintptr_t)(node + 6);
        uint32_t base2_child = 0;
        if (n2 != 0) {
            uint32_t idx16 = *(const uint16_t *)(uintptr_t)cur;
            cur += 2;
            uint32_t base2 = *(const uint32_t *)(uintptr_t)(tbl2 + ((uint64_t)*(const uint8_t *)(uintptr_t)(node + 9) << 2));
            base2_child = base2 + idx16;
        }

        uint32_t n3 = *(const uint8_t *)(uintptr_t)(node + 4);
        uint32_t base3_child = 0;
        if (n3 != 0) {
            uint32_t idx16 = *(const uint16_t *)(uintptr_t)cur;
            uint32_t base3 = *(const uint32_t *)(uintptr_t)(tbl3 + ((uint64_t)*(const uint8_t *)(uintptr_t)(node + 7) << 2));
            base3_child = base3 + idx16;
        }

        if (ch2_self && ch3_self) {
            if (n3 != 0) {
                if (__builtin_expect(sp + n3 <= 128u, 1)) {
                    for (uint32_t i = n3; i-- > 0;) stk[sp++] = base3_child + i;
                } else {
                    for (uint32_t i = 0; i < n3; i++) hit |= col_bvh_traverse_slow(c, self, base3_child + i);
                    reload_q = true;
                }
            }
            if (n2 != 0) {
                if (__builtin_expect(sp + n2 <= 128u, 1)) {
                    for (uint32_t i = n2; i-- > 0;) stk[sp++] = base2_child + i;
                } else {
                    for (uint32_t i = 0; i < n2; i++) hit |= col_bvh_traverse_slow(c, self, base2_child + i);
                    reload_q = true;
                }
            }
        } else if (ch3_self) {
            // n2 is non-self: execute n2 first (preserving n2-before-n3 order), then push n3 onto stk.
            if (n2 != 0) {
                for (uint32_t i = 0; i < n2; i++) {
                    if (ch2_fn == ADDR_BVH_TRAVERSE) {
                        hit |= col_bvh_traverse(c, ch2_this, base2_child + i);
                    } else {
                        c->x[0] = ch2_this; c->x[1] = base2_child + i; c->pc = ch2_fn; c->x[30] = 0;
                        ch2_gfn(c);
                        hit |= (uint32_t)c->x[0];
                    }
                }
                reload_q = true;
            }
            if (n3 != 0) {
                if (__builtin_expect(sp + n3 <= 128u, 1)) {
                    for (uint32_t i = n3; i-- > 0;) stk[sp++] = base3_child + i;
                } else {
                    for (uint32_t i = 0; i < n3; i++) hit |= col_bvh_traverse_slow(c, self, base3_child + i);
                    reload_q = true;
                }
            }
        } else {
            // ch2_self && !ch3_self: if n3 != 0, execute n2 synchronously first, then n3; else push n2.
            if (n3 == 0) {
                if (n2 != 0) {
                    if (__builtin_expect(sp + n2 <= 128u, 1)) {
                        for (uint32_t i = n2; i-- > 0;) stk[sp++] = base2_child + i;
                    } else {
                        for (uint32_t i = 0; i < n2; i++) hit |= col_bvh_traverse_slow(c, self, base2_child + i);
                        reload_q = true;
                    }
                }
            } else {
                for (uint32_t i = 0; i < n2; i++) hit |= col_bvh_traverse(c, self, base2_child + i);
                for (uint32_t i = 0; i < n3; i++) {
                    if (ch3_fn == ADDR_BVH_TRAVERSE) {
                        hit |= col_bvh_traverse(c, ch3_this, base3_child + i);
                    } else {
                        c->x[0] = ch3_this; c->x[1] = base3_child + i; c->pc = ch3_fn; c->x[30] = 0;
                        ch3_gfn(c);
                        hit |= (uint32_t)c->x[0];
                    }
                }
                reload_q = true;
            }
        }

        if (__builtin_expect(reload_q, 0)) {
            q = (const float *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(adj_this + 0x18));
            q0 = q[0]; q1 = q[1]; q2 = q[2]; q4 = q[4]; q5 = q[5]; q6 = q[6];
            if (rej_is_seg) {
                q16 = q[16]; q17 = q[17]; q18 = q[18];
                q20 = q[20]; q21 = q[21]; q22 = q[22];
                q24 = q[24]; q25 = q[25]; q26 = q[26];
            }
        }
    }

    return hit & 1u;
}

static void xcde_col_aabb_reject(Cpu *c) {
    uint32_t r = col_aabb_reject(c->x[0], c->x[1], c->x[2]);
    if (__builtin_expect(g_verify == 1, 0)) {
        verify_against_guest(c, ADDR_AABB_REJECT, r, "col_aabb_reject");
    }
    c->x[0] = r;
}

static void xcde_col_seg_aabb_reject(Cpu *c) {
    uint32_t r = col_seg_aabb_reject(c->x[0], c->x[1], c->x[2]);
    if (__builtin_expect(g_verify == 1, 0)) {
        verify_against_guest(c, ADDR_SEG_AABB_REJECT, r, "col_seg_aabb_reject");
    }
    c->x[0] = r;
}

static void xcde_col_bvh_traverse(Cpu *c) {
    uint64_t saved_lr = c->x[30];
    uint32_t r = col_bvh_traverse(c, c->x[0], (uint32_t)c->x[1]);
    c->x[30] = saved_lr;
    c->x[0] = r;
}

static void xcde_col_ray_poly(Cpu *c) {
    uint64_t saved_lr = c->x[30];
    uint32_t r = col_ray_poly(c, c->x[0], (uint32_t)c->x[1]);
    c->x[30] = saved_lr;
    c->x[0] = r;
}

static void xcde_col_sweep_poly(Cpu *c) {
    uint64_t saved_lr = c->x[30];
    uint32_t r = col_sweep_poly(c, c->x[0], (uint32_t)c->x[1]);
    c->x[30] = saved_lr;
    c->x[0] = r;
}

static void xcde_col_capsule_poly(Cpu *c) {
    uint64_t saved_lr = c->x[30];
    uint32_t r = col_capsule_poly(c, c->x[0], (uint32_t)c->x[1]);
    c->x[30] = saved_lr;
    c->x[0] = r;
}

// [XCDE] Native replacement for f_70006f5460 (5-plane sphere-vs-frustum test).
void xcde_frustum_sphere(Cpu *c) {
    const uint8_t *frustum = (const uint8_t *)(uintptr_t)c->x[0];
    const float *pos = (const float *)(uintptr_t)c->x[1];
    float rad;
    uint32_t rbits = (uint32_t)c->v[0];
    memcpy(&rad, &rbits, 4);
    float px = pos[0], py = pos[1], pz = pos[2];
    for (int i = 0; i < 4; i++) {
        const float *pl = (const float *)(frustum + 0x10 + i * 0x20);
        float dist = ((px * pl[0] + pz * pl[2]) + (py * pl[1])) + pl[4];
        if (dist > rad) { c->x[0] = 0; return; }
    }
    const float *pl = (const float *)(frustum + 0x90);
    float dist = ((px * pl[0] + pz * pl[2]) + (py * pl[1])) + pl[4];
    c->x[0] = (dist <= rad) ? 1u : 0u;
}

static inline void quat_to_rot(const float q[4], float r0[4], float r1[4], float r2[4]) {
    float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    float x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
    float xx = qx * x2, yy = qy * y2, zz = qz * z2;
    float xy = qx * y2, xz = qx * z2, yz = qy * z2;
    float wx = qw * x2, wy = qw * y2, wz = qw * z2;
    r0[0] = 1.0f - (yy + zz); r0[1] = xy + wz;          r0[2] = xz - wy;          r0[3] = 0.0f;
    r1[0] = xy - wz;          r1[1] = 1.0f - (xx + zz); r1[2] = yz + wx;          r1[3] = 0.0f;
    r2[0] = xz + wy;          r2[1] = yz - wx;          r2[2] = 1.0f - (xx + yy); r2[3] = 0.0f;
}

static inline void mat4_mul_rows(float out[4][4], const float A[4][4], const float B[4][4]) {
    for (int r = 0; r < 4; r++) {
        for (int k = 0; k < 4; k++) {
            out[r][k] = fmaf(A[3][k], B[r][3], fmaf(A[2][k], B[r][2], fmaf(A[1][k], B[r][1], A[0][k] * B[r][0])));
        }
    }
}

// [XCDE] Native replacement for f_700001cf30 (bone TRS-to-4x4 matrix + parent multiply).
void xcde_bone_trs_mat(Cpu *c) {
    uint64_t self = c->x[0], node = c->x[1], out_addr = c->x[2];
    uint64_t skel = *(const uint64_t *)(uintptr_t)(self + 0x50);
    uint32_t bone_idx = (uint32_t)(int32_t)(int16_t)(*(const uint16_t *)(uintptr_t)(node + 0x78));
    uint64_t trs_arr = *(const uint64_t *)(uintptr_t)(skel + 0x80);
    const float *trs = (const float *)(uintptr_t)(trs_arr + (uint64_t)bone_idx * 0x30u);

    float r0[4], r1[4], r2[4];
    quat_to_rot(trs + 4, r0, r1, r2);
    float sx = trs[8], sy = trs[9], sz = trs[10];
    float M[4][4] = {
        { r0[0] * sx, r0[1] * sx, r0[2] * sx, 0.0f },
        { r1[0] * sy, r1[1] * sy, r1[2] * sy, 0.0f },
        { r2[0] * sz, r2[1] * sz, r2[2] * sz, 0.0f },
        { trs[0],     trs[1],     trs[2],     1.0f }
    };

    uint64_t parent_tbl = *(const uint64_t *)(uintptr_t)(skel + 0x60);
    uint32_t parent_idx = *(const uint16_t *)(uintptr_t)(parent_tbl + ((uint64_t)(bone_idx & 0xffffu) << 1));
    if (parent_idx != 0xffffu) {
        uint64_t skel_inst = *(const uint64_t *)(uintptr_t)(self + 0x58);
        const float *ptrs = NULL;
        // [XCDE] Inline f_700077abc0 fast path: when parent's world TRS is not dirty, read it directly.
        uint64_t sdef = *(const uint64_t *)(uintptr_t)(skel_inst + 0x18);
        uint32_t nbones = *(const uint32_t *)(uintptr_t)(sdef + 0x38);
        uint64_t bits_ptr = *(const uint64_t *)(uintptr_t)(skel_inst + 0x70);
        uint32_t bit_idx = nbones + (parent_idx & 0xffffu);
        uint64_t wtrs = *(const uint64_t *)(uintptr_t)(skel_inst + 0x48);
        uint64_t wcnt = *(const uint64_t *)(uintptr_t)(skel_inst + 0x50);
        if (__builtin_expect(bits_ptr != 0 && wtrs != 0 && (parent_idx & 0xffffu) < wcnt &&
                             ((((const uint32_t *)(uintptr_t)bits_ptr)[bit_idx >> 5] >> (bit_idx & 31u)) & 1u) == 0u, 1)) {
            ptrs = (const float *)(uintptr_t)(wtrs + (uint64_t)(parent_idx & 0xffffu) * 0x30u);
        } else {
            static GuestFn s_fn_700077abc0 = NULL;
            uint64_t saved_lr = c->x[30];
            c->x[0] = skel_inst;
            c->x[1] = parent_idx;
            GuestFn fn = s_fn_700077abc0;
            if (__builtin_expect(!fn, 0)) {
                fn = rt_resolve_fn(c, 0x700077abc0ull);
                s_fn_700077abc0 = fn;
            }
            c->pc = 0x700077abc0ull;
            c->x[30] = 0;
            fn(c);
            c->x[30] = saved_lr;
            ptrs = (const float *)(uintptr_t)c->x[0];
        }
        float pr0[4], pr1[4], pr2[4];
        quat_to_rot(ptrs + 4, pr0, pr1, pr2);
        float psx = ptrs[8], psy = ptrs[9], psz = ptrs[10];
        float P[4][4] = {
            { pr0[0] * psx, pr0[1] * psx, pr0[2] * psx, 0.0f },
            { pr1[0] * psy, pr1[1] * psy, pr1[2] * psy, 0.0f },
            { pr2[0] * psz, pr2[1] * psz, pr2[2] * psz, 0.0f },
            { ptrs[0],      ptrs[1],      ptrs[2],      1.0f }
        };
        float tmp[4][4];
        mat4_mul_rows(tmp, P, M);
        memcpy(M, tmp, sizeof(M));
    }

    float u = *(const float *)(uintptr_t)(self + 0x60);
    for (int r = 0; r < 4; r++) {
        M[r][0] *= u;
        M[r][1] *= u;
        M[r][2] *= u;
    }
    memcpy((void *)(uintptr_t)out_addr, M, sizeof(M));
}

void xcde_anim_eval_vec3(Cpu *c) {
    uint64_t track = c->x[0];
    float *out = (float *)(uintptr_t)c->x[1];
    float t;
    uint32_t t_bits = (uint32_t)c->v[0];
    memcpy(&t, &t_bits, sizeof(t));

    uint64_t begin = *(const uint64_t *)(uintptr_t)track;
    uint32_t count = *(const uint32_t *)(uintptr_t)(track + 8u);
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        float kt = *(const float *)(uintptr_t)(begin + (uint64_t)mid * 52u);
        if (kt > t) hi = mid;
        else lo = mid + 1u;
    }
    uint32_t idx = (lo == 0u) ? 0u : (lo - 1u);
    const float *kp = (const float *)(uintptr_t)(begin + (uint64_t)idx * 52u);
    float dt = t - kp[0];
    float dt2 = dt * dt;
    float dt3 = dt * dt2;
    for (int ch = 0; ch < 3; ch++) {
        const float *k = kp + 1 + ch * 4;
        out[ch] = (dt * k[2] + dt3 * k[0]) + (k[3] + dt2 * k[1]);
    }
}

void xcde_anim_eval_quat(Cpu *c) {
    uint64_t track = c->x[0];
    float *out = (float *)(uintptr_t)c->x[1];
    float t;
    uint32_t t_bits = (uint32_t)c->v[0];
    memcpy(&t, &t_bits, sizeof(t));

    uint64_t begin = *(const uint64_t *)(uintptr_t)track;
    uint32_t count = *(const uint32_t *)(uintptr_t)(track + 8u);
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        float kt = *(const float *)(uintptr_t)(begin + (uint64_t)mid * 68u);
        if (kt > t) hi = mid;
        else lo = mid + 1u;
    }
    uint32_t idx = (lo == 0u) ? 0u : (lo - 1u);
    const float *kp = (const float *)(uintptr_t)(begin + (uint64_t)idx * 68u);
    float dt = t - kp[0];
    float dt2 = dt * dt;
    float dt3 = dt * dt2;
    float q[4];
    for (int ch = 0; ch < 4; ch++) {
        const float *k = kp + 1 + ch * 4;
        q[ch] = (dt * k[2] + dt3 * k[0]) + (k[3] + dt2 * k[1]);
    }
    float len_sq = (q[2] * q[2] + q[0] * q[0]) + (q[3] * q[3] + q[1] * q[1]);
    float eps = **(const float *const *)(uintptr_t)0x7000b80378ull;
    if (eps > len_sq) {
        out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 0.0f;
    } else {
        float inv_len = rsqrte_s(c, len_sq);
        inv_len *= fmaf(-inv_len, len_sq * inv_len, 3.0f) * 0.5f;
        inv_len *= fmaf(-inv_len, len_sq * inv_len, 3.0f) * 0.5f;
        out[0] = q[0] * inv_len;
        out[1] = q[1] * inv_len;
        out[2] = q[2] * inv_len;
        out[3] = q[3] * inv_len;
    }
}

// [XCDE] Native C replacement for portal/occluder visibility culling (f_70006f2c00, 0.54% on guest t83).
// Returns 0 if occluded, 1 if visible.
void xcde_occluder_test(Cpu *c) {
    uint64_t occ = c->x[0];
    int32_t count = *(const int32_t *)(uintptr_t)(occ + 0x38ull);
    if (count < 1) { c->x[0] = 1u; return; }

    const float *sp = (const float *)(uintptr_t)c->x[1];
    float rad;
    uint32_t rbits = (uint32_t)c->v[0];
    memcpy(&rad, &rbits, 4);

    const float *cam = (const float *)(uintptr_t)(occ + 0x20ull);
    float dx = sp[0] - cam[0];
    float dy = sp[1] - cam[1];
    float dz = sp[2] - cam[2];
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    float inv_dist = 1.0f / dist;
    float ndx = dx * inv_dist, ndy = dy * inv_dist, ndz = dz * inv_dist;

    float eps_denom = *(const float *)(uintptr_t)0x70009d39d0ull;
    float eps_ang   = *(const float *)(uintptr_t)0x70009d3aa8ull;
    float eps_min   = *(const float *)(uintptr_t)0x70009d3a0cull;

    uint64_t items = *(const uint64_t *)(uintptr_t)(occ + 0x30ull);
    uint64_t planes = *(const uint64_t *)(uintptr_t)(occ + 0x10ull);

    for (int32_t i = 0; i < count; i++) {
        uint64_t item = items + (uint64_t)i * 0x20ull;
        uint16_t pidx = *(const uint16_t *)(uintptr_t)(item + 0x14ull);
        uint16_t side = *(const uint16_t *)(uintptr_t)(item + 0x16ull);
        const float *pl = (const float *)(uintptr_t)(planes + ((uint64_t)pidx << 6));

        // Plane equation at pl[4..7]: dot(sp, n) + d
        float sdist = sp[0] * pl[4] + sp[1] * pl[5] + sp[2] * pl[6] + pl[7];
        if (sdist < 0.0f) {
            if (side == 0) continue;
        } else {
            if (side == 1) continue;
        }

        float denom = dx * pl[4] + dy * pl[5] + dz * pl[6];
        if (fabsf(denom) < eps_denom) continue;

        float t = *(const float *)(uintptr_t)(item + 0x10ull) / denom;
        if (!(t >= 0.0f && t <= 1.0f)) continue;

        const float *orig = (const float *)(uintptr_t)item;
        float hx = dx * t + orig[0];
        float hy = dy * t + orig[1];
        float hz = dz * t + orig[2];

        // Local U projection (pl[8..11])
        float u = hx * pl[8] + hy * pl[9] + hz * pl[10];
        float u_lim = pl[11];
        if (!(u <= u_lim && u >= -u_lim)) continue;

        if (fabsf(sdist) <= rad) continue;

        // Local V projection (pl[12..15])
        float v = hx * pl[12] + hy * pl[13] + hz * pl[14];
        float v_lim = pl[15];
        if (!(v >= -v_lim && v <= v_lim)) continue;

        // Angle test via guest PLT helper f_70008b6c58 (sin/cos/acos)
        float dot_nd = ndx * pl[4] + ndy * pl[5] + ndz * pl[6];
        uint32_t dbits;
        memcpy(&dbits, &dot_nd, 4);
        c->v[0] = dbits;
        static GuestFn fn_acos_plt;
        if (!fn_acos_plt) fn_acos_plt = rt_resolve_fn(c, 0x70008b6c58ull);
        c->x[30] = 0;
        fn_acos_plt(c);
        uint32_t abits = (uint32_t)c->v[0];
        float ang;
        memcpy(&ang, &abits, 4);
        ang = fabsf(ang);
        if (ang < eps_ang) continue;

        float ox = hx + pl[0] - cam[0];
        float oy = hy + pl[1] - cam[1];
        float oz = hz + pl[2] - cam[2];
        float odist = sqrtf(ox * ox + oy * oy + oz * oz);
        float ratio = odist / dist;
        if (ratio > 1.0f) ratio = 1.0f;
        if (ratio < eps_min) ratio = eps_min;

        float margin = (1.0f / ang) * ratio * rad;
        if (margin <= u_lim && margin <= v_lim) {
            float u_in = u_lim - margin;
            float v_in = v_lim - margin;
            if (v >= -v_in && v <= v_in && u <= u_in && u >= -u_in) {
                c->x[0] = 0u;
                return;
            }
        }
    }
    c->x[0] = 1u;
}

// [XCDE] Native C replacement for mesh/skinned bounding-box & sphere update (f_700064d790, 0.64% on guest t83).
void xcde_model_bounds_update(Cpu *c) {
    uint64_t obj = c->x[0];
    const float *m0 = (const float *)(uintptr_t)(obj + 0x70ull);
    const float *m1 = (const float *)(uintptr_t)(obj + 0x80ull);
    const float *m2 = (const float *)(uintptr_t)(obj + 0x90ull);
    const float *mt = (const float *)(uintptr_t)(obj + 0xa0ull);

    float sx = m0[0] + m1[0] + m2[0];
    float sy = m0[1] + m1[1] + m2[1];
    float sz = m0[2] + m1[2] + m2[2];
    float inv_sqrt3 = *(const float *)(uintptr_t)0x70009d3e68ull;
    float obj_scale = sqrtf(sx * sx + sy * sy + sz * sz) * inv_sqrt3;
    *(float *)(uintptr_t)(obj + 0xf0ull) = obj_scale;

    int16_t nbones = *(const int16_t *)(uintptr_t)(obj + 0x180ull);
    float bmax_x, bmax_y, bmax_z;
    float bmin_x, bmin_y, bmin_z;

    if (nbones == 0) {
        uint64_t r0 = *(const uint64_t *)(uintptr_t)(obj + 0x30ull);
        const float *box = (const float *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(r0 + 0x20ull));
        float lx0 = box[1], ly0 = box[2], lz0 = box[3];
        float lx1 = box[4], ly1 = box[5], lz1 = box[6];
        bmin_x = 100000.0f; bmin_y = 100000.0f;
        bmin_z = *(const float *)(uintptr_t)0x70009d3d00ull;
        bmax_x = -100000.0f; bmax_y = -100000.0f;
        bmax_z = *(const float *)(uintptr_t)0x70009d3d58ull;

        const float xs[2] = { lx0, lx1 };
        const float ys[2] = { ly0, ly1 };
        const float zs[2] = { lz0, lz1 };
        for (int iy = 0; iy < 2; iy++) {
            float cy = ys[iy];
            for (int ix = 0; ix < 2; ix++) {
                float cx = xs[ix];
                for (int iz = 0; iz < 2; iz++) {
                    float cz = zs[iz];
                    float wx = m0[0] * cx + m1[0] * cy + m2[0] * cz + mt[0];
                    float wy = m0[1] * cx + m1[1] * cy + m2[1] * cz + mt[1];
                    float wz = m0[2] * cx + m1[2] * cy + m2[2] * cz + mt[2];
                    if (wx > bmax_x) bmax_x = wx;
                    if (wy > bmax_y) bmax_y = wy;
                    if (wz > bmax_z) bmax_z = wz;
                    if (wx < bmin_x) bmin_x = wx;
                    if (wy < bmin_y) bmin_y = wy;
                    if (wz < bmin_z) bmin_z = wz;
                }
            }
        }
    } else if (nbones < 1) {
        bmax_x = -100000.0f; bmax_y = -100000.0f; bmax_z = -100000.0f;
        bmin_x = *(const float *)(uintptr_t)0x70009d3d00ull;
        bmin_y = 100000.0f; bmin_z = 100000.0f;
    } else {
        float big = *(const float *)(uintptr_t)0x70009d3d00ull;
        bmax_x = -100000.0f; bmax_y = -100000.0f; bmax_z = -100000.0f;
        bmin_x = big; bmin_y = big; bmin_z = big;

        uint64_t r0 = *(const uint64_t *)(uintptr_t)(obj + 0x30ull);
        uint64_t mdl = *(const uint64_t *)(uintptr_t)(r0 + 0x20ull);
        uint32_t skel_off = *(const uint32_t *)(uintptr_t)(mdl + 0x28ull);
        uint64_t skel = mdl + skel_off;
        uint32_t off_tbl_rel = *(const uint32_t *)(uintptr_t)(skel + 0x14ull);
        uint64_t off_tbl = skel + off_tbl_rel;
        uint32_t desc_rel = *(const uint32_t *)(uintptr_t)(skel + 0x8ull);
        uint64_t desc_ptr = mdl + skel_off + desc_rel + 8ull;

        uint64_t slot_ptr = *(const uint64_t *)(uintptr_t)0x7000b93ec0ull;
        int32_t buf_idx = *(const int32_t *)(uintptr_t)slot_ptr;
        uint64_t mat_ptr = *(const uint64_t *)(uintptr_t)(obj + ((uint64_t)(int64_t)buf_idx << 3) + 0x60ull);
        float scale_k = obj_scale * inv_sqrt3;

        for (int32_t i = 0; i < (int32_t)nbones; i++, desc_ptr += 0x18ull, mat_ptr += 0x40ull) {
            float br = *(const float *)(uintptr_t)(desc_ptr - 4ull);
            if (!(br > 0.0f)) continue;

            const float *bm = (const float *)(uintptr_t)mat_ptr;
            float px, py, pz;
            uint8_t flags = *(const uint8_t *)(uintptr_t)desc_ptr;
            if (flags & 2u) {
                uint16_t oi = *(const uint16_t *)(uintptr_t)(desc_ptr + 4ull);
                const float *ov = (const float *)(uintptr_t)(off_tbl + ((uint64_t)oi << 5));
                px = bm[0] * ov[0] + bm[4] * ov[1] + bm[8]  * ov[2] + bm[12];
                py = bm[1] * ov[0] + bm[5] * ov[1] + bm[9]  * ov[2] + bm[13];
                pz = bm[2] * ov[0] + bm[6] * ov[1] + bm[10] * ov[2] + bm[14];
            } else {
                px = bm[12]; py = bm[13]; pz = bm[14];
            }

            float bsx = bm[0] + bm[4] + bm[8];
            float bsy = bm[1] + bm[5] + bm[9];
            float bsz = bm[2] + bm[6] + bm[10];
            float bscale = sqrtf(bsx * bsx + bsy * bsy + bsz * bsz);

            float wx = m0[0] * px + m1[0] * py + m2[0] * pz + mt[0];
            float wy = m0[1] * px + m1[1] * py + m2[1] * pz + mt[1];
            float wz = m0[2] * px + m1[2] * py + m2[2] * pz + mt[2];

            float wr = br * (scale_k * bscale);
            uint32_t wrb;
            memcpy(&wrb, &wr, 4);
            if (wrb == 0xccccccccu || (wrb & 0x7f800000u) == 0x7f800000u) continue;

            float hi_x = wx + wr, hi_y = wy + wr, hi_z = wz + wr;
            float lo_x = wx - wr, lo_y = wy - wr, lo_z = wz - wr;
            if (hi_x > bmax_x) bmax_x = hi_x;
            if (hi_y > bmax_y) bmax_y = hi_y;
            if (hi_z > bmax_z) bmax_z = hi_z;
            if (lo_x < bmin_x) bmin_x = lo_x;
            if (lo_y < bmin_y) bmin_y = lo_y;
            if (lo_z < bmin_z) bmin_z = lo_z;
        }
    }

    *(float *)(uintptr_t)(obj + 0x110ull) = bmax_x;
    *(float *)(uintptr_t)(obj + 0x114ull) = bmax_y;
    *(float *)(uintptr_t)(obj + 0x118ull) = bmax_z;
    *(float *)(uintptr_t)(obj + 0x120ull) = bmin_x;
    *(float *)(uintptr_t)(obj + 0x124ull) = bmin_y;
    *(float *)(uintptr_t)(obj + 0x128ull) = bmin_z;

    float hx = (bmax_x - bmin_x) * 0.5f;
    float hy = (bmax_y - bmin_y) * 0.5f;
    float hz = (bmax_z - bmin_z) * 0.5f;
    *(float *)(uintptr_t)(obj + 0x140ull) = hx;
    *(float *)(uintptr_t)(obj + 0x144ull) = hy;
    *(float *)(uintptr_t)(obj + 0x148ull) = hz;
    *(float *)(uintptr_t)(obj + 0x14cull) = 0.0f;

    float cx = bmin_x + hx, cy = bmin_y + hy, cz = bmin_z + hz;
    float rad = sqrtf(hx * hx + hy * hy + hz * hz);
    *(float *)(uintptr_t)(obj + 0x160ull) = rad;

    float prev_x = *(const float *)(uintptr_t)(obj + 0x130ull);
    float prev_y = *(const float *)(uintptr_t)(obj + 0x134ull);
    float prev_z = *(const float *)(uintptr_t)(obj + 0x138ull);
    float mdx = prev_x - cx, mdy = prev_y - cy, mdz = prev_z - cz;
    if (!(mdx * mdx + mdy * mdy + mdz * mdz < rad * rad)) {
        uint16_t fl = *(const uint16_t *)(uintptr_t)(obj + 0x168ull);
        *(uint16_t *)(uintptr_t)(obj + 0x168ull) = (uint16_t)(fl | 1u);
    }
    *(float *)(uintptr_t)(obj + 0x130ull) = cx;
    *(float *)(uintptr_t)(obj + 0x134ull) = cy;
    *(float *)(uintptr_t)(obj + 0x138ull) = cz;

    *(float *)(uintptr_t)(obj + 0x150ull) = cx;
    *(float *)(uintptr_t)(obj + 0x154ull) = cy;
    *(float *)(uintptr_t)(obj + 0x158ull) = cz;
    *(float *)(uintptr_t)(obj + 0x164ull) = rad;
    if (nbones != 0) {
        *(float *)(uintptr_t)(obj + 0x15cull) = 0.0f;
    }

    static GuestFn fn_bounds_tail;
    if (!fn_bounds_tail) fn_bounds_tail = rt_resolve_fn(c, 0x7000687f70ull);
    __attribute__((musttail)) return fn_bounds_tail(c);
}

// Native C fast paths for SDK libc memcpy (f_70044f11b8) and strcmp (f_70044d7900).
void xcde_sdk_memcpy(Cpu *c) {
    size_t n = (size_t)c->x[2];
    if (n) memcpy((void *)(uintptr_t)c->x[0], (const void *)(uintptr_t)c->x[1], n);
}

void xcde_sdk_strcmp(Cpu *c) {
    c->x[0] = (uint32_t)strcmp((const char *)(uintptr_t)c->x[0], (const char *)(uintptr_t)c->x[1]);
}

// Native C fast paths for shader-resource stage-pair binders:
//   f_700060fdf0 (0.41% on guest t83) and its twin f_700060feb0.
// Avoids 10-reg guest stack frame + per-iteration rt_dispatch hash lookup by resolving
// the NVN function pointer once outside the loop.
static inline void bind_stage_pairs_impl(Cpu *c, uint32_t off_max, uint32_t off_lut,
                                         uint32_t off_entries, uint64_t fn_ptr_slot_addr,
                                         uint64_t ret_lr) {
    uint64_t self = c->x[0];
    uint32_t idx = (uint32_t)c->x[1];
    uint64_t hdr = *(const uint64_t *)(uintptr_t)(self + 0x8ull);
    uint16_t max_idx = *(const uint16_t *)(uintptr_t)(hdr + off_max);
    if (max_idx <= idx) return;

    uint64_t base = *(const uint64_t *)(uintptr_t)self;
    uint32_t lut_off = *(const uint32_t *)(uintptr_t)(hdr + off_lut);
    uint8_t slot = *(const uint8_t *)(uintptr_t)(base + lut_off + idx);
    if (slot == 0xffu) return;

    uint32_t entries_off = *(const uint32_t *)(uintptr_t)(hdr + off_entries);
    uint64_t entry = base + entries_off + (uint64_t)slot * 12ull;
    uint32_t count = *(const uint8_t *)(uintptr_t)(entry + 9ull);
    if (count == 0) return;

    uint32_t pairs_off = *(const uint32_t *)(uintptr_t)(entry + 4ull);
    const uint8_t *pairs = (const uint8_t *)(uintptr_t)(base + pairs_off);
    uint64_t fn_ptr_holder = *(const uint64_t *)(uintptr_t)fn_ptr_slot_addr;
    uint64_t target_pc = *(const uint64_t *)(uintptr_t)fn_ptr_holder;
    GuestFn fn = rt_resolve_fn(c, target_pc);

    const uint32_t *stage_tbl = (const uint32_t *)(uintptr_t)0x70009e0f5cull;
    uint64_t arg2 = c->x[2];
    uint64_t arg3 = c->x[3];
    uint64_t saved_lr = c->x[30];

    for (uint32_t i = 0; i < count; i++, pairs += 2) {
        c->x[0] = arg2;
        c->x[1] = stage_tbl[pairs[0]];
        c->x[2] = pairs[1];
        c->x[3] = arg3;
        c->pc = target_pc;
        c->x[30] = ret_lr;
        fn(c);
    }
    c->x[30] = saved_lr;
}

void xcde_bind_stage_pairs_0(Cpu *c) {
    bind_stage_pairs_impl(c, 0x3au, 0x40u, 0x3cu, 0x7000b938d0ull, 0x700060fe88ull);
}

void xcde_bind_stage_pairs_1(Cpu *c) {
    bind_stage_pairs_impl(c, 0x46u, 0x4cu, 0x48u, 0x7000b938d8ull, 0x700060ff48ull);
}

// [XCDE] Wave 19: Native replacement for f_70006101c0 (UniformBuffer stage-pair bind) and
// f_70006102a0 (StorageBuffer stage-pair bind) on guest t83. Resolves the NVN bind stub once
// outside the loop and eliminates guest frame/register marshalling overhead.
static inline void bind_buf_pairs_impl(Cpu *c, uint32_t off_limit, uint32_t off_map, uint32_t off_desc,
                                       uint64_t fn_holder_addr, uint64_t ret_lr) {
    uint64_t buf_obj = c->x[0];
    uint32_t idx = (uint32_t)c->x[1];
    uint64_t mgr_ptr = *(const uint64_t *)(uintptr_t)0x7000b93760ull;
    uint64_t mgr = *(const uint64_t *)(uintptr_t)mgr_ptr;
    uint64_t sinfo = *(const uint64_t *)(uintptr_t)(mgr + 0x1120ull);
    uint64_t hdr = *(const uint64_t *)(uintptr_t)(sinfo + 0x8ull);
    uint32_t limit = *(const uint16_t *)(uintptr_t)(hdr + off_limit);
    if (limit <= idx) return;

    uint64_t base = *(const uint64_t *)(uintptr_t)sinfo;
    uint32_t map_off = *(const uint32_t *)(uintptr_t)(hdr + off_map);
    uint8_t slot = *(const uint8_t *)(uintptr_t)(base + map_off + idx);
    if (slot == 0xffu) return;

    uint32_t desc_off = *(const uint32_t *)(uintptr_t)(hdr + off_desc);
    uint64_t desc = base + desc_off + ((uint64_t)slot << 4);
    uint32_t count = *(const uint8_t *)(uintptr_t)(desc + 0xdull);
    if (count == 0u) return;

    uint64_t cmd_buf = *(const uint64_t *)(uintptr_t)(mgr + 0x1070ull);
    uint64_t fidx_ptr = *(const uint64_t *)(uintptr_t)0x7000b936e8ull;
    int64_t fidx = (int64_t)(*(const int32_t *)(uintptr_t)fidx_ptr);
    uint64_t gpu_addr = *(const uint64_t *)(uintptr_t)(buf_obj + ((uint64_t)fidx << 3) + 0x18ull);
    uint64_t buf_size = (uint64_t)(int64_t)(*(const int32_t *)(uintptr_t)(buf_obj + 0x10ull));

    uint32_t pairs_off = *(const uint32_t *)(uintptr_t)(desc + 0x8ull);
    const uint8_t *pairs = (const uint8_t *)(uintptr_t)(base + pairs_off);
    uint64_t fn_ptr_holder = *(const uint64_t *)(uintptr_t)fn_holder_addr;
    uint64_t target_pc = *(const uint64_t *)(uintptr_t)fn_ptr_holder;
    GuestFn fn = rt_resolve_fn(c, target_pc);

    const uint32_t *stage_tbl = (const uint32_t *)(uintptr_t)0x70009e0f5cull;
    uint64_t saved_lr = c->x[30];

    for (uint32_t i = 0; i < count; i++, pairs += 2) {
        c->x[0] = cmd_buf;
        c->x[1] = stage_tbl[pairs[0]];
        c->x[2] = pairs[1];
        c->x[3] = gpu_addr;
        c->x[4] = buf_size;
        c->pc = target_pc;
        c->x[30] = ret_lr;
        fn(c);
    }
    c->x[30] = saved_lr;
}

void xcde_bind_buf_pairs_0(Cpu *c) {
    bind_buf_pairs_impl(c, 0x22u, 0x28u, 0x24u, 0x7000b938c0ull, 0x7000610278ull);
}

void xcde_bind_buf_pairs_1(Cpu *c) {
    bind_buf_pairs_impl(c, 0x2eu, 0x34u, 0x30u, 0x7000b938c8ull, 0x7000610358ull);
}

// [XCDE] Native replacement for f_700077c0e0 (skeleton world TRS propagation on guest t119).
// Returns 1 if handled natively, 0 to fall back to generated code on any unexpected/assert state.
int xcde_skel_update_world_trs(Cpu *c) {
    uint64_t self = c->x[0];
    uint32_t flags = *(const uint32_t *)(uintptr_t)(self + 0x88ull);
    if ((flags & 2u) != 0u) return 1;

    uint64_t skel_def = *(const uint64_t *)(uintptr_t)(self + 0x18ull);
    uint32_t n_bones = *(const uint32_t *)(uintptr_t)(skel_def + 0x38ull);
    if (n_bones == 0u) {
        *(uint32_t *)(uintptr_t)(self + 0x88ull) = flags | 2u;
        return 1;
    }

    uint64_t local_arr = *(const uint64_t *)(uintptr_t)(self + 0x20ull);
    uint64_t local_cnt = *(const uint64_t *)(uintptr_t)(self + 0x28ull);
    uint64_t world_arr = *(const uint64_t *)(uintptr_t)(self + 0x48ull);
    uint64_t world_cnt = *(const uint64_t *)(uintptr_t)(self + 0x50ull);
    uint64_t parent_arr = *(const uint64_t *)(uintptr_t)(skel_def + 0x20ull);
    uint32_t parent_cnt = *(const uint32_t *)(uintptr_t)(skel_def + 0x28ull);
    uint64_t dirty_ptr = *(const uint64_t *)(uintptr_t)(self + 0x70ull);
    uint32_t dirty_nbits = *(const uint32_t *)(uintptr_t)(self + 0x78ull);

    if (__builtin_expect(!local_arr || !world_arr || !parent_arr || !dirty_ptr ||
                         local_cnt < n_bones || world_cnt < n_bones ||
                         parent_cnt < n_bones || dirty_nbits < n_bones * 2u, 0)) {
        return 0;
    }

    uint32_t *dirty = (uint32_t *)(uintptr_t)dirty_ptr;
    const uint16_t *parents = (const uint16_t *)(uintptr_t)parent_arr;
    const float *local_trs = (const float *)(uintptr_t)local_arr;
    float *world_trs = (float *)(uintptr_t)world_arr;
    const float eps = *(const float *)(uintptr_t)0x70009d3a0cull;
    const float *clamp_tbl = (const float *)(uintptr_t)0x70009d0b18ull;
    const float clamp_neg = clamp_tbl[0];
    const float clamp_pos = clamp_tbl[1];

    for (uint32_t i = 0; i < n_bones; i++) {
        uint32_t wbit = n_bones + i;
        uint32_t wword = wbit >> 5;
        uint32_t wmask = 1u << (wbit & 31u);
        if ((dirty[wword] & wmask) == 0u) continue;
        // If local-dirty bit is unexpectedly set, fall back (would assert in guest code).
        if (__builtin_expect((dirty[i >> 5] & (1u << (i & 31u))) != 0u, 0)) return 0;

        uint32_t p = parents[i];
        const float *src = local_trs + (size_t)i * 12u;
        float *dst = world_trs + (size_t)i * 12u;

        if (p == 0xffffu) {
            memcpy(dst, src, 48u);
        } else {
            if (__builtin_expect(p >= n_bones || (dirty[(n_bones + p) >> 5] & (1u << ((n_bones + p) & 31u))) != 0u, 0)) {
                return 0;
            }
            const float *par = world_trs + (size_t)p * 12u;
            // Rotate local translation by parent quaternion: t_w = par.t + q_par * t_loc * conj(q_par)
            float qx = par[4], qy = par[5], qz = par[6], qw = par[7];
            float vx = src[0], vy = src[1], vz = src[2];
            float rx = (qy * vz - qz * vy) + qw * vx;
            float ry = (qz * vx - qx * vz) + qw * vy;
            float rz = (qx * vy - qy * vx) + qw * vz;
            float rw = -(vx * qx) - vy * qy - vz * qz;
            dst[0] = par[0] + (qw * rx + (qy * rz - qz * ry - qx * rw));
            dst[1] = par[1] + (qw * ry + (qz * rx - qx * rz - qy * rw));
            dst[2] = par[2] + (qw * rz + (qx * ry - qy * rx - qz * rw));
            dst[3] = par[3] + src[3];

            // World quaternion: q_w = q_par * q_loc (matching ARM64 FMLA rounding on final term)
            float lx = src[4], ly = src[5], lz = src[6], lw = src[7];
            dst[4] = fmaf(-qz, ly, ((qw * lx) + (qx * lw)) + (qy * lz));
            dst[5] = fmaf(-qx, lz, ((qw * ly) + (qy * lw)) + (qz * lx));
            dst[6] = fmaf(-qy, lx, ((qw * lz) + (qz * lw)) + (qx * ly));
            dst[7] = fmaf(-qz, lz, ((qw * lw) - (qx * lx)) - (qy * ly));

            // World scale: s_w = s_par * s_loc, clamped away from 0
            for (int k = 0; k < 4; k++) {
                float s = par[8 + k] * src[8 + k];
                if (__builtin_expect(fabsf(s) < eps, 0)) {
                    s = (s <= 0.0f) ? clamp_neg : clamp_pos;
                }
                dst[8 + k] = s;
            }
        }
        dirty[wword] &= ~wmask;
    }

    *(uint32_t *)(uintptr_t)(self + 0x88ull) = flags | 2u;
    return 1;
}

// [XCDE] Native replacement for f_700077c970 (bone world TRS -> 4x4 matrix on guest t119).
// Returns 1 if handled natively, 0 to fall back to generated code on assert paths.
int xcde_skel_bone_mat(Cpu *c) {
    uint64_t self = c->x[0];
    uint32_t bone_idx = (uint32_t)(c->x[1] & 0xffffu);
    uint64_t out_addr = c->x[2];
    if (__builtin_expect(out_addr == 0ull, 0)) return 0;
    if ((*(const uint8_t *)(uintptr_t)(self + 0x88ull) & 2u) == 0u) return 1;

    uint64_t world_arr = *(const uint64_t *)(uintptr_t)(self + 0x48ull);
    uint64_t world_cnt = *(const uint64_t *)(uintptr_t)(self + 0x50ull);
    if (__builtin_expect(world_arr == 0ull || bone_idx >= world_cnt, 0)) return 0;

    const float *trs = (const float *)(uintptr_t)(world_arr + (uint64_t)bone_idx * 0x30ull);
    float r0[4], r1[4], r2[4];
    quat_to_rot(trs + 4, r0, r1, r2);
    float sx = trs[8], sy = trs[9], sz = trs[10];
    float *out = (float *)(uintptr_t)out_addr;
    out[0]  = r0[0] * sx; out[1]  = r0[1] * sx; out[2]  = r0[2] * sx; out[3]  = 0.0f;
    out[4]  = r1[0] * sy; out[5]  = r1[1] * sy; out[6]  = r1[2] * sy; out[7]  = 0.0f;
    out[8]  = r2[0] * sz; out[9]  = r2[1] * sz; out[10] = r2[2] * sz; out[11] = 0.0f;
    out[12] = trs[0];     out[13] = trs[1];     out[14] = trs[2];     out[15] = 1.0f;
    return 1;
}

// [XCDE] Wave 14: Native replacements for top PGO guest utilities:
//   f_70044f1850 (SDK memset, 844M blocks),
//   f_70008b41e0 (BitSet::test, 788M blocks),
//   f_70005ab0d0 (CRC32-table string hash, 594M blocks).
void xcde_sdk_memset(Cpu *c) {
    size_t n = (size_t)c->x[2];
    if (n) memset((void *)(uintptr_t)c->x[0], (int)(c->x[1] & 0xffu), n);
}

int xcde_bitset_test(Cpu *c) {
    uint64_t words_ptr = *(const uint64_t *)(uintptr_t)c->x[0];
    if (__builtin_expect(words_ptr == 0ull, 0)) return 0;
    uint32_t idx = (uint32_t)c->x[1];
    uint32_t word = ((const uint32_t *)(uintptr_t)words_ptr)[idx >> 5];
    c->x[0] = (word >> (idx & 31u)) & 1u;
    return 1;
}

void xcde_str_hash(Cpu *c) {
    uint64_t s_addr = c->x[0];
    if (!s_addr) return;
    const uint8_t *s = (const uint8_t *)(uintptr_t)s_addr;
    uint8_t ch = s[0];
    if (!ch) { c->x[0] = 0; return; }
    const uint32_t *tbl = (const uint32_t *)(uintptr_t)0x70009d9aa0ull;
    uint32_t h = 0, prev = 0, len = 0;
    for (;;) {
        uint32_t t = tbl[(prev ^ ch) & 0xffu] ^ h;
        prev = t;
        h = t >> 8;
        len++;
        if (len > 254u) break;
        ch = s[len];
        if (!ch) break;
    }
    c->x[0] = tbl[len] ^ h;
}

// [XCDE] Wave 15: Native replacement for f_7000754b30 (UI/HUD layout animation evaluator on guest t83,
// 2.37B PGO blocks / 0.41% perf). Replaces emulated NEON sl_ldm/sl_gen keyframe search + 30+ fp_op_s calls.
void xcde_ui_layout_eval(Cpu *c) {
    uint64_t mgr = c->x[0];
    uint64_t list_obj = c->x[1];
    uint64_t default_par = c->x[2];
    int32_t count = *(const int32_t *)(uintptr_t)list_obj;
    if (count < 1) return;

    uint64_t mgr_sub = *(const uint64_t *)(uintptr_t)(mgr + 0x2c8ull);
    uint64_t node_base = *(const uint64_t *)(uintptr_t)(mgr_sub + 0x30ull);
    uint64_t desc_ptr = *(const uint64_t *)(uintptr_t)(list_obj + 0x8ull);
    uint64_t cb_tbl_1 = *(const uint64_t *)(uintptr_t)0x7000b94aa8ull;
    uint64_t cb_tbl_0 = *(const uint64_t *)(uintptr_t)0x7000b94ab0ull;
    uint64_t saved_lr = c->x[30];

    uint64_t last_pc = 0;
    GuestFn last_fn = NULL;

    uint64_t node_addr = node_base;
    for (int32_t idx = 0; idx < count; idx++, desc_ptr += 8ull, node_addr += 0x6cull) {
        uint8_t *node = (uint8_t *)(uintptr_t)node_addr;
        uint8_t flags = node[0x6a];
        uint8_t flags_cl = flags & 0xfe;
        node[0x6a] = flags_cl;

        int16_t par_idx = *(const int16_t *)(uintptr_t)(desc_ptr + 4ull);
        uint64_t par_addr = default_par;
        if (par_idx != -1) {
            uint64_t ms = *(const uint64_t *)(uintptr_t)(mgr + 0x2c8ull);
            uint64_t nb = *(const uint64_t *)(uintptr_t)(ms + 0x30ull);
            par_addr = nb + (uint64_t)((int64_t)par_idx * 0x6cll);
        }

        if (par_addr != 0ull && (((const uint8_t *)(uintptr_t)par_addr)[0x6a] & 1u) != 0u) {
            node[0x6a] = flags | 1u;
            *(uint16_t *)(node + 0x68) = 0u;
            continue;
        }

        uint8_t desc0 = *(const uint8_t *)(uintptr_t)desc_ptr;
        if ((desc0 & 2u) != 0u) {
            memcpy(node, (const void *)(uintptr_t)par_addr, 0x6cu);
            continue;
        }

        uint64_t data_base = *(const uint64_t *)(uintptr_t)(list_obj + 0x10ull);
        uint64_t tracks = *(const uint64_t *)(uintptr_t)(list_obj + 0x18ull);
        int16_t track_idx = *(const int16_t *)(uintptr_t)(desc_ptr + 6ull);
        uint64_t track = tracks + ((uint64_t)(int64_t)track_idx << 6);
        uint32_t kf_off = *(const uint32_t *)(uintptr_t)(track + 4ull);
        uint16_t track_flags = *(const uint16_t *)(uintptr_t)(track + 0x32ull);
        const uint8_t *kf_base = (const uint8_t *)(uintptr_t)(data_base + kf_off);

        if ((track_flags & 2u) != 0u) {
            *(uint16_t *)(node + 0x68) = 0u;
        } else {
            uint64_t time_arr = *(const uint64_t *)(uintptr_t)(mgr + 0x2e0ull);
            uint16_t time_idx = *(const uint16_t *)(uintptr_t)(desc_ptr + 2ull);
            float cur_tf = *(const float *)(uintptr_t)(time_arr + (uint64_t)time_idx * 12ull + 4ull);
            int32_t cur_frame = (int32_t)cur_tf;

            uint16_t max_frame = *(const uint16_t *)(uintptr_t)(track + 0x38ull);
            if ((int32_t)max_frame < cur_frame) {
                uint16_t n_kf16 = *(const uint16_t *)(uintptr_t)track;
                *(uint16_t *)(node + 0x68) = (uint16_t)(n_kf16 - 1u);
                node[0x6a] = flags | 1u;
                continue;
            }
            uint16_t min_frame = *(const uint16_t *)(uintptr_t)(track + 0x3aull);
            if ((int32_t)min_frame > cur_frame) {
                *(uint16_t *)(node + 0x68) = 0u;
                node[0x6a] = flags | 1u;
                continue;
            }
            if ((track_flags & 1u) != 0u) {
                *(uint16_t *)(node + 0x68) = 0u;
            } else {
                uint16_t last_kf_frame = *(const uint16_t *)(uintptr_t)(track + 0x34ull);
                int32_t n_kf = *(const int32_t *)(uintptr_t)track;
                if ((int32_t)last_kf_frame < cur_frame) {
                    *(uint16_t *)(node + 0x68) = (uint16_t)(n_kf - 1);
                } else if (n_kf > 0) {
                    const uint16_t *kf16 = (const uint16_t *)kf_base;
                    uint16_t chosen = *(uint16_t *)(node + 0x68);
                    for (int32_t k = n_kf - 1; k >= 0; k--) {
                        if ((int32_t)kf16[(size_t)k * 2u] <= cur_frame) {
                            chosen = (uint16_t)k;
                            break;
                        }
                    }
                    *(uint16_t *)(node + 0x68) = chosen;
                }
            }
        }

        uint16_t kf_i = *(const uint16_t *)(node + 0x68);
        if ((kf_base[(size_t)kf_i * 4u + 2u] & 1u) != 0u) {
            node[0x6a] = flags | 1u;
            continue;
        }

        if ((flags & 4u) == 0u) {
            if (par_addr != 0ull && (((const uint8_t *)(uintptr_t)par_addr)[0x6a] & 2u) != 0u) {
                node[0x6a] = flags_cl | 2u;
                node[0x6b] = ((const uint8_t *)(uintptr_t)par_addr)[0x6b];
            } else {
                node[0x6a] = flags & 0xfcu;
            }
        }

        // Dispatch node type callback cb_tbl_0[desc[1]]
        uint8_t desc1 = *(const uint8_t *)(uintptr_t)(desc_ptr + 1ull);
        uint64_t cb0_pc = *(const uint64_t *)(uintptr_t)(cb_tbl_0 + ((uint64_t)desc1 << 3));
        if (cb0_pc != last_pc) { last_pc = cb0_pc; last_fn = rt_resolve_fn(c, cb0_pc); }
        c->x[0] = node_addr;
        c->pc = cb0_pc;
        c->x[30] = 0x7000754eb4ull;
        last_fn(c);

        // Dispatch channel callbacks
        uint16_t n_ch = *(const uint16_t *)(uintptr_t)(track + 0x30ull);
        if (n_ch != 0u) {
            uint64_t dbase = *(const uint64_t *)(uintptr_t)(list_obj + 0x10ull);
            for (uint32_t j = 0; j < n_ch; j++) {
                uint8_t ch_cb_idx = *(const uint8_t *)(uintptr_t)(track + 0x2cull + j);
                uint8_t ch_arg1 = *(const uint8_t *)(uintptr_t)(track + 0x28ull + j);
                uint32_t off2 = *(const uint32_t *)(uintptr_t)(track + 0x08ull + (uint64_t)j * 4ull);
                uint32_t off3 = *(const uint32_t *)(uintptr_t)(track + 0x18ull + (uint64_t)j * 4ull);
                uint64_t cb1_pc = *(const uint64_t *)(uintptr_t)(cb_tbl_1 + ((uint64_t)ch_cb_idx << 3));
                if (cb1_pc != last_pc) { last_pc = cb1_pc; last_fn = rt_resolve_fn(c, cb1_pc); }
                c->x[0] = node_addr;
                c->x[1] = ch_arg1;
                c->x[2] = dbase + off2;
                c->x[3] = dbase + off3;
                c->pc = cb1_pc;
                c->x[30] = 0x7000754eecull;
                last_fn(c);
            }
        }

        // Compose 2D affine transform & color/alpha with parent
        if (par_addr == 0ull) {
            memcpy(node, node + 52, 52);
        } else {
            const float *p = (const float *)(uintptr_t)par_addr;
            float *n = (float *)node;
            float p00 = p[2], p01 = p[5], p10 = p[3], p11 = p[4];
            float l00 = n[15], l01 = n[16], l10 = n[17], l11 = n[18];
            float ltx = n[13], lty = n[14];
            n[2]  = fmaf(p01, l10, p00 * l00);
            n[5]  = fmaf(p01, l01, p00 * l11);
            n[0]  = fmaf(p01, lty, p00 * ltx) + p[0];
            n[4]  = fmaf(p10, l10, p11 * l00);
            n[3]  = fmaf(p10, l01, p11 * l11);
            n[1]  = fmaf(p10, lty, p11 * ltx) + p[1];
            n[10] = p[10] + n[23];
            n[11] = p[11] + n[24];
            n[12] = p[12] + n[25];
            n[6]  = p[6] * n[19];
            n[7]  = p[7] * n[20];
            n[8]  = p[8] * n[21];
            n[9]  = p[9] * n[22];
        }
    }
    c->x[30] = saved_lr;
}

// [XCDE] Wave 15: Native replacement for f_700077f470 (skeleton blend normalization on guest t119, 0.36% perf).
void xcde_skel_blend_norm(Cpu *c) {
    uint64_t self = c->x[0];
    uint32_t n_bones = *(const uint32_t *)(uintptr_t)self;
    if (n_bones == 0u) return;

    const float *eps_ptr = (const float *)(uintptr_t)(*(const uint64_t *)(uintptr_t)0x7000b80378ull);
    float eps = *eps_ptr;
    const float *weights = (const float *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(self + 0x10ull));
    const float *bind_trs = (const float *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(self + 0x08ull));
    float *out_trs = (float *)(uintptr_t)c->x[1];

    for (uint32_t i = 0; i < n_bones; i++) {
        float *o = out_trs + (size_t)i * 12u;
        float w = weights[i];
        if (w == 0.0f) {
            memcpy(o, bind_trs + (size_t)i * 12u, 48u);
            continue;
        }
        float inv_w = 1.0f / w;
        o[0] *= inv_w; o[1] *= inv_w; o[2] *= inv_w; o[3] *= inv_w;
        o[8] *= inv_w; o[9] *= inv_w; o[10] *= inv_w; o[11] *= inv_w;
        float q0 = o[4] * inv_w, q1 = o[5] * inv_w, q2 = o[6] * inv_w, q3 = o[7] * inv_w;
        float len2 = (q0 * q0 + q2 * q2) + (q1 * q1 + q3 * q3);
        if (eps >= len2) {
            o[4] = 0.0f; o[5] = 0.0f; o[6] = 0.0f; o[7] = 0.0f;
        } else {
            float inv_len = 1.0f / sqrtf(len2);
            o[4] = q0 * inv_len; o[5] = q1 * inv_len; o[6] = q2 * inv_len; o[7] = q3 * inv_len;
        }
    }
}

__attribute__((constructor)) static void col_bvh_init(void) {
    const char *v = getenv("XCDE_COL_BVH_VERIFY");
    g_verify = (v && atoi(v) != 0) ? 1 : 0;

    const char *e = getenv("XCDE_COL_BVH");
    if (e && atoi(e) == 0) return;

    hle_override(ADDR_AABB_REJECT, xcde_col_aabb_reject, "xcde_col_aabb_reject");
    hle_override(ADDR_SEG_AABB_REJECT, xcde_col_seg_aabb_reject, "xcde_col_seg_aabb_reject");
    hle_override(ADDR_BVH_TRAVERSE, xcde_col_bvh_traverse, "xcde_col_bvh_traverse");
    hle_override(ADDR_RAY_POLY, xcde_col_ray_poly, "xcde_col_ray_poly");
    hle_override(ADDR_SWEEP_POLY, xcde_col_sweep_poly, "xcde_col_sweep_poly");
    hle_override(ADDR_CAPSULE_POLY, xcde_col_capsule_poly, "xcde_col_capsule_poly");
}
