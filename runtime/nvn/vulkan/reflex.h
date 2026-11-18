// [XCDE] T272: NVIDIA Reflex (Streamline sl.reflex + PCL, Windows/NVIDIA only) for the game's frame loop.
// The calls live in runtime/nvn/vulkan/dlss.cpp (same Streamline runtime as DLSS). Inert (all functions no-op) when
// the feature is disabled, unsupported or not loaded; xcde.ini `reflex = off | on | boost` (default off, opt-in) or
// XCDE_REFLEX=0/1/2.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// xcde.ini reflex values (runtime/settings.h) and the PCL markers, in Streamline's own order
enum { REFLEX_OFF = 0, REFLEX_ON = 1, REFLEX_BOOST = 2 };
enum {
    REFLEX_SIM_START = 0, REFLEX_SIM_END = 1, REFLEX_SUBMIT_START = 2,
    REFLEX_SUBMIT_END = 3, REFLEX_PRESENT_START = 4, REFLEX_PRESENT_END = 5,
};

bool reflex_available(void);   // feature supported by this GPU/driver and the swapchain is hooked
bool reflex_loaded(void);      // the Reflex plugin was loaded this run (false: enabling it needs a restart)
const char *reflex_status(void);  // why it is (not) available, for the F1 menu / log
void reflex_set_mode(int mode);   // live option (REFLEX_OFF/ON/BOOST); the runtime must have been loaded at startup
int reflex_mode(void);         // mode in effect, REFLEX_OFF when unavailable

// Called by the frame limiter (runtime/hle/framepace.c) once per game frame while it waits at the frame boundary:
// obtains this frame's Streamline token and calls slReflexSleep. eSimulationStart is marked when the limiter returns
// (xcde_limiter_done), where the next simulation really begins. Returns the frame number (0: inactive).
uint64_t reflex_frame_begin(void);
uint64_t reflex_current_frame(void);  // frame begun above (for the present call), 0 when inactive
void reflex_marker(int marker);       // marker on the current frame (guest thread: SimulationEnd / RenderSubmitStart)

// Present markers: the render worker (exec.c) knows the game frame of the present it runs; device.c vk_frame_end
// emits the markers around vkQueuePresentKHR with that frame.
void reflex_set_present_frame(uint64_t frame);
void reflex_clear_present_frame(void);
void reflex_present_marker(int marker);
uint64_t reflex_present_frame_value(void);  // the present being executed (dlss.cpp's token lookup), 0 when none

#ifdef __cplusplus
}
#endif
