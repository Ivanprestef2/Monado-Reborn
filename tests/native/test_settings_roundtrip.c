// [XCDE] T247/T248: exercise the runtime INI reader/writer together, not source text.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/settings.h"
#include "../../runtime/cheats_options.h"
#include "../../runtime/quality_policy.h"
#include "../../runtime/perf_overlay_opts.h"
#include "../../runtime/hle/foliage_distance.h"
// [XCDE] T275: runtime/log.c is not linked into this focused test.
void rt_logf(const char *fmt, ...) { (void)fmt; }
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
          "window = 1920x1080\n"
          "cheat_damage_dealt = 1\ncheat_damage_dealt_percent = 250\ncheat_exp = 2\ncheat_exp_percent = 1o0\n"
          "cheat_enemy_health = true\ncheat_enemy_health_percent = 5\ncheat_high_jump = true\ncamera_follow_jump = true\n"
          "grass_lod = extreme\n"
          "audio_channels = 5.1\n"
          "future_plugin = preserve this\n", f); fclose(f);
    settings_load();
    assert(g_settings.audio_channels == AUDIO_OUT_SURROUND_51);
    assert(g_settings.grass_lod == GRASS_ULTRA);
    // [XCDE] GitHub #171: the enlarged MPF block supports Ultra (3x), legacy
    // `extreme` / index 5 maps to Ultra (3x), and the diagnostic override caps at 3x.
    assert(grass_lod_parse("ultra") == GRASS_ULTRA);
    assert(grass_lod_parse("extreme") == GRASS_ULTRA);
    assert(grass_lod_valid(4) == GRASS_ULTRA && grass_lod_valid(5) == GRASS_ULTRA);
    assert(grass_lod_valid(-1) == GRASS_DEFAULT && grass_lod_valid(6) == GRASS_DEFAULT);
    assert(grass_lod_scale(GRASS_ULTRA) == 3.f);
#ifdef _WIN32
    assert(!_putenv_s("XCDE_GRASS_DISTANCE", "5"));
#else
    assert(!setenv("XCDE_GRASS_DISTANCE", "5", 1));
#endif
    xcde_foliage_configure(GRASS_ULTRA);
    assert(xcde_foliage_requested_scale() == 3.f);
#ifdef _WIN32
    assert(!_putenv_s("XCDE_GRASS_DISTANCE", ""));
#else
    assert(!unsetenv("XCDE_GRASS_DISTANCE"));
