#include "../fps_meter.h"
#include "gpu_picker.h"
// [XCDE] In-game settings overlay (runtime/ui/menu.h; Windows / Vulkan): F1 opens and closes it (Esc closes), the
// launcher's options over the running game. Dear ImGui drawn by its Vulkan backend into the window image after the
// game image (device.c calls menu_render inside that rendering). Events arrive on the main thread and are queued for
// the render thread, which owns the ImGui context; while the menu is open the game sees no keyboard / mouse / pad
// input. Options that the renderer reads every frame apply at once (resolution, upscaler and its quality, SMAA, depth
// of field, volume, FPS in the title, V-Sync); the others are saved to xcde.ini and apply at the next start
// (marked "al reiniciar"). "Guardar" writes xcde.ini (settings_save) for the launcher and the next start.
#include <SDL3/SDL.h>
#include <math.h>
#include <mutex>
#include <stdio.h>
#include <string>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif

#include "imgui.h"
#include "theme.h"
#include "quality_controls.h"
#include "cheats_controls.h"  // [XCDE] optional cheats (Monado II port)
#include "imgui_impl_vulkan.h"
#include "../window_size.h"  // [XCDE] window size presets, shared with the launcher

extern "C" {
#include "../nvn/vulkan/vk.h"
#include "../settings.h"
#include "../hle/foliage_distance.h"
#include "../graphics_options.h"
#include "../ui_scale.h"  // [XCDE] GitHub #121
#include "../cheats_options.h"
#include "../input_bind.h"
#include "menu.h"
extern bool g_party_switch;
bool vk_imgui_backend_init(void);
void input_reset(void);
void vk_request_swapchain_recreate(void);
bool dlss_available(void);
const char *dlss_status(void);
bool diag_deterministic(void);
bool tmp_jitter_active(void);  // a temporal upscaler is selected: the game's TAA is off (runtime/hle/graphics.c)
void plat_terminate(void);  // [XCDE] GitHub #22: the F1 menu's "exit the game" button (the platform's immediate exit)
void audio_select_device(const char *name);  // [XCDE] live audio hot-swap (runtime/services/audio.c)
void audio_select_channels(int mode);
const SDL_DisplayMode *xcde_pick_exclusive_mode(SDL_Window *win, SDL_DisplayMode *out);
// [XCDE] GitHub #39: what the performance overlay (perf_overlay.h) reads
int xcde_present_fps(void);
float vk_display_hz_cached(void);
bool tmp_fg_active(void);
const char *tmp_overlay_info(uint32_t *render_w, uint32_t *render_h, uint32_t *out_w, uint32_t *out_h);
}

#ifndef _WIN32
// [XCDE] Resolve a scalable system font through fontconfig. This covers Linux distributions
// whose font packages install fonts outside the traditional Arch/Debian paths (e.g. Fedora).
static std::string fc_match_font(const char *pattern) {
    const std::string cmd = std::string("fc-match -f '%{file}' '") + pattern + "' 2>/dev/null";
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};

    char path[1024];
    size_t n = fread(path, 1, sizeof(path) - 1, pipe);
    pclose(pipe);
    path[n] = '\0';
    while (n > 0 && (path[n - 1] == '\n' || path[n - 1] == '\r' || path[n - 1] == ' '))
        path[--n] = '\0';

    if (n < 4) return {};
    const char *ext = path + n - 4;
    if (strcasecmp(ext, ".ttf") != 0 && strcasecmp(ext, ".otf") != 0 && strcasecmp(ext, ".ttc") != 0)
        return {};

    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) ? std::string(path) : std::string();
}
#endif

static XcdeFpsMeter fps_meter;
#include "perf_overlay.h"
extern "C" void menu_fps_tick(uint64_t now_ns) { xcde_fps_tick(&fps_meter, now_ns); perf_frame_tick(now_ns); }
extern "C" void menu_frame_info(uint32_t w, uint32_t h) { perf_game_w = (int)w; perf_game_h = (int)h; }
static std::mutex q_lock;
struct QueuedEvent { SDL_Event e; char text[64]; };
static QueuedEvent q[256];
static int q_n;
static volatile bool open_, want_window, want_size, want_display, want_text_input;
static bool fs_applied = false, fs_init, text_input_on = false;
static int fs_mode_applied = FULLSCREEN_BORDERLESS;

static std::mutex toast_lock;
static char toast_msg[320];
static uint64_t toast_until_ns;
static const uint64_t TOAST_DUR_NS = 2800000000ull, TOAST_FADE_NS = 600000000ull;

extern "C" void menu_toast(const char *msg) {
    if (!msg || !*msg) return;
    std::lock_guard<std::mutex> g(toast_lock);
    snprintf(toast_msg, sizeof toast_msg, "%s", msg);
    toast_until_ns = SDL_GetTicksNS() + TOAST_DUR_NS;
}

static bool is_input(Uint32 t) {
    switch (t) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return true;
    default: return false;
    }
}

extern "C" bool menu_handle_event(const SDL_Event *e) {
    int mk = g_settings.hotkey_menu > 0 ? g_settings.hotkey_menu : XCDE_KEY_F1;
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat &&
        ((int)e->key.scancode == mk || (open_ && e->key.scancode == SDL_SCANCODE_ESCAPE))) {
        open_ = !open_ && (int)e->key.scancode == mk;
        input_reset();  // (keys held when the menu opened / closed are released for the game)
        return true;
    }
    if (!open_ || !is_input(e->type)) return false;
    std::lock_guard<std::mutex> g(q_lock);
    if (q_n < 256) {
        QueuedEvent &dst = q[q_n++];
        dst.e = *e;
        dst.text[0] = 0;
        // SDL3's SDL_TextInputEvent::text is a transient pointer valid only during the event poll.
        if (e->type == SDL_EVENT_TEXT_INPUT && e->text.text) snprintf(dst.text, sizeof dst.text, "%s", e->text.text);
    }
    return true;
}

extern "C" bool menu_is_open(void) { return open_; }

extern "C" void menu_apply_window_requests(void *sdl_window) {
    if (!fs_init) { fs_init = true; fs_applied = g_settings.fullscreen; fs_mode_applied = g_settings.fullscreen_mode; }
    if (!sdl_window) return;
    SDL_Window *win = (SDL_Window *)sdl_window;
    if (want_display) {
        want_display = false;
        if (g_settings.display_index >= 0) {
            int ndisp = 0;
            SDL_DisplayID *disps = SDL_GetDisplays(&ndisp);
            if (disps && g_settings.display_index < ndisp) {
                SDL_Rect r;
                if (SDL_GetDisplayBounds(disps[g_settings.display_index], &r)) {
                    bool was_fs = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) != 0;
                    if (was_fs) SDL_SetWindowFullscreen(win, false);
                    int ww = g_window_w, wh = g_window_h;
                    SDL_GetWindowSize(win, &ww, &wh);
                    SDL_SetWindowPosition(win, r.x + (r.w - ww) / 2, r.y + (r.h - wh) / 2);
                    if (was_fs) want_window = true;
                }
            }
            SDL_free(disps);
        }
    }
    if (want_window) {
        want_window = false;
        bool mode_changed = fs_applied && g_settings.fullscreen && fs_mode_applied != g_settings.fullscreen_mode;
        if (g_settings.fullscreen) {
            if (mode_changed) {
                // Tear down the previous fullscreen mode cleanly (Wayland/X11 keep stale viewport/mouse scale otherwise).
                SDL_SetWindowFullscreen(win, false);
                SDL_SyncWindow(win);
            }
            if (g_settings.fullscreen_mode == FULLSCREEN_EXCLUSIVE) {
                SDL_DisplayMode dm;
                SDL_SetWindowFullscreenMode(win, xcde_pick_exclusive_mode(win, &dm));
            } else {
                SDL_SetWindowFullscreenMode(win, NULL);
            }
            SDL_SetWindowFullscreen(win, true);
            SDL_SyncWindow(win);
            vk_request_swapchain_recreate();
        } else {
            SDL_SetWindowFullscreen(win, false);
            SDL_SyncWindow(win);
            vk_request_swapchain_recreate();
        }
        fs_applied = g_settings.fullscreen;
        fs_mode_applied = g_settings.fullscreen_mode;
    }
    // [XCDE] a size chosen while fullscreen waits for the window: fullscreen covers the display, not this size.
    if (want_size) {
        Uint32 flags = SDL_GetWindowFlags(win);
        if (!(flags & SDL_WINDOW_FULLSCREEN)) {
            want_size = false;
            if (flags & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(win);
            SDL_SetWindowSize(win, g_window_w, g_window_h);
        }
    }
    bool need_text = open_ && want_text_input;
    if (need_text != text_input_on) {
        if (need_text) SDL_StartTextInput(win);
        else SDL_StopTextInput(win);
        text_input_on = need_text;
    }
}

