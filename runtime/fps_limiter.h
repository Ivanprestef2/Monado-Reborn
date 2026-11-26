// [XCDE] FPS target validation and RTSS / MangoHud "early" style host frame limiter policy.
// Shared by settings_ini.c, framepace.c, quality_controls.h, tools/launcher/launcher.cpp, and native tests.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { XCDE_FPS_MIN = 15, XCDE_FPS_MAX = 1000 };
enum { FPS_LIMITER_DEFAULT = 0, FPS_LIMITER_EARLY = 1, FPS_LIMITER_COUNT = 2 };

static const char *const xcde_fps_limiter_ini[FPS_LIMITER_COUNT] = {"default", "early"};

static inline int xcde_fps_clamp(int fps) {
    if (fps <= 0) return 0;
    if (fps < XCDE_FPS_MIN) return XCDE_FPS_MIN;
    if (fps > XCDE_FPS_MAX) return XCDE_FPS_MAX;
    return fps;
}

static inline int xcde_fps_parse(const char *v) {
    if (!v || !*v) return 30;
    if (!strcmp(v, "unlimited") || !strcmp(v, "0") || !strcmp(v, "unl\xc3\xadmited") || !strcmp(v, "ilimitado")) return 0;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < 0) return 30;
    if (n == 0) return 0;
    if (n > XCDE_FPS_MAX) return XCDE_FPS_MAX;
    return xcde_fps_clamp((int)n);
}

static inline int xcde_fps_limiter_valid(int mode) {
    return mode == FPS_LIMITER_EARLY ? FPS_LIMITER_EARLY : FPS_LIMITER_DEFAULT;
}

static inline int xcde_fps_limiter_parse(const char *v) {
    if (v && (!strcmp(v, "early") || !strcmp(v, "rtss") || !strcmp(v, "1"))) return FPS_LIMITER_EARLY;
    return FPS_LIMITER_DEFAULT;
}

// Exact rounded frame period in nanoseconds for a positive integer FPS target.
static inline uint64_t xcde_fps_period_ns(int fps) {
    if (fps <= 0) return 0;
    return (1000000000ull + (uint64_t)fps / 2) / (uint64_t)fps;
}

// Fixed-cadence "early" limiter state (RTSS / MangoHud style):
// - When ahead of schedule, returns the exact nanosecond deadline to sleep+spin until and advances
//   the deadline by one period (zero cumulative wakeup drift).
// - When behind schedule (GPU-bound or hitch), returns 0 (no wait) and re-anchors to now + period
//   so hitches never trigger short burst-catchup frames.
typedef struct {
    uint64_t next_deadline_ns;
    int last_fps;
} XcdeEarlyLimiter;

static inline void xcde_early_limiter_reset(XcdeEarlyLimiter *lim) {
    lim->next_deadline_ns = 0;
    lim->last_fps = 0;
}

static inline uint64_t xcde_early_limiter_step(XcdeEarlyLimiter *lim, uint64_t now_ns, int target_fps) {
    if (target_fps <= 0) {
        xcde_early_limiter_reset(lim);
        return 0;
    }
    uint64_t period_ns = xcde_fps_period_ns(target_fps);
    if (!lim->next_deadline_ns || lim->last_fps != target_fps) {
        lim->last_fps = target_fps;
        lim->next_deadline_ns = now_ns + period_ns;
        return 0;
    }
    if (now_ns < lim->next_deadline_ns) {
        uint64_t wake_ns = lim->next_deadline_ns;
        lim->next_deadline_ns += period_ns;
        return wake_ns;
    }
    lim->next_deadline_ns = now_ns + period_ns;
    return 0;
}

// Hybrid present-aligned pacer (zero frame lag / 0 extra frames in flight):
// - Paces frame start to (next_present_ns - lead_ns) so input is polled as late as possible.
// - Absorbs frame-to-frame work-time variance (e.g. moving vs standing still) right before
//   vkQueuePresentKHR so present-to-present intervals stay flat without adding a queued frame.
// - When GPU/CPU-bound (work >= period), waits 0 ns and re-anchors cleanly without burst catch-up.
enum { XCDE_PACER_WINDOW = 16, XCDE_PACER_INFLIGHT = 4 };
typedef struct {
    uint64_t next_present_ns;
    uint64_t frame_start_ns;
    uint64_t inflight_start[XCDE_PACER_INFLIGHT];
    uint32_t inflight_head;
    uint32_t inflight_count;
    uint64_t work_hist[XCDE_PACER_WINDOW];
    uint32_t work_idx;
    uint32_t work_count;
    int last_fps;
} XcdePresentPacer;

