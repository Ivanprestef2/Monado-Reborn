# Evidence: Reflex gameplay session (2026-11-18, 60 FPS)

- **When:** 2026-11-18, 22:45–22:50 (Europe/Moscow); build after `b1bfa53`.
- **Configuration:** `fps = 60`, `reflex = on`, `upscaler = fsr3`, 1920x1080 window, **V-Sync off**
  (`present mode mailbox`; `xcde.ini` with `vsync = 0`).
- **Game:** save loaded from `save/save_0` (real gameplay, ~133k draws/frame and 458 MB of textures), no
  automated route.

## Results

| Metric | Value |
|---|---|
| Frames | 2,709 in 71.9 s |
| Average FPS | 32.1 (60 cap; GPU-limited) |
| `slReflexSleep` | 2,641 OK / 0 failures |
| PCL markers | 16,175 (6/frame + pings) |
| PCL pings | 335 |
| App PCL proxy | n=321, avg **31.25 ms**, min 7.33, max 248.35, 1 outlier >250 ms |
| Crash / stall | none; no exception or `[stall] no frame`; last project crash 20:14 |

The PCL proxy is the application's ping→present span, **not** the ETW PCL shown by RTSS/FrameView; no
latency-gain claim. The 32 FPS average does not reach the 60 cap because of the GTX 1050 Ti.

Full unpublished log: `build/reflex_probe/last_game_user_2250.log`. Versioned extract:
`docs/evidence/reflex_gameplay_2026-11-18_run.txt`.