#endif
    assert(g_settings.run_speed == 1.5f);  // [XCDE] GitHub #100
    assert(g_settings.ui_scale == 50);  // [XCDE] GitHub #121: below the minimum, clamped
    assert(g_settings.res_mode == RES_1_5);
    assert(g_window_w == 1920 && g_window_h == 1080);  // [XCDE] window size chosen in the launcher / F1 menu
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
    assert(g_settings.cheat_high_jump);  // [XCDE] T276: x8 jump opt-in
    assert(g_settings.camera_follow_jump);  // [XCDE] T276: camera follows jumps
    assert(xcde_cheats_snapshot().high_jump);  // published to the guest jump hook by settings_load
    xcde_quality_apply(&g_settings,QUALITY_POTATO); g_settings.effects_off=EFFECT_SSR;
    g_settings.cheat_sp = true; g_settings.cheat_sp_percent = 2500;  // [XCDE] out of range: saved as 1x
    g_settings.cheat_no_fall_damage = true;
    g_settings.stick_deadzone = 0; settings_save();
    g_settings.gpu_uuid[0] = 0; g_settings.gpu_index = -1; g_settings.fps_overlay_style = FPS_OVERLAY_DETAILED;
    g_settings.cheat_damage_dealt = false; g_settings.cheat_damage_dealt_percent = 100; g_settings.cheat_enemy_health = false;
    g_settings.cheat_sp = false; g_settings.cheat_sp_percent = 0; g_settings.cheat_no_fall_damage = false;
    g_settings.cheat_high_jump = false; g_settings.camera_follow_jump = false; settings_load();
    assert(g_settings.fps_overlay_style == FPS_OVERLAY_SIMPLE);  // [XCDE] GitHub #39
    assert(g_settings.gpu_uuid[0] && g_settings.gpu_index == 1 && g_settings.stick_deadzone == 0);
    assert(g_settings.grass_lod == GRASS_LOW);  // [XCDE] Potato sets Low grass
    assert(g_settings.vulkan_api == 12 && !g_settings.fps_overlay && !g_settings.upscaler_auto);
    assert(g_settings.quality_preset==QUALITY_POTATO && g_settings.gpu_performance && g_settings.taa_off);
    assert(g_settings.dynamic_min_height==360 && g_settings.dynamic_max_height==540);
    // [XCDE] Monado II cheats round-trip through the file, with the out-of-range multiplier written as 1x
    assert(g_settings.cheat_damage_dealt && g_settings.cheat_damage_dealt_percent == 250);
    assert(g_settings.cheat_enemy_health && g_settings.cheat_enemy_health_percent == 100);
    assert(g_settings.cheat_sp && g_settings.cheat_sp_percent == 100 && g_settings.cheat_no_fall_damage);
    assert(g_settings.cheat_high_jump);  // [XCDE] T276 round-trips through cheat_high_jump
    assert(g_settings.camera_follow_jump);  // [XCDE] T276 round-trips through camera_follow_jump
    // [XCDE] the old effect checkbox mask folds into the graphics choices at load (graphics_options.h)
    assert(!g_settings.effects_off && g_settings.graphics.value[GFX_REFLECTIONS]==1 && g_settings.graphics.value[GFX_AO]==1);
    // [XCDE] Quality & Handheld presets round-trip cleanly
    xcde_quality_apply(&g_settings, QUALITY_QUALITY); settings_save();
    xcde_quality_apply(&g_settings, QUALITY_BALANCED); settings_load();
    assert(g_settings.quality_preset == QUALITY_QUALITY && !g_settings.gpu_performance);
    assert(g_settings.grass_lod == GRASS_ULTRA && g_settings.antialias == AA_OFF && g_settings.upscale_quality == UQ_NATIVE);
    assert(g_settings.graphics.value[GFX_SHADOW_MAPS] == 1 && g_settings.graphics.value[GFX_PROP_LOD] == 3 && g_settings.graphics.value[GFX_FILTERING] == 4);
    xcde_quality_apply(&g_settings, QUALITY_HANDHELD); settings_save();
    xcde_quality_apply(&g_settings, QUALITY_BALANCED); settings_load();
    assert(g_settings.quality_preset == QUALITY_HANDHELD && g_settings.gpu_performance);
    assert(g_settings.upscaler == UPSCALE_FSR3 && g_settings.grass_lod == GRASS_ORIGINAL && g_settings.graphics.value[GFX_FILTERING] == 3);
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
    // [XCDE] the F1 menu's window size: the chosen preset is written and read back
    g_window_w = 2560; g_window_h = 1440; settings_save();
    g_window_w = 1280; g_window_h = 720; settings_load();
    assert(g_window_w == 2560 && g_window_h == 1440);
    // [XCDE] audio output mode round-trips (5.1 / stereo / auto)
    g_settings.audio_channels = AUDIO_OUT_STEREO; settings_save();
    g_settings.audio_channels = AUDIO_OUT_AUTO; settings_load();
    assert(g_settings.audio_channels == AUDIO_OUT_STEREO);
    g_settings.audio_channels = AUDIO_OUT_SURROUND_51; settings_save();
    g_settings.audio_channels = AUDIO_OUT_AUTO; settings_load();
    assert(g_settings.audio_channels == AUDIO_OUT_SURROUND_51);
    // [XCDE] custom FPS and early limiter mode round-trip
    g_settings.fps = 72; g_settings.fps_limiter = FPS_LIMITER_EARLY; settings_save();
    g_settings.fps = 30; g_settings.fps_limiter = FPS_LIMITER_DEFAULT; settings_load();
    assert(g_settings.fps == 72 && g_settings.fps_limiter == FPS_LIMITER_EARLY);
    g_settings.fps = 40; g_settings.fps_limiter = FPS_LIMITER_DEFAULT; settings_save();
    g_settings.fps = 60; g_settings.fps_limiter = FPS_LIMITER_EARLY; settings_load();
    assert(g_settings.fps == 40 && g_settings.fps_limiter == FPS_LIMITER_DEFAULT);
    g_settings.fps = 0; settings_save(); g_settings.fps = 30; settings_load();
    assert(g_settings.fps == 0);
    // [XCDE] QoL settings round-trip: focus mute, bg FPS, display/fullscreen mode, audio device, hotkeys, mod_order
    g_settings.fullscreen_mode = FULLSCREEN_EXCLUSIVE;
    g_settings.display_index = 2;
    snprintf(g_settings.audio_device, sizeof g_settings.audio_device, "%s", "HDMI 5.1 AVR");
    g_settings.mute_unfocused = true;
    g_settings.bg_fps = 15;
    g_settings.hotkey_menu = XCDE_KEY_INSERT;
    g_settings.menu_pad_combo = false;
    g_settings.hotkey_party = XCDE_KEY_F3;
    g_settings.hotkey_screenshot = XCDE_KEY_PRINTSCREEN;
    g_settings.hotkey_hud = XCDE_KEY_NONE;
    assert(settings_save());
    g_settings.fullscreen_mode = FULLSCREEN_BORDERLESS;
    g_settings.display_index = -1;
    g_settings.audio_device[0] = 0;
    g_settings.mute_unfocused = false;
    g_settings.bg_fps = 0;
    g_settings.hotkey_menu = XCDE_KEY_F1;
    g_settings.menu_pad_combo = true;
    g_settings.hotkey_party = XCDE_KEY_F2;
    g_settings.hotkey_screenshot = XCDE_KEY_F12;
    g_settings.hotkey_hud = XCDE_KEY_F4;
    settings_load();
    assert(g_settings.fullscreen_mode == FULLSCREEN_EXCLUSIVE && g_settings.display_index == 2);
    assert(!strcmp(g_settings.audio_device, "HDMI 5.1 AVR"));
    assert(g_settings.mute_unfocused && g_settings.bg_fps == 15);
    assert(g_settings.hotkey_menu == XCDE_KEY_INSERT && !g_settings.menu_pad_combo);
    assert(g_settings.hotkey_party == XCDE_KEY_F3 && g_settings.hotkey_screenshot == XCDE_KEY_PRINTSCREEN);
    assert(g_settings.hotkey_hud == XCDE_KEY_NONE);
    {
        FILE *mf = fopen(argv[1], "a"); assert(mf);
        fputs("mod_order = mod_z, mod_a\n", mf); fclose(mf);
        settings_load();
        assert(settings_mod_priority("mod_z") == 0 && settings_mod_priority("mod_a") == 1);
        assert(settings_mod_priority("unlisted") > settings_mod_priority("mod_a"));
    }
    char invalid_path[1024];
    assert(snprintf(invalid_path, sizeof invalid_path, "%s/xcde.ini", argv[1]) < (int)sizeof invalid_path);
#ifdef _WIN32
    assert(!_putenv_s("XCDE_INI", invalid_path));
#else
    assert(!setenv("XCDE_INI", invalid_path, 1));
#endif
    assert(!settings_save());  // the existing INI cannot also be a parent directory
    assert(!remove(argv[1])); puts("settings round-trip: stable GPU ID, policies, 1.5x, window size, unknown keys and default reset OK"); return 0;
}
