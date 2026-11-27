// [XCDE] T-CLIFF-BATCH: preserve command order while coalescing contiguous instance records.
#pragma once
#include <string.h>
#include "../nvn.h"

typedef struct { uint64_t addr, size; } VkCliffRange;
typedef struct {
    const CmdRec *next;
    VkCliffRange last;
    uint32_t draws, instances, vertex_binds, stream_binds, inst_stream;
} VkCliffBatch;

// The caller validates shader semantics, fixed state, and inst_stream's divisor/attribute bounds.
// Only inst_stream buffer binds and identical stream-state binds may occur between these draws.
// Uncommitted commands before an incompatible draw are left for ordinary execution.
static inline VkCliffBatch vk_inst_batch_scan(const CmdRec *first, VkCliffRange initial,
                                              const uint32_t streams[][2], uint32_t nstream,
                                              uint32_t inst_stream, uint32_t limit) {
    uint32_t first_inst = (first->op == CMD_DRAW_ELEMENTS && first->x[6] && first->x[6] <= UINT32_MAX)
                              ? (uint32_t)first->x[6] : 1u;
    VkCliffBatch out = {.next = (const CmdRec *)((const uint8_t *)first + first->size),
                       .last = initial, .draws = 1, .instances = first_inst, .inst_stream = inst_stream};
    if (inst_stream >= nstream || nstream > 16 || first->op != CMD_DRAW_ELEMENTS ||
        first->size != sizeof *first || first->x[5] || !first->x[6] || first->x[6] > UINT32_MAX || limit < 2) return out;
    uint32_t stride = streams[inst_stream][0], vb = 0, vs = 0;
    if (!stride || streams[inst_stream][1] != 1) return out;
    uint64_t cap_inst = initial.size / stride;
    if (cap_inst < first_inst) return out;
    VkCliffRange pending = initial;
    const CmdRec *q = out.next;
    for (uint32_t scanned = 0; out.draws < limit && scanned < limit * 8; scanned++) {
        if (q->size < sizeof *q || (q->size & 7) || q->size > 65536) break;
        if (q->op == CMD_BIND_VERTEX_BUFFERS) {
            if (q->x[0] != inst_stream || q->x[1] != 1 || q->size < sizeof *q + sizeof pending) break;
            memcpy(&pending, q->extra, sizeof pending);
            vb++;
        } else if (q->op == CMD_BIND_VERTEX_STREAM) {
            if (q->x[0] != nstream || q->size < sizeof *q + (size_t)nstream * 8 ||
                memcmp(q->extra, streams, (size_t)nstream * 8)) break;
            vs++;
        } else if (q->op == CMD_DRAW_ELEMENTS) {
            if (q->size != sizeof *q || !q->x[6] || q->x[6] > UINT32_MAX ||
                memcmp(q->x, first->x, 6 * sizeof(uint64_t))) break;
            uint32_t q_inst = (uint32_t)q->x[6];
            uint64_t offset = (uint64_t)out.instances * stride;
            uint64_t q_bytes = (uint64_t)q_inst * stride;
            if (initial.addr > UINT64_MAX - offset || pending.addr != initial.addr + offset ||
                pending.size < q_bytes || q_inst > UINT32_MAX - out.instances ||
                cap_inst - out.instances < q_inst) break;
            out.draws++;
            out.instances += q_inst;
            out.last = pending;
            out.vertex_binds += vb;
            out.stream_binds += vs;
            vb = vs = 0;
            out.next = (const CmdRec *)((const uint8_t *)q + q->size);
        } else {
            break;  // Includes barriers, queries, inline writes, state changes and list boundaries.
        }
        q = (const CmdRec *)((const uint8_t *)q + q->size);
    }
    return out;
}

static inline VkCliffBatch vk_cliff_batch_scan(const CmdRec *first, VkCliffRange initial,
                                               const uint32_t streams[2][2], uint32_t limit) {
    return vk_inst_batch_scan(first, initial, streams, 2, 1, limit);
}

