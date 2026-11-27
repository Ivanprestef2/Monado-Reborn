// [XCDE] XCDE_BATCH_CENSUS=1: draw-to-draw state transition & batching opportunity census.
// Zero overhead when disabled (single cached static int check).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t addr, size; } BcRange;

typedef struct {
    bool valid;
    uint64_t pass_seq;
    uint64_t vs_hash, fs_hash;
    const void *pipe;
    uint32_t dyn_gen;
    float viewport[4];
    int32_t scissor[4];
    bool indexed;
    uint32_t mode, itype, count, base_inst, inst;
    int32_t base_vertex;
    uint64_t iaddr;
    uint32_t used_streams;
    uint32_t stride[16], divisor[16];
    BcRange vbuf[16];
    uint32_t used_ubuf_v, used_ubuf_f;
    BcRange ubuf_v[18], ubuf_f[18];
    uint32_t tex_mask_v, tex_mask_f;
    uint64_t tex_v[32], tex_f[32];
    uint8_t blend_on;
    uint8_t has_sbuf_or_img;
} BcDrawSnap;

enum {
    BC_B_PASS_BREAK = 0,
    BC_B_PROG_CHANGE,
    BC_B_PIPE_OR_DYN,
    BC_B_TEX_CHANGE,
    BC_B_EXACT_MDI,        // same pipe, desc, VBs; only index/count/base_vertex/inst changed
    BC_B_INST_CONTIG,      // same pipe, desc, mesh; only 1 div==1 VB changed contiguously
    BC_B_INST_NONCONTIG,   // same pipe, desc, mesh; only 1 div==1 VB changed non-contiguously
    BC_B_VB_ONLY,          // same pipe, desc; non-instanced VB(s) (+ maybe idx) changed
    BC_B_UBO1_V_ONLY,      // same pipe, tex, FS UBOs, VBs; only 1 VS UBO changed
    BC_B_UBO1_F_ONLY,      // same pipe, tex, VS UBOs, VBs; only 1 FS UBO changed
    BC_B_UBO_MULTI,        // same pipe, tex, VBs; 2+ UBOs changed
    BC_B_UBO_AND_VB,       // same pipe, tex; UBO(s) + VB/idx changed (FS desc unchanged if FS UBOs unchanged)
    BC_B_OTHER,
    BC_B_COUNT
};

static const char *const bc_bucket_names[BC_B_COUNT] = {
    "pass_break", "prog_chg", "pipe/dyn", "tex_chg",
    "exact_mdi", "inst_contig", "inst_noncontig", "vb_only",
    "ubo1_v", "ubo1_f", "ubo_multi", "ubo+vb", "other"
};

#define BC_PROG_SLOTS 128
typedef struct {
    uint64_t vs_hash, fs_hash;
    uint64_t draws;
    uint64_t merged_extra;
    uint64_t total_inst;
    uint64_t b[BC_B_COUNT];
} BcProgStat;

typedef struct {
    BcDrawSnap prev;
    uint64_t draws, merged_extra, total_inst;
    uint64_t buckets[BC_B_COUNT];
    uint64_t fs_desc_unchanged;   // same prog + pipe + tex_f + ubuf_f (FS descriptor set reusable!)
    uint64_t vs_desc_unchanged;   // same prog + pipe + tex_v + ubuf_v (VS descriptor set reusable!)
    uint64_t inst_contig_blocked_blend;
    uint64_t inst_contig_blocked_inst;
    uint64_t inst_contig_blocked_arrays;
    uint64_t ubo_v_slot_hits[18];
    uint64_t ubo_f_slot_hits[18];
    uint64_t cmds_total, cmds_noop;
    BcProgStat progs[BC_PROG_SLOTS];
    int nprogs;
} BcState;

static BcState g_bc;

static inline int bc_on(void) {
    static int on = -1;
    if (__builtin_expect(on < 0, 0)) {
        const char *e = getenv("XCDE_BATCH_CENSUS");
        on = (e && atoi(e) != 0) ? 1 : 0;
    }
    return on;
}

