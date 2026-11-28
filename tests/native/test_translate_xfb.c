// T134: transform feedback capture (emulated with heap stores in the vertex shader). A vertex shader translated with a
// capture layout in its key must store each captured component, in order, at word xfbp[0].x + ordinal * stride / 4, with
// the ordinal instance-major (capture order of non-indexed lists), bounded by xfbp[1].x, into a writable heap buffer.
// Without a layout nothing changes. Programs are synthesized (no game shaders).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// [XCDE] T275: runtime/log.c is not linked into this focused test.
void rt_logf(const char *fmt, ...) { (void)fmt; }

#include "../../runtime/nvn/maxwell/shader.h"

ShaderInfo *maxwell_translate(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key);

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

#define EXIT_W 0xe30000000007000full
#define NOP_W 0x50b0000000070f00ull
#define STG_W 0xeede200000071008ull

static ShaderInfo *translate(const ShaderKey *key, uint64_t instruction) {
    uint8_t blob[0x80 + 8 * 16];
    memset(blob, 0, sizeof blob);
    uint64_t *code = (uint64_t *)(blob + 0x80);
    code[0] = 0; code[1] = instruction; code[2] = EXIT_W; code[3] = NOP_W;
    return maxwell_translate(blob, sizeof blob, STAGE_VERTEX, key);
}

int main(void) {
    ShaderKey key;
    memset(&key, 0, sizeof key);
    key.glsl = 1;
    ShaderInfo *plain = translate(&key, NOP_W);
    CHECK(plain && plain->msl);
    if (plain && plain->msl) {
        CHECK(!strstr(plain->msl, "XfbP"));
        CHECK(!plain->uses_gmem_store);
        CHECK(!plain->xfb_only_store);
    }

    // the grass writer's layout: v0.xyz, v1.xyz, stride 24
    key.xfb_n = 2;
    key.xfb_slot[0] = 0x20; key.xfb_comps[0] = 3;
    key.xfb_slot[1] = 0x21; key.xfb_comps[1] = 3;
    key.xfb_stride = 24;
    ShaderInfo *s = translate(&key, NOP_W);
    CHECK(s && s->msl);
    if (s && s->msl) {
        const char *m = s->msl;
        CHECK(s->uses_gmem && s->uses_gmem_store);
        CHECK(s->xfb_only_store);
        CHECK(strstr(m, "uniform XfbP { uvec4 xfbp[2]; }") != NULL);
        CHECK(strstr(m, "buffer GMem") && !strstr(m, "readonly buffer GMem"));
        CHECK(strstr(m, "((uint(gl_InstanceIndex) - xfbp[0].y) * xfbp[0].z + (uint(gl_VertexIndex) - xfbp[0].w)) * 6u") != NULL);
        CHECK(strstr(m, "if (xw + 6u <= xfbp[1].x)") != NULL);
        CHECK(strstr(m, "gmem_.w[xw + 0u]") && strstr(m, "gmem_.w[xw + 5u]") && !strstr(m, "gmem_.w[xw + 6u]"));
        // the capture comes before the clip-space epilogue
        const char *cap = strstr(m, "gmem_.w[xw + 0u]");
        const char *end = strrchr(m, '}');
        CHECK(cap && end && cap < end);
    }
    ShaderInfo *writer = translate(&key, STG_W);
    CHECK(writer && writer->msl && writer->uses_gmem_store && !writer->xfb_only_store);
    printf(fails ? "test_translate_xfb: %d failures\n" : "test_translate_xfb: OK\n", fails);
    return fails != 0;
}
