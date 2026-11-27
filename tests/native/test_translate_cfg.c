#include <shaderc/shaderc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/nvn/maxwell/shader.h"

void rt_logf(const char *fmt, ...) { (void)fmt; }
ShaderInfo *maxwell_translate(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key);
void maxwell_free(ShaderInfo *s);

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

#define EXIT_W 0xe30000000007000full
#define NOP_W  0x50b0000000070f00ull

static uint64_t bra_to(size_t from_idx, size_t to_idx, int pr, bool prn) {
    int64_t off = (int64_t)(to_idx * 8) - (int64_t)(from_idx * 8 + 8);
    return 0xe240000000000000ull | ((uint64_t)(pr & 7) << 16) | ((uint64_t)(prn ? 1 : 0) << 19) |
           (((uint64_t)off & 0xffffffull) << 20);
}

static uint64_t ssy_to(size_t from_idx, size_t to_idx) {
    int64_t off = (int64_t)(to_idx * 8) - (int64_t)(from_idx * 8 + 8);
    return 0xe290000000000000ull | (((uint64_t)off & 0xffffffull) << 20);
}

static uint64_t sync_op(int pr, bool prn) {
    return 0xf0f8000000000000ull | ((uint64_t)(pr & 7) << 16) | ((uint64_t)(prn ? 1 : 0) << 19);
}

static uint64_t exit_op(int pr, bool prn) {
    return 0xe300000000000000ull | ((uint64_t)(pr & 7) << 16) | ((uint64_t)(prn ? 1 : 0) << 19);
}

static void check_spirv(shaderc_compiler_t comp, const char *glsl, const char *name) {
    shaderc_compile_options_t opt = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(
        comp, glsl, strlen(glsl), shaderc_fragment_shader, name, "main", opt);
    shaderc_compile_options_release(opt);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        printf("FAIL shaderc %s: %s\n%s\n", name, shaderc_result_get_error_message(r), glsl);
        fails++;
    }
    shaderc_result_release(r);
}

static ShaderInfo *xlat(const uint64_t *code, size_t nwords) {
    size_t sz = 0x80 + nwords * 8;
    uint8_t *blob = calloc(1, sz);
    memcpy(blob + 0x80, code, nwords * 8);
    ShaderKey key = {0};
    key.glsl = 1;
    ShaderInfo *s = maxwell_translate(blob, sz, STAGE_FRAGMENT, &key);
    free(blob);
    return s;
}

