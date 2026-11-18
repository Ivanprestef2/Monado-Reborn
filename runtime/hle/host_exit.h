// [XCDE] Exit to desktop from the game's own menus (the Monado II title / System menu Exit rows): this recomp has no
// LockExit-aware close coordinator (Monado II's services/am LINUX-008), so a confirmed Exit ends the process the way
// the F1 menu's "Exit the game" does. Both menus are only reachable while no save is being written.
#pragma once
#include <stdint.h>
#include <stdio.h>
void plat_terminate(void);
static inline void am_request_host_exit(uint64_t now_ns) {
    (void)now_ns;
    fflush(stderr);
    plat_terminate();
}