extern "C" void menu_test_open(void) { open_ = true; }

extern "C" void menu_open_request(void) {  // [XCDE] GitHub #22: Start + Select on the controller
    if (!open_) open_ = true;
    input_reset();  // (keys / buttons held when the menu opened are released for the game)
}

static ImGuiKey key_of(SDL_Scancode s) {
    switch (s) {
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    default: return ImGuiKey_None;
    }
}
static ImGuiKey pad_key(Uint8 b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    default: return ImGuiKey_None;
    }
}

// Events queued by the main thread -> ImGui IO (render thread). Mouse positions: window points -> pixels.
static void feed(ImGuiIO &io, uint32_t w, uint32_t h) {
    QueuedEvent ev[256];
    int n;
    {
        std::lock_guard<std::mutex> g(q_lock);
        n = q_n;
        memcpy(ev, q, sizeof(QueuedEvent) * n);
        q_n = 0;
    }
    for (int i = 0; i < n; i++) {
        const SDL_Event &e = ev[i].e;
        float sx = 1, sy = 1;
        SDL_Window *win = nullptr;
        if (e.type == SDL_EVENT_MOUSE_MOTION) win = SDL_GetWindowFromID(e.motion.windowID);
        else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) win = SDL_GetWindowFromID(e.button.windowID);
        if (win) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(win, &ww, &wh);
            if (ww > 0 && wh > 0) { sx = (float)w / ww; sy = (float)h / wh; }
        }
        switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION: io.AddMousePosEvent(e.motion.x * sx, e.motion.y * sy); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
            io.AddMousePosEvent(e.button.x * sx, e.button.y * sy);
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            io.AddMousePosEvent(e.button.x * sx, e.button.y * sy);
            io.AddMouseButtonEvent(b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: io.AddMouseWheelEvent(e.wheel.x, e.wheel.y); break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
            ImGuiKey k = key_of(e.key.scancode);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == SDL_EVENT_KEY_DOWN);
            break;
        }
        case SDL_EVENT_TEXT_INPUT: if (ev[i].text[0]) io.AddInputCharactersUTF8(ev[i].text); break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            ImGuiKey k = pad_key(e.gbutton.button);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) { open_ = false; input_reset(); }  // (B: back to the game)
            break;
        }
        default: break;
        }
    }
}

// [XCDE] the menu's language follows the launcher's (xcde.ini launcher_lang; g_settings.ui_lang 1 = English): the
// Spanish text is the key, T() gives the English one.
static const char *const k_en[][2] = {
    {"AMD FSR 4 Lite (temporal, nativo)", "AMD FSR 4 Lite (temporal, native)"},  // [XCDE] T-FSR4-LITE
    {"Igual que la ventana (recomendado)", "Match the window (recommended)"}, {"1x - 720p (como Switch)", "1x - 720p (like Switch)"},
    {"75 % de la ventana", "75 % of the window"}, {"50 % de la ventana", "50 % of the window"},
    {"Dinámica (según objetivo FPS)", "Dynamic (follows FPS target)"}, {"Opciones  (F1 / Esc para volver al juego)", "Options  (F1 / Esc to go back to the game)"},
    {"Gráficos", "Graphics"}, {"Pantalla", "Display"}, {"Pantalla completa", "Fullscreen"},
    {"Tamaño de la ventana", "Window size"}, {"Personalizado (%dx%d)", "Custom (%dx%d)"},
    {"Resolución y escalado", "Resolution and scaling"}, {"Resolución interna", "Internal resolution"},
    {"Bilineal (suave)", "Bilinear (soft)"}, {"AMD FSR 1.0 (nítido)", "AMD FSR 1.0 (sharp)"}, {"Escalado / antialiasing", "Upscaling / anti-aliasing"},
    {"DLSS se carga al iniciar el juego: hasta reiniciar se usa FSR 3.1.", "DLSS loads when the game starts: FSR 3.1 is used until a restart."},
    {"Nativo (solo antialiasing)", "Native (anti-aliasing only)"}, {"Calidad (67 %)", "Quality (67 %)"}, {"Equilibrado (59 %)", "Balanced (59 %)"},
    {"Rendimiento (50 %)", "Performance (50 %)"}, {"Ultra rendimiento (33 %)", "Ultra performance (33 %)"}, {"Calidad del escalado", "Upscaling quality"},
    {"Generación de frames (FSR 3; activarla por primera vez: al reiniciar)", "Frame generation (FSR 3; the first time it is enabled: after a restart)"},
    {"Calidad", "Quality"}, {"Antialiasing SMAA", "SMAA anti-aliasing"},
    {"Profundidad de campo", "Depth of field"},
    {"Antialiasing y desenfoque de movimiento del juego (al reiniciar)", "The game's anti-aliasing and motion blur (after a restart)"},
    {"Antialiasing temporal (TAA) del juego", "The game's temporal anti-aliasing (TAA)"},
    {"Mostrar FPS en la barra de título", "Show FPS in the title bar"}, {"Velocidad", "Speed"}, {"30 (como en Switch)", "30 (like Switch)"},
    {"Sin límite", "Unlímited"}, {"FPS (al reiniciar)", "FPS (after a restart)"}, {"V-Sync (con 30 / 60 FPS)", "V-Sync (at 30 / 60 FPS)"},
    {"Volumen", "Volume"}, {"Audio de los vídeos (al reiniciar)", "Video sound (after a restart)"},
    {"Velocidad al correr", "Run speed"}, {"Normal (x1)", "Normal (x1)"}, {"Rápida (x1.5)", "Fast (x1.5)"},
    {"Muy rápida (x2)", "Very fast (x2)"}, {"Controles", "Controls"}, {"Trucos", "Cheats"},
    {"La asignación de teclas y botones se cambia en el launcher (Play Monado.bat), pestaña Controles.",
     "Keys and buttons are remapped in the launcher, Controls tab."},
    {"GPU / Vulkan", "GPU / Vulkan"}, {"Adaptador Vulkan activo", "Active Vulkan adapter"},
    {"Índice de GPU (-1 = automático)", "GPU index (-1 = automatic)"},
    {"Perfil gráfico", "Graphics profile"}, {"Automático (recomendado)", "Automatic (recommended)"},
    {"Compatibilidad (desactiva rutas opcionales rápidas)", "Compatibility (disables optional fast paths)"},
    {"Bibliotecas de pipelines", "Pipeline libraries"}, {"Según el controlador", "Driver default"},
    {"Desactivadas", "Disabled"}, {"Activadas si son compatibles", "Enabled when supported"},
    {"Presupuesto de memoria Vulkan", "Vulkan memory budget"},
    {"Estos cambios requieren reiniciar el juego.", "These changes require restarting the game."},
    {"Memoria local reportada", "Reported local memory"}, {"Memoria compartida / no local", "Shared / non-local memory"},
    {"Guardar", "Save"}, {"Volver al juego", "Back to the game"}, {"Guardado en xcde.ini", "Saved to xcde.ini"},
    {"Presupuesto de memoria (VMA / controlador)", "Memory budget (VMA / driver)"},
    {"GPU integrada: la memoria del sistema es compartida; el tamaño del heap Vulkan no equivale a VRAM dedicada.",
     "Integrated GPU: system memory is shared; the Vulkan heap size is not dedicated VRAM."},
    {"Selección y compatibilidad (al reiniciar)", "Selection and compatibility (after a restart)"},
    {"Culling por oclusión (al reiniciar)", "Occlusion culling (after a restart)"},
    {"Fix de parpadeo AMD (al reiniciar)", "AMD flicker fix (after a restart)"},
    {"Compatibilidad de sesión larga (al reiniciar)", "Long-session compatibility (after a restart)"},
    {"Automático (AMD integrada: apagada)", "Automatic (AMD integrated: off)"},
    {"Automático (RDNA 3: activado)", "Automatic (RDNA 3: on)"},
    {"Automático (desactivado)", "Automatic (off)"},
    {"Desactivada", "Off"}, {"Desactivado", "Off"}, {"Activada", "On"}, {"Activado", "On"},
    {"Salida de audio", "Audio output"},
    {"Automático (5.1 si es compatible)", "Auto (5.1 if supported)"},
    {"Estéreo (2.0)", "Stereo (2.0)"},
    {"Envolvente 5.1", "Surround 5.1"},
};
static const char *T(const char *es) {
    if (g_settings.ui_lang != 1) return es;
    for (const auto &e : k_en) if (!strcmp(e[0], es)) return e[1];
    return es;
}
struct TList { const char *p[8]; };
static TList TL(const char *const *a, int n) { TList t{}; for (int i = 0; i < n && i < 8; i++) t.p[i] = T(a[i]); return t; }