static inline void bc_note_cmd(bool is_noop) {
    g_bc.cmds_total++;
    if (is_noop) g_bc.cmds_noop++;
}

static inline BcProgStat *bc_prog_slot(uint64_t vh, uint64_t fh) {
    for (int i = 0; i < g_bc.nprogs; i++)
        if (g_bc.progs[i].vs_hash == vh && g_bc.progs[i].fs_hash == fh) return &g_bc.progs[i];
    if (g_bc.nprogs < BC_PROG_SLOTS) {
        BcProgStat *p = &g_bc.progs[g_bc.nprogs++];
        memset(p, 0, sizeof *p);
        p->vs_hash = vh;
        p->fs_hash = fh;
        return p;
    }
    return NULL;
}

static inline void bc_record_draw(const BcDrawSnap *cur, uint32_t merged_draws) {
    uint32_t extra = merged_draws > 1 ? merged_draws - 1 : 0;
    g_bc.draws++;
    g_bc.merged_extra += extra;
    g_bc.total_inst += cur->inst;

    int bucket = BC_B_PASS_BREAK;
    const BcDrawSnap *p = &g_bc.prev;
    if (p->valid) {
        if (cur->pass_seq != p->pass_seq) {
            bucket = BC_B_PASS_BREAK;
        } else if (cur->vs_hash != p->vs_hash || cur->fs_hash != p->fs_hash) {
            bucket = BC_B_PROG_CHANGE;
        } else if (cur->pipe != p->pipe || cur->dyn_gen != p->dyn_gen ||
                   memcmp(cur->viewport, p->viewport, sizeof cur->viewport) != 0 ||
                   memcmp(cur->scissor, p->scissor, sizeof cur->scissor) != 0) {
            bucket = BC_B_PIPE_OR_DYN;
        } else {
            // Same pass, program, pipeline, and dynamic state!
            bool tex_v_diff = (cur->tex_mask_v != p->tex_mask_v);
            if (!tex_v_diff) {
                for (uint32_t m = cur->tex_mask_v; m; ) {
                    int i = __builtin_ctz(m); m &= m - 1u;
                    if (cur->tex_v[i] != p->tex_v[i]) { tex_v_diff = true; break; }
                }
            }
            bool tex_f_diff = (cur->tex_mask_f != p->tex_mask_f);
            if (!tex_f_diff) {
                for (uint32_t m = cur->tex_mask_f; m; ) {
                    int i = __builtin_ctz(m); m &= m - 1u;
                    if (cur->tex_f[i] != p->tex_f[i]) { tex_f_diff = true; break; }
                }
            }

            uint32_t ubo_v_diff = 0, ubo_f_diff = 0;
            for (uint32_t m = cur->used_ubuf_v & 0x3ffffu; m; ) {
                int i = __builtin_ctz(m); m &= m - 1u;
                if (cur->ubuf_v[i].addr != p->ubuf_v[i].addr || cur->ubuf_v[i].size != p->ubuf_v[i].size) {
                    ubo_v_diff |= 1u << i;
                    g_bc.ubo_v_slot_hits[i]++;
                }
            }
            for (uint32_t m = cur->used_ubuf_f & 0x3ffffu; m; ) {
                int i = __builtin_ctz(m); m &= m - 1u;
                if (cur->ubuf_f[i].addr != p->ubuf_f[i].addr || cur->ubuf_f[i].size != p->ubuf_f[i].size) {
                    ubo_f_diff |= 1u << i;
                    g_bc.ubo_f_slot_hits[i]++;
                }
            }

            if (!tex_f_diff && !ubo_f_diff) g_bc.fs_desc_unchanged++;
            if (!tex_v_diff && !ubo_v_diff) g_bc.vs_desc_unchanged++;

            if (tex_v_diff || tex_f_diff) {
                bucket = BC_B_TEX_CHANGE;
            } else if (cur->has_sbuf_or_img || p->has_sbuf_or_img) {
                bucket = BC_B_OTHER;
            } else {
                uint32_t vb_diff = 0;
                for (uint32_t m = cur->used_streams & 0xffffu; m; ) {
                    int s = __builtin_ctz(m); m &= m - 1u;
                    if (cur->vbuf[s].addr != p->vbuf[s].addr || cur->vbuf[s].size != p->vbuf[s].size)
                        vb_diff |= 1u << s;
                }
                bool same_mesh = (cur->indexed == p->indexed && cur->mode == p->mode &&
                                  cur->itype == p->itype && cur->iaddr == p->iaddr &&
                                  cur->count == p->count && cur->base_vertex == p->base_vertex);
                if (!ubo_v_diff && !ubo_f_diff) {
                    if (!vb_diff) {
                        bucket = BC_B_EXACT_MDI;
                    } else if (__builtin_popcount(vb_diff) == 1 && same_mesh) {
                        int s = __builtin_ctz(vb_diff);
                        if (cur->divisor[s] == 1 && cur->stride[s] > 0) {
                            uint64_t expected = p->vbuf[s].addr + (uint64_t)p->inst * p->stride[s];
                            if (cur->vbuf[s].addr == expected) {
                                bucket = BC_B_INST_CONTIG;
                                if (cur->blend_on) g_bc.inst_contig_blocked_blend++;
                                if (cur->inst > 1 || p->inst > 1) g_bc.inst_contig_blocked_inst++;
                                if (!cur->indexed) g_bc.inst_contig_blocked_arrays++;
                            } else {
                                bucket = BC_B_INST_NONCONTIG;
                            }
                        } else {
                            bucket = BC_B_VB_ONLY;
                        }
                    } else {
                        bucket = BC_B_VB_ONLY;
                    }
                } else {
                    int nv = __builtin_popcount(ubo_v_diff), nf = __builtin_popcount(ubo_f_diff);
                    if (vb_diff || !same_mesh) {
                        bucket = BC_B_UBO_AND_VB;
                    } else if (nv == 1 && nf == 0) {
                        bucket = BC_B_UBO1_V_ONLY;
                    } else if (nv == 0 && nf == 1) {
                        bucket = BC_B_UBO1_F_ONLY;
                    } else {
                        bucket = BC_B_UBO_MULTI;
                    }
                }
            }
        }
    }

    g_bc.buckets[bucket]++;
    BcProgStat *ps = bc_prog_slot(cur->vs_hash, cur->fs_hash);
    if (ps) {
        ps->draws++;
        ps->merged_extra += extra;
        ps->total_inst += cur->inst;
        ps->b[bucket]++;
    }
    g_bc.prev = *cur;
    g_bc.prev.valid = true;
}