static inline void xcde_present_pacer_reset(XcdePresentPacer *p) {
    memset(p, 0, sizeof *p);
}

static inline uint64_t xcde_present_pacer_lead_ns(const XcdePresentPacer *p, uint64_t period_ns) {
    if (!p->work_count) return period_ns / 2;
    uint64_t max_work = 0;
    for (uint32_t i = 0; i < p->work_count; i++)
        if (p->work_hist[i] > max_work) max_work = p->work_hist[i];
    uint64_t lead = max_work + 1000000ull;  // +1.0 ms headroom for sudden movement/draw spikes
    uint64_t min_lead = period_ns / 4;
    if (lead < min_lead) lead = min_lead;
    if (lead > period_ns) lead = period_ns;
    return lead;
}

static inline void xcde_present_pacer_push_start(XcdePresentPacer *p, uint64_t start_ns) {
    p->frame_start_ns = start_ns;
    if (p->inflight_count == XCDE_PACER_INFLIGHT) {
        p->inflight_head = (p->inflight_head + 1u) % XCDE_PACER_INFLIGHT;
        p->inflight_count--;
    }
    uint32_t slot = (p->inflight_head + p->inflight_count) % XCDE_PACER_INFLIGHT;
    p->inflight_start[slot] = start_ns;
    p->inflight_count++;
}

static inline uint64_t xcde_present_pacer_pop_start(XcdePresentPacer *p) {
    if (!p->inflight_count) return p->frame_start_ns;
    uint64_t start_ns = p->inflight_start[p->inflight_head];
    p->inflight_head = (p->inflight_head + 1u) % XCDE_PACER_INFLIGHT;
    p->inflight_count--;
    return start_ns;
}

static inline uint64_t xcde_present_pacer_frame_start(XcdePresentPacer *p, uint64_t now_ns, int target_fps) {
    if (target_fps <= 0) {
        xcde_present_pacer_reset(p);
        return 0;
    }
    uint64_t period_ns = xcde_fps_period_ns(target_fps);
    if (!p->next_present_ns || p->last_fps != target_fps) {
        xcde_present_pacer_reset(p);
        p->last_fps = target_fps;
        p->next_present_ns = now_ns + period_ns;
        xcde_present_pacer_push_start(p, now_ns);
        return 0;
    }
    // [XCDE] Issue #261: with async vk-render, frame N+1 may call frame_start() before frame N calls
    // before_present(). Never reset next_present_ns while an earlier frame is still in flight (doing so
    // would push frame N's present deadline forward by a full period and cause a double-frame stutter).
    uint64_t target_present = p->next_present_ns + (uint64_t)p->inflight_count * period_ns;
    if (now_ns >= target_present) {
        if (!p->inflight_count) p->next_present_ns = now_ns + period_ns;
        xcde_present_pacer_push_start(p, now_ns);
        return 0;
    }
    uint64_t lead_ns = xcde_present_pacer_lead_ns(p, period_ns);
    uint64_t start_target = target_present > lead_ns ? target_present - lead_ns : 0;
    if (now_ns < start_target) {
        xcde_present_pacer_push_start(p, start_target);
        return start_target;
    }
    xcde_present_pacer_push_start(p, now_ns);
    return 0;
}

static inline uint64_t xcde_present_pacer_before_present(XcdePresentPacer *p, uint64_t now_ns, int target_fps) {
    if (target_fps <= 0) {
        xcde_present_pacer_reset(p);
        return 0;
    }
    uint64_t period_ns = xcde_fps_period_ns(target_fps);
    uint64_t start_ns = xcde_present_pacer_pop_start(p);
    if (start_ns && now_ns >= start_ns) {
        uint64_t work_ns = now_ns - start_ns;
        if (work_ns < 250000000ull) {
            p->work_hist[p->work_idx] = work_ns;
            p->work_idx = (p->work_idx + 1) % XCDE_PACER_WINDOW;
            if (p->work_count < XCDE_PACER_WINDOW) p->work_count++;
        }
    }
    if (!p->next_present_ns || p->last_fps != target_fps) {
        p->last_fps = target_fps;
        p->next_present_ns = now_ns + period_ns;
        return 0;
    }
    if (now_ns < p->next_present_ns) {
        uint64_t wake_ns = p->next_present_ns;
        p->next_present_ns += period_ns;
        return wake_ns;
    }
    p->next_present_ns = now_ns + period_ns;
    return 0;
}

#ifdef __cplusplus
}
#endif
