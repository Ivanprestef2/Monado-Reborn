// [XCDE] Unit tests for custom FPS parsing/clamping and the RTSS / MangoHud "early" style limiter state machine.
#include <assert.h>
#include <stdio.h>
#include "../../runtime/fps_limiter.h"

int main(void) {
    // Clamping bounds (15..1000, 0 = unlimited)
    assert(xcde_fps_clamp(0) == 0);
    assert(xcde_fps_clamp(-5) == 0);
    assert(xcde_fps_clamp(1) == XCDE_FPS_MIN);
    assert(xcde_fps_clamp(14) == XCDE_FPS_MIN);
    assert(xcde_fps_clamp(15) == 15);
    assert(xcde_fps_clamp(45) == 45);
    assert(xcde_fps_clamp(72) == 72);
    assert(xcde_fps_clamp(240) == 240);
    assert(xcde_fps_clamp(1000) == 1000);
    assert(xcde_fps_clamp(1500) == XCDE_FPS_MAX);

    // INI parsing
    assert(xcde_fps_parse("30") == 30);
    assert(xcde_fps_parse("45") == 45);
    assert(xcde_fps_parse("72") == 72);
    assert(xcde_fps_parse("144") == 144);
    assert(xcde_fps_parse("240") == 240);
    assert(xcde_fps_parse("10") == XCDE_FPS_MIN);
    assert(xcde_fps_parse("5000") == XCDE_FPS_MAX);
    assert(xcde_fps_parse("unlimited") == 0);
    assert(xcde_fps_parse("0") == 0);
    assert(xcde_fps_parse("ilimitado") == 0);
    assert(xcde_fps_parse("60abc") == 30);
    assert(xcde_fps_parse("") == 30);
    assert(xcde_fps_parse("-10") == 30);

    // Limiter mode parsing
    assert(xcde_fps_limiter_parse("default") == FPS_LIMITER_DEFAULT);
    assert(xcde_fps_limiter_parse("guest") == FPS_LIMITER_DEFAULT);
    assert(xcde_fps_limiter_parse("early") == FPS_LIMITER_EARLY);
    assert(xcde_fps_limiter_parse("rtss") == FPS_LIMITER_EARLY);
    assert(xcde_fps_limiter_parse("1") == FPS_LIMITER_EARLY);
    assert(xcde_fps_limiter_parse("bogus") == FPS_LIMITER_DEFAULT);
    assert(xcde_fps_limiter_valid(FPS_LIMITER_EARLY) == FPS_LIMITER_EARLY);
    assert(xcde_fps_limiter_valid(99) == FPS_LIMITER_DEFAULT);

    // Early limiter fixed-cadence state machine
    XcdeEarlyLimiter lim = {0};
    uint64_t t = 1000000000ull;  // 1.0 s
    uint64_t p60 = xcde_fps_period_ns(60);
    assert(p60 == 16666667ull);

    // Frame 0: anchor only, no wait
    assert(xcde_early_limiter_step(&lim, t, 60) == 0);
    assert(lim.next_deadline_ns == t + p60);

    // Frame 1: arrives 5 ms later -> waits until exact t + p60, next deadline advances to t + 2*p60
    uint64_t w1 = xcde_early_limiter_step(&lim, t + 5000000ull, 60);
    assert(w1 == t + p60);
    assert(lim.next_deadline_ns == t + 2 * p60);

    // Frame 2: woke 25 us late, work took 4 ms -> still targets exact t + 2*p60 (no cumulative drift)
    uint64_t w2 = xcde_early_limiter_step(&lim, w1 + 25000ull + 4000000ull, 60);
    assert(w2 == t + 2 * p60);
    assert(lim.next_deadline_ns == t + 3 * p60);

    // Frame 3: hitch (arrives 10 ms past deadline) -> no wait, re-anchors to now + p60 (no burst catch-up)
    uint64_t hitch_now = t + 3 * p60 + 10000000ull;
    assert(xcde_early_limiter_step(&lim, hitch_now, 60) == 0);
    assert(lim.next_deadline_ns == hitch_now + p60);

    // Live FPS change to custom 72 FPS -> re-anchors cleanly
    uint64_t p72 = xcde_fps_period_ns(72);
    uint64_t t_switch = hitch_now + 5000000ull;
    assert(xcde_early_limiter_step(&lim, t_switch, 72) == 0);
    assert(lim.next_deadline_ns == t_switch + p72);
    assert(xcde_early_limiter_step(&lim, t_switch + 2000000ull, 72) == t_switch + p72);

    // Unlimited (0) -> resets state and never waits
    assert(xcde_early_limiter_step(&lim, t_switch + 20000000ull, 0) == 0);
    assert(lim.next_deadline_ns == 0 && lim.last_fps == 0);

    // Hybrid present-aligned pacer: flat present-to-present cadence despite moving work-time variance
    XcdePresentPacer pp = {0};
    uint64_t pt = 2000000000ull;  // 2.0 s
    // Frame 0: initial anchor (no start wait), work = 5 ms -> waits until pt + p60
    assert(xcde_present_pacer_frame_start(&pp, pt, 60) == 0);
    uint64_t p0 = xcde_present_pacer_before_present(&pp, pt + 5000000ull, 60);
    assert(p0 == pt + p60);

    // Frame 1 (moving: work jumps 5 ms -> 9 ms): starts late, presents at exact p0 + p60
    uint64_t s1 = xcde_present_pacer_frame_start(&pp, p0 + 30000ull, 60);
    assert(s1 > p0 && s1 < p0 + p60);  // starts late for low input latency
    uint64_t p1 = xcde_present_pacer_before_present(&pp, s1 + 9000000ull, 60);
    // If s1 + 9 ms < p0 + p60, p1 hits exact p0 + p60; otherwise re-anchors cleanly
    uint64_t prev_p = p1 ? p1 : (s1 + 9000000ull);
    // Subsequent frames with 5..9 ms fluctuating work must hit exact p60 intervals (0 jitter)
    uint64_t works[4] = {6000000ull, 9000000ull, 5000000ull, 8500000ull};
    for (int i = 0; i < 4; i++) {
        uint64_t s = xcde_present_pacer_frame_start(&pp, prev_p + 30000ull, 60);
        uint64_t start_actual = s ? s : (prev_p + 30000ull);
        uint64_t pres = xcde_present_pacer_before_present(&pp, start_actual + works[i], 60);
        assert(pres == prev_p + p60);
        prev_p = pres;
    }

    // GPU-bound frame (work = 20 ms > p60): 0 wait and clean re-anchor (no burst catch-up)
    uint64_t sb = xcde_present_pacer_frame_start(&pp, prev_p + 30000ull, 60);
    uint64_t sb_act = sb ? sb : (prev_p + 30000ull);
    uint64_t slow_now = sb_act + 20000000ull;
    assert(xcde_present_pacer_before_present(&pp, slow_now, 60) == 0);
    assert(pp.next_present_ns == slow_now + p60);

    // Async overlap regression (Issue #261): Frame N+1 calls frame_start() slightly past Frame N's
    // deadline BEFORE vk-render calls before_present() for Frame N. Frame N must NOT sleep an extra
    // full period, and Frame N's work time must still be measured against Frame N's own start time.
    XcdePresentPacer ap = {0};
    uint64_t at = 3000000000ull;
    assert(xcde_present_pacer_frame_start(&ap, at, 60) == 0);
    // Frame N+1 starts 0.5 ms after Frame N's target present (at + p60) while Frame N is still in flight
    uint64_t n1_start = xcde_present_pacer_frame_start(&ap, at + p60 + 500000ull, 60);
    assert(n1_start > at + p60 && n1_start < at + 2 * p60);
    // Frame N finishes 1.0 ms after its target present -> must present immediately (0 wait), not sleep ~16 ms!
    assert(xcde_present_pacer_before_present(&ap, at + p60 + 1000000ull, 60) == 0);
    // Frame N's measured work time was p60 + 1 ms, NOT 0.5 ms
    assert(ap.work_count == 1 && ap.work_hist[0] == p60 + 1000000ull);

    // Unlimited (0) resets pacer
    assert(xcde_present_pacer_frame_start(&pp, slow_now + 1000000ull, 0) == 0);
    assert(pp.next_present_ns == 0 && pp.work_count == 0);

    puts("fps limiter: custom FPS bounds/parsing, early limiter, and present-aligned pacer OK");
    return 0;
}
