// [XCDE] T247/T248: exercise the runtime INI reader/writer together, not source text.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/settings.h"
#include "../../runtime/quality_policy.h"
#include "../../runtime/perf_overlay_opts.h"
#include "../../runtime/hle/foliage_distance.h"
bool bind_settings_load(const char *key, const char *value) { (void)key; (void)value; return false; }
void bind_settings_save(FILE *f) { fputs("mouse_look = 1\n", f); }
int main(int argc, char **argv) {
    assert(argc == 2);
#ifdef _WIN32
    assert(!_putenv_s("XCDE_INI", argv[1]));
#else
    assert(!setenv("XCDE_INI", argv[1], 1));
#endif
    FILE *f = fopen(argv[1], "w"); assert(f);
    fputs("gpu_uuid = 0123456789abcdef0123456789abcdef\nresolution = 1.5\ngpu_index = 1\ngpu_profile = compatibility\n"
          "gpu_gpl = off\ngpu_memory_budget = 0\ngpu_preset = performance\ngpu_keep_heavy = 1\n"
          "occlusion = on\namd_fix = off\nsession_fix = off\n"
          "vulkan_api = 1.2\nfps_overlay = 0\nfps_overlay_style = simple\nupscaler = auto\n"
          "stick_deadzone = 20\neffects_off = 1\nrun_speed = 1.5\nui_scale = 30\nfps_overlay_scale = 300\nfps_overlay_position = bottom-right\n"
          "cheat_damage_dealt = 1\ncheat_damage_dealt_percent = 250\ncheat_exp = 2\ncheat_exp_percent = 1o0\n"
          "cheat_enemy_health = true\ncheat_enemy_health_percent = 5\n"
          "grass_lod = extreme\n"
          "future_plugin = preserve this\n", f); fclose(f);
    settings_load();
    assert(g_settings.grass_lod == GRASS_EXTREME);
    // [XCDE] GitHub #171: the enlarged MPF block supports Ultra (3x) and the new
    // Extreme (4x); the diagnostic override caps at 4x.
    assert(grass_lod_parse("ultra") == GRASS_ULTRA);
    assert(grass_lod_parse("extreme") == GRASS_EXTREME);
    assert(grass_lod_valid(4) == GRASS_ULTRA && grass_lod_valid(5) == GRASS_EXTREME);
    assert(grass_lod_valid(GRASS_PRESET_COUNT) == GRASS_DEFAULT);
    assert(grass_lod_scale(GRASS_ULTRA) == 3.f && grass_lod_scale(GRASS_EXTREME) == 4.f);
#ifdef _WIN32
    assert(!_putenv_s("XCDE_GRASS_DISTANCE", "5"));
#else
    assert(!setenv("XCDE_GRASS_DISTANCE", "5", 1));
#endif
    xcde_foliage_configure(GRASS_EXTREME);
    assert(xcde_foliage_requested_scale() == 4.f);
#ifdef _WIN32
    assert(!_putenv_s("XCDE_GRASS_DISTANCE", ""));
#else
    assert(!unsetenv("XCDE_GRASS_DISTANCE"));
#endif
    assert(g_settings.run_speed == 1.5f);  // [XCDE] GitHub #100
    assert(g_settings.ui_scale == 50);  // [XCDE] GitHub #121: below the minimum, clamped
    assert(g_settings.res_mode == RES_1_5);
    // [XCDE] the retired effects_off AO bit moves to the single Ambient occlusion option (Off)
    assert(g_settings.graphics.value[GFX_AO] == 1 && !(g_settings.effects_off & EFFECT_TSSAO));
    assert(!strcmp(g_settings.gpu_uuid, "0123456789abcdef0123456789abcdef"));
    assert(g_settings.gpu_index == 1 && g_settings.gpu_profile == GPU_PROFILE_COMPATIBILITY);
    assert(g_settings.gpu_gpl == GPU_GPL_OFF && !g_settings.gpu_memory_budget);
    assert(g_settings.occlusion == OCCL_ON && g_settings.amd_fix == AMD_FIX_OFF && g_settings.session_fix == SESSION_FIX_OFF);
    assert(g_settings.gpu_performance && g_settings.gpu_keep_heavy);
    assert(g_settings.vulkan_api == 12 && !g_settings.fps_overlay && g_settings.upscaler_auto);
    assert(g_settings.fps_overlay_style == FPS_OVERLAY_SIMPLE);
    assert(g_settings.fps_overlay_scale == PERF_OVERLAY_SCALE_MAX && g_settings.fps_overlay_pos == PERF_POS_BOTTOM_RIGHT);  // GitHub #147
    // [XCDE] Monado II cheats: 1 / true opt in, and malformed or under-the-floor multipliers (enemy health: 10 %) read as 1x
    assert(g_settings.cheat_damage_dealt && g_settings.cheat_damage_dealt_percent == 250);
    assert(!g_settings.cheat_exp && g_settings.cheat_exp_percent == 100);  // "2" is not 1 / true and "1o0" is not a number
    assert(g_settings.cheat_enemy_health && g_settings.cheat_enemy_health_percent == 100);
    xcde_quality_apply(&g_settings,QUALITY_POTATO); g_settings.effects_off=EFFECT_SSR;
    g_settings.cheat_sp = true; g_settings.cheat_sp_percent = 2500;  // [XCDE] out of range: saved as 1x
    g_settings.cheat_no_fall_damage = true;
    g_settings.stick_deadzone = 0; settings_save();
    g_settings.gpu_uuid[0] = 0; g_settings.gpu_index = -1; g_settings.fps_overlay_style = FPS_OVERLAY_DETAILED;
    g_settings.cheat_damage_dealt = false; g_settings.cheat_damage_dealt_percent = 100; g_settings.cheat_enemy_health = false;
    g_settings.cheat_sp = false; g_settings.cheat_sp_percent = 0; g_settings.cheat_no_fall_damage = false; settings_load();
    assert(g_settings.fps_overlay_style == FPS_OVERLAY_SIMPLE);  // [XCDE] GitHub #39
    assert(g_settings.gpu_uuid[0] && g_settings.gpu_index == 1 && g_settings.stick_deadzone == 0);
    assert(g_settings.grass_lod == GRASS_EXTREME);  // [XCDE] GitHub #171: extreme round-trips
    assert(g_settings.vulkan_api == 12 && !g_settings.fps_overlay && !g_settings.upscaler_auto);
    assert(g_settings.quality_preset==QUALITY_POTATO && g_settings.gpu_performance);
    assert(g_settings.dynamic_min_height==360 && g_settings.dynamic_max_height==540);
    // [XCDE] Monado II cheats round-trip through the file, with the out-of-range multiplier written as 1x
    assert(g_settings.cheat_damage_dealt && g_settings.cheat_damage_dealt_percent == 250);
    assert(g_settings.cheat_enemy_health && g_settings.cheat_enemy_health_percent == 100);
    assert(g_settings.cheat_sp && g_settings.cheat_sp_percent == 100 && g_settings.cheat_no_fall_damage);
    // [XCDE] the old effect checkbox mask folds into the graphics choices at load (graphics_options.h)
    // (GFX_AO is 1 from the effects_off = 1 written above: #130's AO migration, now part of the general fold)
    assert(!g_settings.effects_off && g_settings.graphics.value[GFX_REFLECTIONS]==1 && g_settings.graphics.value[GFX_AO]==1);
    char data[8192] = {0}; f = fopen(argv[1], "r"); assert(f);
    assert(fread(data, 1, sizeof data - 1, f)); fclose(f);
    assert(strstr(data, "future_plugin = preserve this"));
    g_settings.res_mode = RES_1_5; g_settings.run_speed = 2.0f; g_settings.ui_scale = 75; g_settings.fps_overlay_scale = 80; g_settings.fps_overlay_pos = PERF_POS_TOP_RIGHT; settings_save();
    g_settings.res_mode = RES_MATCH; g_settings.run_speed = 1.0f; g_settings.ui_scale = 100; g_settings.fps_overlay_scale = 0; g_settings.fps_overlay_pos = 0; settings_load();
    assert(g_settings.res_mode == RES_1_5 && g_settings.run_speed == 2.0f);  // [XCDE] GitHub #100
    assert(g_settings.ui_scale == 75);  // [XCDE] GitHub #121
    assert(g_settings.fps_overlay_scale == 80 && g_settings.fps_overlay_pos == PERF_POS_TOP_RIGHT);  // GitHub #147
    // [XCDE] RCAS strength: unset stays "default" (FSR 1 0.9 / FSR 3 0.3); an explicit value round-trips, 0 = off
    assert(strstr(data, "upscale_sharpness = default") && g_settings.upscale_sharpness < 0.0f);
    g_settings.upscaler = UPSCALE_FSR1; assert(xcde_sharpness(&g_settings) == 0.9f);
    g_settings.upscaler = UPSCALE_FSR3; assert(xcde_sharpness(&g_settings) == 0.3f);
    g_settings.upscale_sharpness = 0.0f; settings_save(); g_settings.upscale_sharpness = -1.0f; settings_load();
    assert(g_settings.upscale_sharpness == 0.0f && xcde_sharpness(&g_settings) == 0.0f);
    assert(!remove(argv[1])); puts("settings round-trip: stable GPU ID, policies, 1.5x, unknown keys and default reset OK"); return 0;
}
