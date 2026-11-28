#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MATERIAL_DRAWS = 8, MATERIAL_TEXTURES = 8 };

// Preserve the translated bodies and uniform layouts. The vertex stage passes
// the indirect draw slot through a flat varying, leaving guest instance IDs intact.
static bool material_shader_sources(const char *vs, const char *fs, unsigned location,
                                    unsigned textures, char **vertex, char **fragment) {
    const char *main_start = strstr(vs, "void main() {\n");
    const char *fs_version = strchr(fs, '\n');
    if (!main_start || !fs_version || !textures || textures > MATERIAL_TEXTURES) return false;
    const char *vs_main = main_start + strlen("void main() {\n");
    fs_version++;
    size_t vertex_size = strlen(vs) + 256;
    size_t fragment_size = strlen(fs) + 2048;
    char *v = malloc(vertex_size), *f = malloc(fragment_size);
    if (!v || !f) goto fail;
    snprintf(v, vertex_size, "%.*slayout(location=%u) flat out uint mdiDraw;\n"
             "void main() {\n  mdiDraw = uint(gl_DrawID);\n%s",
             (int)(main_start - vs), vs, location, vs_main);
    int used = snprintf(f, fragment_size, "%.*s#extension GL_EXT_nonuniform_qualifier : require\n",
                        (int)(fs_version - fs), fs);
    const char *cursor = fs_version;
    for (unsigned i = 0; i < textures; i++) {
        char declaration[96];
        snprintf(declaration, sizeof declaration, "layout(set = 1, binding = %u) uniform sampler2D t%u;\n", 32 + i, i);
        const char *found = strstr(cursor, declaration);
        if (!found) goto fail;
        used += snprintf(f + used, fragment_size - used, "%.*s", (int)(found - cursor), cursor);
        if (i == 0)
            used += snprintf(f + used, fragment_size - used, "layout(location=%u) flat in uint mdiDraw;\n"
                             "layout(set=2,binding=0) uniform sampler2D mdiTextures[%u];\n"
                             "layout(std430,set=2,binding=1) readonly buffer DrawMaterials { uint mdiMaterial[]; };\n",
                             location, textures * MATERIAL_DRAWS);
        used += snprintf(f + used, fragment_size - used,
                         "#define t%u mdiTextures[nonuniformEXT(mdiMaterial[mdiDraw] * %uu + %uu)]\n",
                         i, textures, i);
        cursor = found + strlen(declaration);
    }
    snprintf(f + used, fragment_size - used, "%s", cursor);
    *vertex = v;
    *fragment = f;
    return true;
fail:
    free(v);
    free(f);
    return false;
}
