// Headless boot executable: fixed default settings (the window build reads xcde.ini: settings_ini.c).
#include "../settings.h"
#include "../hle/foliage_distance.h"

int g_window_w = 1280, g_window_h = 720;
const char *const g_language_codes[N_LANGUAGES] = {"", "ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-Hans", "zh-Hant"};
const char *const g_language_names[N_LANGUAGES] = {"Auto", "Japanese", "English", "French", "German", "Italian", "Spanish",
                                                   "Chinese", "Korean", "Dutch", "Portuguese", "Russian",
                                                   "Chinese (Simplified)", "Chinese (Traditional)"};
Settings g_settings = {.fps_overlay = true, .upscaler_auto = true, .upscale_sharpness = -1.0f, .volume = 1.0f, .audio_channels = AUDIO_OUT_AUTO, .display_index = -1, .hotkey_menu = XCDE_KEY_F1, .menu_pad_combo = true, .hotkey_party = XCDE_KEY_F2, .hotkey_screenshot = XCDE_KEY_F12, .hotkey_hud = XCDE_KEY_F4, .movie_audio = true, .gpu_index = -1, .gpu_profile = GPU_PROFILE_AUTO,
                       .gpu_gpl = GPU_GPL_AUTO, .occlusion = OCCL_AUTO, .amd_fix = AMD_FIX_AUTO, .session_fix = SESSION_FIX_AUTO, .gpu_memory_budget = true, .input_rumble = true, .grass_lod = GRASS_DEFAULT, .save_slots_99 = true,
                       .graphics = {.value = {[GFX_SHADOW_MASK] = 1}}};

void settings_load(void) { xcde_foliage_configure(g_settings.grass_lod); }
bool settings_save(void) { return false; }
bool settings_read_game_dump(char *out, size_t cap) { (void)out; (void)cap; return false; }
int settings_mod_priority(const char *name) { (void)name; return 0x7fffffff; }
const char *settings_language(void) { return "en-US"; }
const char *settings_language_name(void) { return "English"; }


// [XCDE] The headless boot build links the generated game code without hle/party.c: accept every footstep marker
// (the follower repeat guard lives in party.c, linked into the window build only).
struct Cpu;
_Bool xcde_footstep_marker(struct Cpu *c, unsigned long long action, unsigned long long marker) {
    (void)c; (void)action; (void)marker;
    return 1;
}