static inline void bc_flush_stats(void) {
    if (!bc_on() || !g_bc.draws) return;
    double inv = 100.0 / (double)g_bc.draws;
    LOG("[batch-census] draws=%llu (+%llu cliff-merged, inst=%llu) | fs_desc_same=%.1f%% vs_desc_same=%.1f%% | cmds=%llu (noop=%llu, %.1f%%)\n",
        (unsigned long long)g_bc.draws, (unsigned long long)g_bc.merged_extra, (unsigned long long)g_bc.total_inst,
        g_bc.fs_desc_unchanged * inv, g_bc.vs_desc_unchanged * inv,
        (unsigned long long)g_bc.cmds_total, (unsigned long long)g_bc.cmds_noop,
        g_bc.cmds_total ? (100.0 * (double)g_bc.cmds_noop / (double)g_bc.cmds_total) : 0.0);
    LOG("[batch-census] transitions: pass_brk=%.1f%% prog=%.1f%% pipe/dyn=%.1f%% tex=%.1f%% | exact_mdi=%.1f%% inst_c=%.1f%% inst_nc=%.1f%% vb=%.1f%% | ubo1_v=%.1f%% ubo1_f=%.1f%% ubo_m=%.1f%% ubo+vb=%.1f%% other=%.1f%%\n",
        g_bc.buckets[BC_B_PASS_BREAK] * inv, g_bc.buckets[BC_B_PROG_CHANGE] * inv,
        g_bc.buckets[BC_B_PIPE_OR_DYN] * inv, g_bc.buckets[BC_B_TEX_CHANGE] * inv,
        g_bc.buckets[BC_B_EXACT_MDI] * inv, g_bc.buckets[BC_B_INST_CONTIG] * inv,
        g_bc.buckets[BC_B_INST_NONCONTIG] * inv, g_bc.buckets[BC_B_VB_ONLY] * inv,
        g_bc.buckets[BC_B_UBO1_V_ONLY] * inv, g_bc.buckets[BC_B_UBO1_F_ONLY] * inv,
        g_bc.buckets[BC_B_UBO_MULTI] * inv, g_bc.buckets[BC_B_UBO_AND_VB] * inv,
        g_bc.buckets[BC_B_OTHER] * inv);
    if (g_bc.buckets[BC_B_INST_CONTIG]) {
        LOG("[batch-census] inst_contig_unmerged=%llu (blend=%llu inst>1=%llu arrays=%llu)\n",
            (unsigned long long)g_bc.buckets[BC_B_INST_CONTIG],
            (unsigned long long)g_bc.inst_contig_blocked_blend,
            (unsigned long long)g_bc.inst_contig_blocked_inst,
            (unsigned long long)g_bc.inst_contig_blocked_arrays);
    }
    // Hot UBO slots
    char uline[512];
    int uo = snprintf(uline, sizeof uline, "[batch-census] hot UBO slots:");
    for (int i = 0; i < 18 && uo < (int)sizeof uline - 24; i++)
        if (g_bc.ubo_v_slot_hits[i] > g_bc.draws / 50)
            uo += snprintf(uline + uo, sizeof uline - uo, " V%d:%llu", i, (unsigned long long)g_bc.ubo_v_slot_hits[i]);
    for (int i = 0; i < 18 && uo < (int)sizeof uline - 24; i++)
        if (g_bc.ubo_f_slot_hits[i] > g_bc.draws / 50)
            uo += snprintf(uline + uo, sizeof uline - uo, " F%d:%llu", i, (unsigned long long)g_bc.ubo_f_slot_hits[i]);
    LOG("%s\n", uline);
    // Top 8 programs by draw count
    int used[BC_PROG_SLOTS] = {0};
    for (int top = 0; top < 8 && top < g_bc.nprogs; top++) {
        int best = -1;
        for (int i = 0; i < g_bc.nprogs; i++)
            if (!used[i] && (best < 0 || g_bc.progs[i].draws > g_bc.progs[best].draws)) best = i;
        if (best < 0) break;
        used[best] = 1;
        BcProgStat *ps = &g_bc.progs[best];
        if (ps->draws * 100 < g_bc.draws) break;  // skip <1% programs
        int dom = 0;
        for (int b = 1; b < BC_B_COUNT; b++) if (ps->b[b] > ps->b[dom]) dom = b;
        LOG("[batch-census]   #%d vs=%016llx fs=%016llx draws=%llu (%.1f%%, +%llu merged, avg_inst=%.1f) dom=%s(%llu) [mdi=%llu ic=%llu inc=%llu vb=%llu u1v=%llu u+vb=%llu tex=%llu pd=%llu]\n",
            top + 1, (unsigned long long)ps->vs_hash, (unsigned long long)ps->fs_hash,
            (unsigned long long)ps->draws, ps->draws * inv, (unsigned long long)ps->merged_extra,
            ps->draws ? (double)ps->total_inst / (double)ps->draws : 0.0,
            bc_bucket_names[dom], (unsigned long long)ps->b[dom],
            (unsigned long long)ps->b[BC_B_EXACT_MDI], (unsigned long long)ps->b[BC_B_INST_CONTIG],
            (unsigned long long)ps->b[BC_B_INST_NONCONTIG], (unsigned long long)ps->b[BC_B_VB_ONLY],
            (unsigned long long)ps->b[BC_B_UBO1_V_ONLY], (unsigned long long)ps->b[BC_B_UBO_AND_VB],
            (unsigned long long)ps->b[BC_B_TEX_CHANGE], (unsigned long long)ps->b[BC_B_PIPE_OR_DYN]);
    }
    BcDrawSnap keep = g_bc.prev;
    memset(&g_bc, 0, sizeof g_bc);
    g_bc.prev = keep;
}