int main(void) {
    shaderc_compiler_t comp = shaderc_compiler_initialize();
    CHECK(comp != NULL);

    // 1. Forward if-skip: @P0 BRA i=3; i=2 NOP; i=3 EXIT
    {
        uint64_t c[8] = {0, bra_to(1, 3, 0, false), NOP_W, EXIT_W, 0};
        ShaderInfo *s = xlat(c, 4);
        CHECK(s && s->msl);
        CHECK(strstr(s->msl, "if (!p0)") != NULL);
        CHECK(strstr(s->msl, "switch (pc)") == NULL);
        CHECK(strstr(s->msl, "ssy[16]") == NULL);
        check_spirv(comp, s->msl, "if_skip");
        maxwell_free(s);
    }

    // 2. Forward if/else diamond: i=1 @P0 BRA i=5; i=2 NOP; i=3 BRA i=6; i=5 NOP; i=6 EXIT
    {
        uint64_t c[8] = {0, bra_to(1, 5, 0, false), NOP_W, bra_to(3, 6, 7, false),
                         0, NOP_W, EXIT_W, NOP_W};
        ShaderInfo *s = xlat(c, 8);
        CHECK(s && s->msl);
        CHECK(strstr(s->msl, "if (!p0)") != NULL);
        CHECK(strstr(s->msl, "} else {") != NULL);
        CHECK(strstr(s->msl, "switch (pc)") == NULL);
        CHECK(strstr(s->msl, "ssy[16]") == NULL);
        check_spirv(comp, s->msl, "if_else");
        maxwell_free(s);
    }

    // 3. Natural loop: i=1 NOP; i=2 NOP; i=3 @P0 BRA i=2; i=5 EXIT
    {
        uint64_t c[8] = {0, NOP_W, NOP_W, bra_to(3, 2, 0, false), 0, EXIT_W, NOP_W, NOP_W};
        ShaderInfo *s = xlat(c, 8);
        CHECK(s && s->msl);
        CHECK(strstr(s->msl, "for (uint _lg = 0u;") != NULL);
        CHECK(strstr(s->msl, "continue;") != NULL);
        CHECK(strstr(s->msl, "switch (pc)") == NULL);
        CHECK(strstr(s->msl, "ssy[16]") == NULL);
        check_spirv(comp, s->msl, "loop");
        maxwell_free(s);
    }

    // 4. Nested if inside loop with early EXIT:
    // i=1 NOP; i=2 @P1 BRA i=5; i=3 @P2 EXIT; i=5 @P0 BRA i=2; i=6 EXIT
    {
        uint64_t c[8] = {0, NOP_W, bra_to(2, 5, 1, false), exit_op(2, false),
                         0, bra_to(5, 2, 0, false), EXIT_W, NOP_W};
        ShaderInfo *s = xlat(c, 8);
        CHECK(s && s->msl);
        CHECK(strstr(s->msl, "for (uint _lg = 0u;") != NULL);
        CHECK(strstr(s->msl, "if (!p1)") != NULL);
        CHECK(strstr(s->msl, "exit_now = true; break;") != NULL);
        CHECK(strstr(s->msl, "switch (pc)") == NULL);
        check_spirv(comp, s->msl, "nested_loop_exit");
        maxwell_free(s);
    }

    // 5. SSY + SYNC diamond: i=1 SSY i=6; i=2 @P0 BRA i=5; i=3 SYNC; i=5 SYNC; i=6 EXIT
    {
        uint64_t c[8] = {0, ssy_to(1, 6), bra_to(2, 5, 0, false), sync_op(7, false),
                         0, sync_op(7, false), EXIT_W, NOP_W};
        ShaderInfo *s = xlat(c, 8);
        CHECK(s && s->msl);
        CHECK(strstr(s->msl, "switch (pc)") == NULL);
        CHECK(strstr(s->msl, "ssy[16]") == NULL);
        check_spirv(comp, s->msl, "ssy_sync");
        maxwell_free(s);
    }

    // 6. Hardware FP16 helpers in fragment shaders, software h2f() preserved in vertex shaders
    {
        uint64_t c[4] = {0, NOP_W, EXIT_W, NOP_W};
        uint8_t blob[0x80 + 32];
        memset(blob, 0, sizeof blob);
        memcpy(blob + 0x80, c, sizeof c);
        ShaderKey k = {.glsl = 1};
        ShaderInfo *fs = maxwell_translate(blob, sizeof blob, STAGE_FRAGMENT, &k);
        ShaderInfo *vs = maxwell_translate(blob, sizeof blob, STAGE_VERTEX, &k);
        CHECK(fs && fs->msl && vs && vs->msl);
        if (fs && fs->msl) {
            CHECK(strstr(fs->msl, "float h2f(uint h) { return unpackHalf2x16(h).x; }") != NULL);
            CHECK(strstr(fs->msl, "uint hpack0(vec2 r) { return packHalf2x16(r); }") != NULL);
            check_spirv(comp, fs->msl, "fp16_hw_fs");
        }
        if (vs && vs->msl) CHECK(strstr(vs->msl, "if (e == 0u)") != NULL);
        maxwell_free(fs);
        maxwell_free(vs);
    }

    // 7. Early forward exits to merge in if-else and if-skip (no switch(pc) fallback)
    {
        // i=1 @P0 BRA i=6; i=2 @P1 BRA i=9 (early exit to merge); i=3 NOP; i=5 BRA i=9 (end then);
        // i=6 @P2 BRA i=9 (early exit in else); i=7 NOP; i=9 EXIT
        uint64_t c[12] = {0, bra_to(1, 6, 0, false), bra_to(2, 9, 1, false), NOP_W,
                          0, bra_to(5, 9, 7, false), bra_to(6, 9, 2, false), NOP_W,
                          0, EXIT_W, NOP_W, NOP_W};
        ShaderInfo *s = xlat(c, 12);
        CHECK(s && s->msl);
        if (s && s->msl) {
            CHECK(strstr(s->msl, "switch (pc)") == NULL);
            CHECK(strstr(s->msl, "if (p1) break;") != NULL);
            CHECK(strstr(s->msl, "if (p2) break;") != NULL);
            check_spirv(comp, s->msl, "early_exit_if_else");
        }
        maxwell_free(s);
    }
    {
        // if-skip with early exit to target: i=1 @P0 BRA i=6; i=2 @P1 BRA i=6; i=3 NOP; i=5 NOP; i=6 EXIT
        uint64_t c[8] = {0, bra_to(1, 6, 0, false), bra_to(2, 6, 1, false), NOP_W,
                         0, NOP_W, EXIT_W, NOP_W};
        ShaderInfo *s = xlat(c, 8);
        CHECK(s && s->msl);
        if (s && s->msl) {
            CHECK(strstr(s->msl, "switch (pc)") == NULL);
            CHECK(strstr(s->msl, "if (p1) break;") != NULL);
            check_spirv(comp, s->msl, "early_exit_if_skip");
        }
        maxwell_free(s);
    }

    // 8. Native f16vec2 arithmetic in FS/CS when shaderFloat16 is enabled (VS stays software h2f)
    {
        // HADD2 reg (0x5d10), HMUL2 reg (0x5d08), HFMA2 reg (0x5d00), HSET2 reg (0x5d18), HSETP2 reg (0x5d20)
        uint64_t hadd2 = 0x5d10000000070001ull;   // r1 = HADD2(r0, r0)
        uint64_t hmul2 = 0x5d08000100170002ull;   // r2 = HMUL2.SAT(r0, r1)
        uint64_t hfma2 = 0x5d00000000270103ull;   // r3 = HFMA2(r1, r2, r0)
        uint64_t hset2 = 0x5d18000800270104ull;   // r4 = HSET2(r1, r2)
        uint64_t hsetp = 0x5d20000800270100ull;   // p0 = HSETP2(r1, r2)
        uint64_t c[8] = {0, hadd2, hmul2, hfma2, 0, hset2, hsetp, EXIT_W};
        uint8_t blob[0x80 + sizeof c];
        memset(blob, 0, sizeof blob);
        memcpy(blob + 0x80, c, sizeof c);
        ShaderKey k = {.glsl = 1};
        bool prev = maxwell_native_fp16;
        maxwell_native_fp16 = true;
        ShaderInfo *fs = maxwell_translate(blob, sizeof blob, STAGE_FRAGMENT, &k);
        ShaderInfo *vs = maxwell_translate(blob, sizeof blob, STAGE_VERTEX, &k);
        maxwell_native_fp16 = prev;
        CHECK(fs && fs->msl && vs && vs->msl);
        if (fs && fs->msl) {
            CHECK(strstr(fs->msl, "GL_EXT_shader_explicit_arithmetic_types_float16") != NULL);
            CHECK(strstr(fs->msl, "f16vec2 hx0(") != NULL);
            CHECK(strstr(fs->msl, "f16vec2 hx_ =") != NULL);
            check_spirv(comp, fs->msl, "fp16_native_fs");
        }
        if (vs && vs->msl) {
            CHECK(strstr(vs->msl, "GL_EXT_shader_explicit_arithmetic_types_float16") == NULL);
            CHECK(strstr(vs->msl, "if (e == 0u)") != NULL);
        }
        maxwell_free(fs);
        maxwell_free(vs);
    }

    shaderc_compiler_release(comp);
    if (fails) { printf("test_translate_cfg: %d FAIL(s)\n", fails); return 1; }
    printf("test_translate_cfg: all assertions passed\n");
    return 0;
}