static const char *const k_res_ui[] = {"Igual que la ventana (recomendado)", "1x - 720p (como Switch)", "1.5x - 1080p",
                                       "2x - 1440p", "3x - 2160p", "75 % de la ventana", "50 % de la ventana",
                                       "Dinámica (según objetivo FPS)"};
static const int k_res_mode[] = {RES_MATCH, 1, RES_1_5, 2, 3, RES_Q75, RES_Q50, RES_DYNAMIC};

static void hotkey_combo(const char *label, int *scancode, int fallback, bool allow_none, bool en) {
    const char *cur = xcde_hotkey_label(*scancode, fallback, en);
    if (monado_begin_combo(label, cur)) {
        for (int i = allow_none ? 0 : 1; i < XCDE_HOTKEY_COUNT; i++) {
            const char *txt = i == 0 ? (en ? "None" : "Ninguna") : xcde_hotkey_choices[i].label;
            bool sel = (*scancode == xcde_hotkey_choices[i].scancode);
            if (ImGui::Selectable(txt, sel)) *scancode = xcde_hotkey_choices[i].scancode;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

static void ui(void) {
    static bool saved_note, dirty;
    Settings opening=g_settings, quality_before=g_settings;
    bool open_party = g_party_switch;
    int open_w = g_window_w, open_h = g_window_h, open_ml = g_mouse_look, open_ms = g_mouse_sens;
    bool open_mi = g_mouse_invert;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Size.x * 0.5f, vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // [XCDE] the font and spacing grow with the output height (font_h: 22 px per 1080 lines), so the window does too:
    // a fixed 940 x 720 crammed the menu at 1440p / 4K. 1080p and below keep 940 x 720.
    const float menu_scale = fmaxf(1.0f, vp->Size.y / 1080.0f);
    ImGui::SetNextWindowSize(ImVec2(fminf(vp->Size.x-24,940*menu_scale),fminf(vp->Size.y-24,720*menu_scale)), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin(T("Opciones  (F1 / Esc para volver al juego)"), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::BeginChild("options_body",ImVec2(0,-ImGui::GetFrameHeightWithSpacing()*2),ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);  // (short windows: every option reachable)
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem(T("Gráficos"))) {
            const bool en = g_settings.ui_lang != 0;
            if (monado_quality_controls(&g_settings,en)) {
                if (g_settings.upscaler_auto && dlss_available()) g_settings.upscaler = UPSCALE_DLSS;
                xcde_graphics_publish(&g_settings.graphics);
                xcde_foliage_set_preset(g_settings.grass_lod);
            }
            quality_before=g_settings;
            ImGui::SeparatorText(T("Pantalla"));
            if (ImGui::Checkbox(T("Pantalla completa"), &g_settings.fullscreen)) want_window = true;
            {
                const char *fs_modes[] = {en ? "Borderless (desktop)" : "Sin bordes (escritorio)",
                                          en ? "Exclusive fullscreen" : "Pantalla completa exclusiva"};
                int fm = g_settings.fullscreen_mode == FULLSCREEN_EXCLUSIVE ? 1 : 0;
                if (monado_combo(en ? "Fullscreen mode" : "Modo pantalla completa", &fm, fs_modes, 2)) {
                    g_settings.fullscreen_mode = fm ? FULLSCREEN_EXCLUSIVE : FULLSCREEN_BORDERLESS;
                    if (g_settings.fullscreen) want_window = true;
                }
                ImGui::SetItemTooltip("%s", en
                    ? "Borderless: instant Alt+Tab, uses desktop mode.\nExclusive: dedicated display mode (switches live when fullscreen)."
                    : "Sin bordes: Alt+Tab instantáneo, usa el modo del escritorio.\nExclusiva: modo dedicado de pantalla (cambia al instante en pantalla completa).");
            }
            {   // [XCDE] Monitor / display picker
                int ndisp = 0;
                SDL_DisplayID *disps = SDL_GetDisplays(&ndisp);
                char dpreview[128];
                if (g_settings.display_index < 0 || !disps || g_settings.display_index >= ndisp) {
                    snprintf(dpreview, sizeof dpreview, "%s", en ? "Automatic (current display)" : "Automático (pantalla actual)");
                } else {
                    const char *dn = SDL_GetDisplayName(disps[g_settings.display_index]);
                    snprintf(dpreview, sizeof dpreview, "%d: %s", g_settings.display_index + 1, dn ? dn : "Display");
                }
                if (monado_begin_combo(en ? "Display / Monitor" : "Monitor / Pantalla", dpreview)) {
                    if (ImGui::Selectable(en ? "Automatic (current display)" : "Automático (pantalla actual)", g_settings.display_index < 0))
                        g_settings.display_index = -1;
                    for (int i = 0; disps && i < ndisp && i < 16; i++) {
                        const char *dn = SDL_GetDisplayName(disps[i]);
                        char item[144];
                        snprintf(item, sizeof item, "%d: %s", i + 1, dn ? dn : "Display");
                        if (ImGui::Selectable(item, g_settings.display_index == i)) {
                            g_settings.display_index = i;
                            want_display = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                SDL_free(disps);
            }
            {   // [XCDE] same window size presets as the launcher, applied to the window at once
                int wi = xcde_window_size_index(g_window_w, g_window_h);
                char current[48];
                if (wi >= 0) snprintf(current, sizeof current, "%s", xcde_window_size_names[wi]);
                else snprintf(current, sizeof current, T("Personalizado (%dx%d)"), g_window_w, g_window_h);
                if (monado_begin_combo(T("Tamaño de la ventana"), current)) {
                    for (int i = 0; i < XCDE_WINDOW_SIZE_COUNT; i++) {
                        if (ImGui::Selectable(xcde_window_size_names[i], i == wi)) {
                            int w, h;
                            if (xcde_window_size_parse(xcde_window_size_names[i], &w, &h)) {
                                g_window_w = w;
                                g_window_h = h;
                                want_size = true;
                            }
                        }
                        if (i == wi) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("%s", g_settings.ui_lang
                    ? "The game window's size when it is not fullscreen (also saved for the next start).\nIn fullscreen it applies when you leave it."
                    : "Tamaño de la ventana del juego cuando no está a pantalla completa (también se guarda para el próximo inicio).\nA pantalla completa se aplica al salir de ella.");
            }
            const char *aspects[] = {g_settings.ui_lang ? "Automatic" : "Automático",
                                     g_settings.ui_lang ? "Original 16:9" : "16:9 original"};
            monado_combo(g_settings.ui_lang ? "Aspect ratio" : "Relación de aspecto", &g_settings.aspect, aspects, 2);
            {   // [XCDE] GitHub #121: HUD size, live (runtime/ui_scale.h)
                int hs = ui_scale_percent(g_settings.ui_scale);
                if (ImGui::SliderInt(g_settings.ui_lang ? "HUD size" : "Tamaño del HUD", &hs, UI_SCALE_MIN, 100, "%d %%"))
                    g_settings.ui_scale = ui_scale_percent(hs);
                ImGui::SetItemTooltip("%s", g_settings.ui_lang
                    ? "Shrinks the field and battle HUD towards its corners (minimap, party, arts palette, hints).\nMenus keep the game's own size. Applies at once."
                    : "Reduce el HUD de exploración y combate hacia sus esquinas (minimapa, grupo, paleta de artes, ayudas).\nLos menús mantienen el tamaño del juego. Se aplica al instante.");
                ImGui::Checkbox(en ? "Hide field HUD (screenshot mode)" : "Ocultar HUD de exploración (modo captura)", &g_settings.hide_hud);
            }
            ImGui::SeparatorText(T("Resolución y escalado"));
            int ri = 0;
            for (int i = 0; i < 8; i++) if (k_res_mode[i] == g_settings.res_mode) ri = i;
            if (monado_combo(T("Resolución interna"), &ri, TL(k_res_ui, 8).p, 8)) g_settings.res_mode = k_res_mode[ri];
            if ((g_settings.res_mode >= 1 && g_settings.res_mode <= 8) || g_settings.res_mode == RES_1_5)
                // [XCDE] GitHub #22: an Nx internal resolution keeps the window's shape only horizontally: in a wider-than-
                // 16:9 window (16:10, 21:9, 32:9) the fixed line count must stretch to fill it, and the image goes soft.
                ImGui::TextWrapped(g_settings.ui_lang
                    ? "An Nx resolution keeps the Switch's line count: in windows wider or taller than 16:9 it is stretched. \"Same as the window\" stays sharp."
                    : "Una resolución Nx mantiene las líneas de la Switch: en ventanas m\xC3\xA1s anchas o altas que 16:9 se estira. \xC2\xABIgual que la ventana\xC2\xBB lo mantiene n\xC3\xADtido.");
            static const char *ups[] = {"Bilineal (suave)", "AMD FSR 1.0 (nítido)", "AMD FSR 3.1 (temporal)", "NVIDIA DLSS (temporal, RTX)",
                                        "AMD FSR 4 Lite (temporal, nativo)"};
            static const int upv[] = {UPSCALE_BILINEAR, UPSCALE_FSR1, UPSCALE_FSR3, UPSCALE_DLSS, UPSCALE_FSR4_LITE};
            const int nups = 5;  // [XCDE] T-FSR4-LITE: the native component (Linux and Windows)
            int ui_ = 0;
            for (int i = 0; i < nups; i++) if (upv[i] == g_settings.upscaler) ui_ = i;
            const char *preview = g_settings.upscaler_auto ? (g_settings.ui_lang ? "Automatic (DLSS on supported RTX)" : "Automático (DLSS en RTX compatible)") : T(ups[ui_]);
            if (monado_begin_combo(T("Escalado / antialiasing"), preview)) {
                if (ImGui::Selectable(g_settings.ui_lang ? "Automatic (restart)" : "Automático (reiniciar)", g_settings.upscaler_auto)) {
                    g_settings.upscaler_auto = true;
                    if (dlss_available()) g_settings.upscaler = UPSCALE_DLSS;
                }
                for (int i = 0; i < nups; ++i) {
                    bool unavailable = i == 3 && !dlss_available();
                    ImGui::BeginDisabled(unavailable);
                    if (ImGui::Selectable(T(ups[i]), !g_settings.upscaler_auto && ui_ == i)) { g_settings.upscaler_auto = false; g_settings.upscaler = upv[i]; }
                    ImGui::EndDisabled();
                    if (unavailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", dlss_status());
                }
                ImGui::EndCombo();
            }
            if (g_settings.upscaler == UPSCALE_FSR1 || g_settings.upscaler == UPSCALE_FSR3 || g_settings.upscaler == UPSCALE_FSR4_LITE) {  // [XCDE] RCAS strength, live
                float sharp = xcde_sharpness(&g_settings);
                if (ImGui::SliderFloat(g_settings.ui_lang ? "Sharpening (RCAS)" : "Nitidez (RCAS)", &sharp, 0.0f, 1.0f, "%.2f"))
                    g_settings.upscale_sharpness = sharp;
                ImGui::SetItemTooltip("%s", g_settings.ui_lang ? "0 = off, 1 = maximum. Applies at once." : "0 = desactivada, 1 = máxima. Se aplica al instante.");
                if (g_settings.upscale_sharpness >= 0.0f) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(g_settings.ui_lang ? "Default" : "Predeterminada")) g_settings.upscale_sharpness = -1.0f;
                }
            }
            if (g_settings.upscaler == UPSCALE_FSR3 || g_settings.upscaler == UPSCALE_DLSS || g_settings.upscaler == UPSCALE_FSR4_LITE || (g_settings.upscaler_auto && dlss_available())) {
                static const char *uq[] = {"Nativo (solo antialiasing)", "Calidad (67 %)", "Equilibrado (59 %)", "Rendimiento (50 %)",
                                           "Ultra rendimiento (33 %)"};
                monado_combo(T("Calidad del escalado"), &g_settings.upscale_quality, TL(uq, 5).p, 5);
                ImGui::BeginDisabled(g_settings.upscaler != UPSCALE_FSR3);
                ImGui::Checkbox(T("Generación de frames (FSR 3; activarla por primera vez: al reiniciar)"), &g_settings.frame_gen);
                ImGui::EndDisabled();
            }
            // [XCDE] One place per option, grouped as in the launcher: anti-aliasing, shadows, detail, effects, indicators.
            ImGui::SeparatorText("Antialiasing");
            bool smaa = g_settings.antialias == AA_SMAA;
            if (ImGui::Checkbox(T("Antialiasing SMAA"), &smaa)) g_settings.antialias = smaa ? AA_SMAA : AA_OFF;
            ImGui::Checkbox(T("Antialiasing y desenfoque de movimiento del juego (al reiniciar)"), &g_settings.game_blur);
            {   // [XCDE] live (runtime/hle/graphics.c xcde_game_taa_off); a temporal upscaler replaces it
                bool by_upscaler = tmp_jitter_active();
                bool taa = !g_settings.taa_off && !by_upscaler;
                ImGui::BeginDisabled(by_upscaler);
                if (ImGui::Checkbox(T("Antialiasing temporal (TAA) del juego"), &taa)) g_settings.taa_off = !taa;
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("%s", by_upscaler
                        ? (en ? "Off while FSR 3 / FSR 4 / DLSS is selected: the upscaler does the anti-aliasing."
                              : "Desactivado con FSR 3 / FSR 4 / DLSS: el escalador hace el antialiasing.")
                        : (en ? "Applies at once. Smooths edges and small highlights; a little softer image."
                              : "Se aplica al instante. Suaviza bordes y brillos pequeños; imagen algo más suave."));
            }
            ImGui::SeparatorText(en ? "Shadows" : "Sombras");
            if (monado_gfx_group(&g_settings.graphics, gfx_ui_shadows, en)) xcde_graphics_publish(&g_settings.graphics);  // (live; the ones marked "restart" apply at the next start)
            ImGui::SeparatorText(en ? "Detail" : "Detalle");
            {   // [XCDE] grass draw distance: applied on the next foliage pass, no restart
                const char *const *labels = g_settings.ui_lang ? grass_lod_labels : grass_lod_labels_es;
                float grass_scale = xcde_foliage_requested_scale();
                int grass_choice = grass_lod_from_scale(grass_scale);
                char custom_grass[48];
                snprintf(custom_grass, sizeof custom_grass, g_settings.ui_lang ? "Custom (%.2fx)" : "Personalizada (%.2fx)", grass_scale);
                if (ImGui::BeginCombo(g_settings.ui_lang ? "Grass distance" : "Distancia de la hierba",
                                      grass_choice < 0 ? custom_grass : labels[grass_choice])) {
                    for (int i = 0; i < GRASS_PRESET_COUNT; ++i) {
                        if (ImGui::Selectable(labels[i], i == grass_choice)) { g_settings.grass_lod = i; xcde_foliage_set_preset(i); }
                        if (i == grass_choice) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("%s", g_settings.ui_lang
                    ? "Applies while playing, without restarting. Higher settings keep grass visible farther away and cost FPS.\n"
                      "Original keeps High's nearby detail at the game's original distance. Low halves grass reach.\n"
                      "Ultra (3x) draws much more grass and is the maximum."
                    : "Se aplica mientras juegas, sin reiniciar. Los valores altos mantienen la hierba visible más lejos y cuestan FPS.\n"
                      "Original conserva el detalle cercano de Alta a la distancia original del juego. Baja reduce el alcance a la mitad.\n"
                      "Ultra (3x) dibuja mucha más hierba y es el máximo.");
            }
            if (monado_gfx_group(&g_settings.graphics, gfx_ui_detail, en)) xcde_graphics_publish(&g_settings.graphics);
            ImGui::SeparatorText(en ? "Effects" : "Efectos");
            bool dof = !g_settings.dof_off;
            if (ImGui::Checkbox(T("Profundidad de campo"), &dof)) g_settings.dof_off = !dof;
            if (monado_gfx_group(&g_settings.graphics, gfx_ui_effects, en)) xcde_graphics_publish(&g_settings.graphics);
            ImGui::TextDisabled("%s", en ? "Original keeps the game's (and mods') own values."
                                         : "Original conserva los valores del juego (y de los mods).");
            ImGui::SeparatorText(en ? "Indicators" : "Indicadores");
            {   // [XCDE] GitHub #39: off / simple / detailed (perf_overlay.h), live
                const char *modes_en[] = {"Off", "Simple", "Detailed"}, *modes_es[] = {"Desactivado", "Simple", "Detallado"};
                int mode = !g_settings.fps_overlay ? 0 : g_settings.fps_overlay_style == FPS_OVERLAY_SIMPLE ? 1 : 2;
                if (monado_combo(g_settings.ui_lang ? "Performance overlay" : "Superposición de rendimiento", &mode,
                                 g_settings.ui_lang ? modes_en : modes_es, 3)) {
                    g_settings.fps_overlay = mode != 0;
                    if (mode) g_settings.fps_overlay_style = mode == 1 ? FPS_OVERLAY_SIMPLE : FPS_OVERLAY_DETAILED;
                }
                ImGui::SetItemTooltip("%s", g_settings.ui_lang
                    ? "Simple: FPS and frame time. Detailed: frame-time graph, 1% / 0.1% lows, GPU time and waits, GPU / CPU load, memory and render resolution."
                    : "Simple: FPS y tiempo por frame. Detallado: gráfica de tiempos, mínimos 1% / 0.1%, tiempo y esperas de GPU, carga de GPU / CPU, memoria y resolución.");
                if (mode) {  // [XCDE] GitHub #147: size and corner, live
                    bool en = g_settings.ui_lang != 0;
                    int sc = perf_overlay_scale_percent(g_settings.fps_overlay_scale);
                    if (ImGui::SliderInt(en ? "Overlay size" : "Tamaño de la superposición", &sc, PERF_OVERLAY_SCALE_MIN, PERF_OVERLAY_SCALE_MAX, "%d %%"))
                        g_settings.fps_overlay_scale = perf_overlay_scale_percent(sc);
                    const char *pos_en[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
                    const char *pos_es[] = {"Arriba a la izquierda", "Arriba a la derecha", "Abajo a la izquierda", "Abajo a la derecha"};
                    int pos = perf_overlay_pos_valid(g_settings.fps_overlay_pos);
                    if (monado_combo(en ? "Overlay position" : "Posición de la superposición", &pos, en ? pos_en : pos_es, PERF_POS_COUNT))
                        g_settings.fps_overlay_pos = pos;
                }
            }
            ImGui::Checkbox(T("Mostrar FPS en la barra de título"), &g_settings.show_fps);
            ImGui::SeparatorText(T("Velocidad"));
            if (ImGui::Checkbox(T("V-Sync (con 30 / 60 FPS)"), &g_settings.vsync)) vk_request_swapchain_recreate();
            {   // [XCDE] Cutscenes FPS mode (parity with launcher)
                const char *cut_en[] = {"Same as game", "Steady 30 FPS (like Switch)", "30 FPS + Frame Gen (restart)"};
                const char *cut_es[] = {"Como el resto del juego", "30 estables (como en Switch)", "30 FPS + Frame Gen (al reiniciar)"};
                int cm = g_settings.cutscene_mode < 0 || g_settings.cutscene_mode > 2 ? 0 : g_settings.cutscene_mode;
                if (monado_combo(en ? "Cutscenes" : "Cinemáticas", &cm, en ? cut_en : cut_es, 3)) g_settings.cutscene_mode = cm;
            }
            {   // [XCDE] Background FPS cap when unfocused
                const char *bg_en[] = {"Off (uncapped)", "15 FPS", "30 FPS"};
                const char *bg_es[] = {"Desactivado", "15 FPS", "30 FPS"};
                int bi = g_settings.bg_fps == 15 ? 1 : g_settings.bg_fps == 30 ? 2 : 0;
                if (monado_combo(en ? "Background FPS limit" : "Límite de FPS en segundo plano", &bi, en ? bg_en : bg_es, 3))
                    g_settings.bg_fps = bi == 1 ? 15 : bi == 2 ? 30 : 0;
                ImGui::SetItemTooltip("%s", en ? "Caps frame rate while the window is unfocused to reduce GPU/CPU load."
                                               : "Limita los FPS cuando la ventana está en segundo plano para reducir la carga.");
            }
            if (ImGui::Button(g_settings.ui_lang ? "Restore graphics" : "Restaurar gráficos")) {
                xcde_quality_apply(&g_settings,QUALITY_BALANCED);
                if (g_settings.upscaler_auto && dlss_available()) g_settings.upscaler = UPSCALE_DLSS;
                g_settings.upscale_sharpness = -1.0f;
                xcde_graphics_publish(&g_settings.graphics);
                xcde_foliage_set_preset(g_settings.grass_lod);
                quality_before=g_settings;
            }
            ImGui::SeparatorText(en ? "Startup" : "Inicio");
            ImGui::Checkbox(en ? "Skip launcher on startup" : "Omitir launcher al iniciar", &g_settings.skip_launcher);
            ImGui::SetItemTooltip("%s", en
                ? "Starts the game directly on launch instead of opening the launcher.\nUntick and click Apply to show the launcher again on the next launch."
                : "Inicia el juego directamente al abrir en lugar de mostrar el launcher.\nDesmárcalo y pulsa Aplicar para volver a mostrar el launcher en el próximo inicio.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("GPU / Vulkan"))) {
            ImGui::SeparatorText(T("Adaptador Vulkan activo"));
            ImGui::TextWrapped("%s", g_vk.props.deviceName);
            const char *vendor = g_vk.props.vendorID == 0x1002 ? "AMD" : g_vk.props.vendorID == 0x10de ? "NVIDIA" :
                                 g_vk.props.vendorID == 0x8086 ? "Intel" : "Unknown";
            ImGui::Text("%s | vendor 0x%04x | device 0x%04x", vendor, g_vk.props.vendorID, g_vk.props.deviceID);
            ImGui::TextWrapped("Driver %u: %s (%s)", g_vk.props.driverVersion,
                               g_vk.driver_props.driverName, g_vk.driver_props.driverInfo);
            ImGui::Text("Vulkan %u.%u.%u | adapter index %u", VK_API_VERSION_MAJOR(g_vk.props.apiVersion),
                        VK_API_VERSION_MINOR(g_vk.props.apiVersion), VK_API_VERSION_PATCH(g_vk.props.apiVersion), g_vk.phys_index);
            VkPhysicalDeviceMemoryProperties mp=g_vk.memory_props;
            VkDeviceSize local = 0, shared = 0;
            for (uint32_t i = 0; i < mp.memoryHeapCount; ++i) {
                bool host_visible = false;
                for (uint32_t t = 0; t < mp.memoryTypeCount; ++t)
                    if (mp.memoryTypes[t].heapIndex == i && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                        host_visible = true;
                if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && !host_visible) local += mp.memoryHeaps[i].size;
                else shared += mp.memoryHeaps[i].size;
            }
            ImGui::Text("%s: %.0f MiB", T("Memoria local reportada"), (double)local / (1024.0 * 1024.0));
            ImGui::Text("%s: %.0f MiB", T("Memoria compartida / no local"), (double)shared / (1024.0 * 1024.0));
            if (g_vk.vma) {
                static VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
                static uint64_t budget_at; uint64_t now=SDL_GetTicksNS();
                if (!budget_at || now-budget_at>=500000000ull) { vmaGetHeapBudgets(g_vk.vma, budgets); budget_at=now; }
                ImGui::SeparatorText(T("Presupuesto de memoria (VMA / controlador)"));
                for (uint32_t i = 0; i < mp.memoryHeapCount; ++i) {
                    const double used_mb = (double)budgets[i].usage / (1024.0 * 1024.0);
                    const double budget_mb = (double)budgets[i].budget / (1024.0 * 1024.0);
                    const double ours_mb = (double)budgets[i].statistics.allocationBytes / (1024.0 * 1024.0);
                    if (budget_mb > 0.0) {
                        ImGui::Text("Heap %u%s: %.0f / %.0f MiB | Monado: %.0f MiB", i,
                                    (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (local)" : " (host)",
                                    used_mb, budget_mb, ours_mb);
                        const float pressure = (float)(budgets[i].usage / (double)budgets[i].budget);
                        ImGui::ProgressBar(pressure < 0.0f ? 0.0f : pressure > 1.0f ? 1.0f : pressure,
                                           ImVec2(-1.0f, 0.0f), "");
                    } else {
                        ImGui::Text("Heap %u: presupuesto no disponible | Monado: %.0f MiB", i, ours_mb);
                    }
                }
            }
            if (g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
                ImGui::TextWrapped(T("GPU integrada: la memoria del sistema es compartida; el tamaño del heap Vulkan no equivale a VRAM dedicada."));
            ImGui::SeparatorText(T("Selección y compatibilidad (al reiniciar)"));
            xcde_gpu_picker(g_settings.ui_lang ? "Graphics adapter" : "Adaptador gráfico", g_settings.gpu_uuid,
                            sizeof g_settings.gpu_uuid, &g_settings.gpu_index, g_vk_adapters, g_vk_adapter_count, g_settings.ui_lang != 0);
            const XcdeGpuInfo *selected_gpu = xcde_gpu_preference(g_vk_adapters, g_vk_adapter_count, g_settings.gpu_uuid, g_settings.gpu_index);
            xcde_vulkan_picker(selected_gpu, g_vk.loader_api_version, &g_settings.vulkan_api, g_settings.ui_lang != 0);
            ImGui::Text(g_settings.ui_lang ? "Active route: Vulkan %u.%u" : "Ruta activa: Vulkan %u.%u",
                        VK_API_VERSION_MAJOR(g_vk.api_version), VK_API_VERSION_MINOR(g_vk.api_version));
            ImGui::TextWrapped("DLSS: %s", dlss_status());
            ImGui::Checkbox(g_settings.ui_lang ? "Restore heavy AMD volume effects (restart)" : "Restaurar efectos volumétricos AMD (reiniciar)", &g_settings.gpu_keep_heavy);
            if (g_vk.props.vendorID == 0x1002 && g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
                ImGui::TextWrapped(g_settings.ui_lang ? "Three expensive volume effects are disabled by default on AMD integrated GPUs." :
                    "Tres efectos volumétricos costosos se desactivan por defecto en las GPU integradas AMD.");
            static const char *profiles[] = {"Automático (recomendado)", "Compatibilidad (desactiva rutas opcionales rápidas)"};
            int profile = g_settings.gpu_profile == GPU_PROFILE_COMPATIBILITY ? 1 : 0;
            if (monado_combo(T("Perfil gráfico"), &profile, TL(profiles, 2).p, 2))
                g_settings.gpu_profile = profile ? GPU_PROFILE_COMPATIBILITY : GPU_PROFILE_AUTO;
            static const char *gpl_modes[] = {"Según el controlador", "Desactivadas", "Activadas si son compatibles"};
            int gpl = g_settings.gpu_gpl == GPU_GPL_OFF ? 1 : g_settings.gpu_gpl == GPU_GPL_ON ? 2 : 0;
            if (monado_combo(T("Bibliotecas de pipelines"), &gpl, TL(gpl_modes, 3).p, 3))
                g_settings.gpu_gpl = gpl == 1 ? GPU_GPL_OFF : gpl == 2 ? GPU_GPL_ON : GPU_GPL_AUTO;
            // [XCDE][FSR3] GitHub #14: occlusion culling misresolves on AMD integrated GPUs (the floor-vanishing
            // class of bug, oq_enabled in exec.c); automatic = off there, on elsewhere, overridable from here.
            static const char *occl_modes[] = {"Automático (AMD integrada: apagada)", "Desactivada", "Activada"};
            int occl = g_settings.occlusion == OCCL_OFF ? 1 : g_settings.occlusion == OCCL_ON ? 2 : 0;
            if (monado_combo(T("Culling por oclusión (al reiniciar)"), &occl, TL(occl_modes, 3).p, 3))
                g_settings.occlusion = occl == 1 ? OCCL_OFF : occl == 2 ? OCCL_ON : OCCL_AUTO;
            // [XCDE] issue #7: confirmed on RDNA 3 (RX 7800 XT and Ryzen 8600G iGPU); automatic engages the
            // feedback-snapshot path only there (see LARECOMP_FEEDBACK_DIRECT handling in exec.c).
            static const char *amd_modes[] = {"Automático (RDNA 3: activado)", "Desactivado", "Activado"};
            int amdfix = g_settings.amd_fix == AMD_FIX_OFF ? 1 : g_settings.amd_fix == AMD_FIX_ON ? 2 : 0;
            if (monado_combo(T("Fix de parpadeo AMD (al reiniciar)"), &amdfix, TL(amd_modes, 3).p, 3))
                g_settings.amd_fix = amdfix == 1 ? AMD_FIX_OFF : amdfix == 2 ? AMD_FIX_ON : AMD_FIX_AUTO;
            // [XCDE] issue #25: long-session corruption. Conservative mode: no async render worker.
            static const char *ss_modes[] = {"Automático (desactivado)", "Desactivado", "Activado"};
            int ssfix = g_settings.session_fix == SESSION_FIX_OFF ? 1 : g_settings.session_fix == SESSION_FIX_ON ? 2 : 0;
            if (monado_combo(T("Compatibilidad de sesión larga (al reiniciar)"), &ssfix, TL(ss_modes, 3).p, 3))
                g_settings.session_fix = ssfix == 1 ? SESSION_FIX_OFF : ssfix == 2 ? SESSION_FIX_ON : SESSION_FIX_AUTO;
            ImGui::Checkbox(T("Presupuesto de memoria Vulkan"), &g_settings.gpu_memory_budget);
            ImGui::TextDisabled(T("Estos cambios requieren reiniciar el juego."));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("Audio"))) {
            const bool en = g_settings.ui_lang != 0;
            int vol = (int)lroundf(g_settings.volume * 100);
            if (ImGui::SliderInt(T("Volumen"), &vol, 0, 200, "%d %%")) g_settings.volume = vol / 100.0f;
            {   // [XCDE] Audio output device picker (live hot-swap)
                int ndev = 0;
                SDL_AudioDeviceID *devs = SDL_GetAudioPlaybackDevices(&ndev);
                const char *cur = g_settings.audio_device[0] ? g_settings.audio_device : (en ? "System default" : "Predeterminado del sistema");
                if (monado_begin_combo(en ? "Audio device" : "Dispositivo de audio", cur)) {
                    if (ImGui::Selectable(en ? "System default" : "Predeterminado del sistema", !g_settings.audio_device[0])) {
                        audio_select_device("");
                    }
                    for (int i = 0; devs && i < ndev; i++) {
                        const char *dn = SDL_GetAudioDeviceName(devs[i]);
                        if (!dn || !*dn) continue;
                        bool sel = !strcmp(g_settings.audio_device, dn);
                        if (ImGui::Selectable(dn, sel)) {
                            audio_select_device(dn);
                        }
                    }
                    ImGui::EndCombo();
                }
                SDL_free(devs);
            }
            static const char *audio_modes[] = {"Automático (5.1 si es compatible)", "Estéreo (2.0)", "Envolvente 5.1"};
            int am = audio_out_valid(g_settings.audio_channels);
            if (monado_combo(T("Salida de audio"), &am, TL(audio_modes, AUDIO_OUT_COUNT).p, AUDIO_OUT_COUNT)) {
                audio_select_channels(am);
            }
            ImGui::SetItemTooltip("%s", g_settings.ui_lang
                ? "Auto uses 5.1 LPCM surround on 5.1/7.1 devices and Stereo otherwise.\nThe Switch game mixes up to 5.1 (SDL maps 5.1 onto 7.1 devices). Applies live."
                : "Automático usa sonido envolvente 5.1 LPCM en dispositivos 5.1/7.1 y estéreo en el resto.\nEl juego de Switch mezcla hasta 5.1 (SDL adapta 5.1 a dispositivos 7.1). Se aplica al instante.");
            ImGui::Checkbox(en ? "Mute audio when unfocused" : "Silenciar audio en segundo plano", &g_settings.mute_unfocused);
            ImGui::Checkbox(T("Audio de los vídeos (al reiniciar)"), &g_settings.movie_audio);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("Controles"))) {
            const bool en = g_settings.ui_lang != 0;
            ImGui::TextWrapped("%s", en
                ? "Full button/key remapping is in the launcher's Controls tab. The options below apply live."
                : "La asignación completa de teclas y botones está en la pestaña Controles del launcher. Las opciones de abajo se aplican al instante.");
            ImGui::SeparatorText(en ? "Gamepad & Camera" : "Mando y cámara");
            ImGui::Checkbox(en ? "Gamepad vibration" : "Vibración del mando", &g_settings.input_rumble);
            ImGui::Checkbox(en ? "XC3-style party switch (L2 + L1/R1 or hotkey)" : "Cambio de personaje estilo XC3 (L2 + L1/R1 o atajo)", &g_party_switch);
            ImGui::SliderInt(en ? "Stick deadzone (%)" : "Zona muerta de sticks (%)", &g_settings.stick_deadzone, 0, 30);
            {
                const char *ml_en[] = {"Off", "Hold right mouse button", "Always"};
                const char *ml_es[] = {"No", "Manteniendo el botón derecho", "Siempre"};
                monado_combo(en ? "Mouse camera" : "Cámara con el ratón", &g_mouse_look, en ? ml_en : ml_es, 3);
            }
            ImGui::BeginDisabled(!g_mouse_look);
            ImGui::SliderInt(en ? "Mouse sensitivity" : "Sensibilidad del ratón", &g_mouse_sens, 1, 100);
            ImGui::Checkbox(en ? "Invert mouse Y-axis" : "Invertir eje Y del ratón", &g_mouse_invert);
            ImGui::EndDisabled();
            ImGui::SeparatorText(en ? "Hotkeys" : "Atajos de teclado");
            hotkey_combo(en ? "Overlay menu key" : "Tecla del menú de opciones", &g_settings.hotkey_menu, XCDE_KEY_F1, false, en);
            ImGui::Checkbox(en ? "Open overlay with Start + Select" : "Abrir menú con Start + Select", &g_settings.menu_pad_combo);
            hotkey_combo(en ? "Party switch key" : "Tecla de cambio de personaje", &g_settings.hotkey_party, XCDE_KEY_F2, true, en);
            hotkey_combo(en ? "Screenshot key" : "Tecla de captura de pantalla", &g_settings.hotkey_screenshot, XCDE_KEY_F12, true, en);
            hotkey_combo(en ? "Toggle field HUD key" : "Tecla para ocultar/mostrar HUD", &g_settings.hotkey_hud, XCDE_KEY_F4, true, en);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("Trucos"))) {
            static const char *rs_ui[] = {"Normal (x1)", "Rápida (x1.5)", "Muy rápida (x2)"};
            static const float rs_val[] = {1.0f, 1.5f, 2.0f};
            int rsi = g_settings.run_speed >= 1.75f ? 2 : g_settings.run_speed >= 1.25f ? 1 : 0;
            if (monado_combo(T("Velocidad al correr"), &rsi, TL(rs_ui, 3).p, 3)) g_settings.run_speed = rs_val[rsi];
            ImGui::SetItemTooltip("%s", g_settings.ui_lang
                ? "Multiplies the controlled character's movement speed (in battle too). The game still resolves the ground and collisions."
                : "Multiplica la velocidad del personaje controlado (también en combate). El juego sigue resolviendo el suelo y las colisiones.");
            if (monado_cheat_controls(&g_settings, g_settings.ui_lang == 1, true)) xcde_cheats_publish(&g_settings);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    if (g_settings.quality_preset==quality_before.quality_preset && !xcde_quality_equal(xcde_quality_current(&quality_before),xcde_quality_current(&g_settings))) { g_settings.quality_preset=QUALITY_CUSTOM; saved_note=false; dirty=true; }
    if (memcmp(&opening,&g_settings,sizeof g_settings)!=0 || open_party!=g_party_switch || open_w!=g_window_w ||
        open_h!=g_window_h || open_ml!=g_mouse_look || open_ms!=g_mouse_sens || open_mi!=g_mouse_invert) {
        saved_note = false;
        dirty = true;
    }
    ImGui::Separator();
    if (ImGui::Button(g_settings.ui_lang ? "Apply" : "Aplicar")) {
        saved_note = settings_save();
        dirty = !saved_note;
        if (dirty) menu_toast(g_settings.ui_lang ? "Could not save settings" : "No se pudieron guardar los ajustes");
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Volver al juego"))) { open_ = false; input_reset(); }
    ImGui::SameLine();
    {   // [XCDE] GitHub #22: a close-the-game option where the settings are (a confirm: press again within 2 s).
        static Uint32 asked_at;
        static bool sure;
        if (ImGui::Button(g_settings.ui_lang ? "Exit the game" : "Salir del juego")) {
            if (sure) { settings_save(); plat_terminate(); }
            else { asked_at = SDL_GetTicks(); sure = true; }
        }
        if (sure && SDL_GetTicks() - asked_at > 2000) sure = false;  // (timed out)
        if (sure) { ImGui::SameLine(); ImGui::TextDisabled(g_settings.ui_lang ? "Sure? press again." : "\xC2\xBFSeguro? pulsa otra vez."); }
    }
    if (saved_note) { ImGui::SameLine(); ImGui::TextDisabled(T("Guardado en xcde.ini")); }
    else if (dirty) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.28f, 1.0f), "%s", g_settings.ui_lang ? "Unsaved changes (click Apply)" : "Cambios sin guardar (pulsa Aplicar)"); }
    ImGui::End();
}

// [XCDE] Launch hint: for the first seconds of play, a small note at the top that F1 (Start + Select on a controller)
// opens the settings overlay. It fades out, and goes away for good once the menu has been opened.
static const uint64_t HINT_NS = 7000000000ull, HINT_FADE_NS = 1000000000ull;
static uint64_t hint_t0;
static bool hint_done;

static void launch_hint(float s, uint64_t now) {
    uint64_t t = now - hint_t0;
    float a = t + HINT_FADE_NS > HINT_NS ? (float)(HINT_NS - t) / HINT_FADE_NS : 1.0f;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Size.x * 0.5f, 24 * s), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f * a);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s, 10 * s));
    ImGui::Begin("##launch-hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    const char *mk = xcde_hotkey_label(g_settings.hotkey_menu, XCDE_KEY_F1, g_settings.ui_lang != 0);
    if (g_settings.menu_pad_combo)
        ImGui::Text(g_settings.ui_lang ? "Press %s (or Start + Select) to open the settings overlay"
                                       : "Pulsa %s (o Start + Select) para abrir el men\xC3\xBA de opciones", mk);
    else
        ImGui::Text(g_settings.ui_lang ? "Press %s to open the settings overlay"
                                       : "Pulsa %s para abrir el men\xC3\xBA de opciones", mk);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

static void draw_toast(float s, uint64_t now) {
    char msg[320];
    uint64_t rem = 0;
    {
        std::lock_guard<std::mutex> g(toast_lock);
        if (!toast_until_ns || now >= toast_until_ns) return;
        rem = toast_until_ns - now;
        snprintf(msg, sizeof msg, "%s", toast_msg);
    }
    float a = rem < TOAST_FADE_NS ? (float)rem / (float)TOAST_FADE_NS : 1.0f;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Size.x * 0.5f, 24 * s), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.88f * a);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s, 10 * s));
    ImGui::Begin("##toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextUnformatted(msg);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

extern "C" void menu_render(void *cmd, uint32_t w, uint32_t h) {
    static ImGuiContext *ctx;
    static float font_h;
    static uint64_t t_last;
    bool show_counter = g_settings.fps_overlay && !diag_deterministic();
    uint64_t now = SDL_GetTicksNS();
    bool show_toast = false;
    {
        std::lock_guard<std::mutex> g(toast_lock);
        show_toast = toast_until_ns && now < toast_until_ns;
    }
    if (!hint_done) {
        if (!hint_t0) hint_t0 = now;
        if (open_ || diag_deterministic() || now - hint_t0 >= HINT_NS) hint_done = true;
    }
    if (!open_ && !show_counter && hint_done && !show_toast) { perf_overlay_draw(false, 1, w, h); return; }  // (stops its GPU timing / sampling)
    if (!ctx) {
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        font_h = fmaxf(16.0f, h / 1080.0f * 22.0f);
        {   // [XCDE] Prefer a fontconfig-resolved scalable font on Linux; retain known paths as fallbacks.
            // Check paths before loading: older ImGui versions may assert when a requested font is missing.
#ifdef _WIN32
            static const char *fonts[] = {"C:\\Windows\\Fonts\\segoeui.ttf"};
#else
            static bool fc_checked = false;
            static std::string fc_body;
            if (!fc_checked) {
                fc_checked = true;
                fc_body = fc_match_font("sans-serif:weight=regular");
            }
            static const char *fonts[] = {
                "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf", "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
                "/usr/share/fonts/adwaita-sans-fonts/AdwaitaSans-Regular.ttf",
                "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
                "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf"
            };
#endif
            ImFont *font = nullptr;
            const char *font_path = nullptr;
#ifndef _WIN32
            if (!fc_body.empty()) {
                font = io.Fonts->AddFontFromFileTTF(fc_body.c_str(), font_h, nullptr, io.Fonts->GetGlyphRangesDefault());
                if (font) font_path = fc_body.c_str();
            }
#endif
            if (!font) {
                for (const char *f : fonts) {
                    SDL_PathInfo info;
                    if (!SDL_GetPathInfo(f, &info)) continue;
                    if ((font = io.Fonts->AddFontFromFileTTF(f, font_h, nullptr, io.Fonts->GetGlyphRangesDefault()))) {
                        font_path = f;
                        break;
                    }
                }
            }
            if (font_path)
                perf_font_big = io.Fonts->AddFontFromFileTTF(font_path, font_h * 2.0f, nullptr,
                                                             io.Fonts->GetGlyphRangesDefault());  // (overlay FPS)
            else
                io.Fonts->AddFontDefault();
        }
        // [XCDE] the launcher's look (navy panels, crimson selection, teal accents), as a floating window over the game
        monado_launcher_theme(font_h / 16.0f);
        ImGui::GetStyle().WindowRounding = 12.0f * font_h / 16.0f;
        ImGui::GetStyle().WindowBorderSize = 1.0f;
        if (!vk_imgui_backend_init()) { ImGui::DestroyContext(ctx); ctx = nullptr; open_ = false; hint_done = true; return; }
        ImGui_ImplVulkan_CreateFontsTexture();
    }
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime = t_last ? (float)((now - t_last) / 1e9) : 1.0f / 60;
    if (io.DeltaTime <= 0) io.DeltaTime = 1.0f / 60;
    t_last = now;
    if (open_) feed(io, w, h);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (open_) ui();
    want_text_input = open_ && io.WantTextInput;
    perf_overlay_draw(show_counter, font_h / 16.0f, w, h);  // [XCDE] GitHub #39: simple / detailed (perf_overlay.h)
    if (!hint_done && !open_ && !show_toast) launch_hint(font_h / 16.0f, now);
    if (show_toast) draw_toast(font_h / 16.0f, now);
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);
}
