// [XCDE] Optional gameplay cheats, ported from Monado II (github.com/brendonbusker/Monado-II, MIT; LINUX-072/120/129).
// Coherent live settings shared by the F1 menu and the guest hooks (runtime/hle/cheats_*.c, games/xcde/patches.py).
// Movement speed is not here: this recomp already has its own leader-only run speed (T270, runtime/hle/party.c).
#pragma once
#include "settings.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {
    XCDE_CHEAT_DAMAGE_DEALT, XCDE_CHEAT_DAMAGE_RECEIVED,
    XCDE_CHEAT_EXP, XCDE_CHEAT_AP, XCDE_CHEAT_SP, XCDE_CHEAT_GOLD,
    XCDE_CHEAT_ENEMY_HEALTH, XCDE_CHEAT_COUNT
};
typedef struct {
    bool enabled[XCDE_CHEAT_COUNT];
    int percent[XCDE_CHEAT_COUNT];
    bool no_fall_damage;
} XcdeCheats;

// The cheat multipliers as (name, minimum %) for the settings loader / saver / F1 menu.
#define XCDE_CHEAT_MULTIPLIERS(X) \
    X(damage_dealt, 0) X(damage_received, 0) X(exp, 0) X(ap, 0) X(sp, 0) X(gold, 0) X(enemy_health, 10)

// Copies a complete, validated request. No lock is retained by the caller.
XcdeCheats xcde_cheats_snapshot(void);
void xcde_cheats_publish(const Settings *settings);
// Cheap coherent effective value, without a per-enemy mutex.
int xcde_cheats_enemy_health_percent(void);
static inline int xcde_cheats_minimum_percent(int kind) {
    return kind == XCDE_CHEAT_ENEMY_HEALTH ? 10 : 0;
}
static inline int xcde_cheats_effective_percent(const XcdeCheats *options, int kind) {
    if (!options || kind < 0 || kind >= XCDE_CHEAT_COUNT || !options->enabled[kind]) return 100;
    int percent = options->percent[kind];
    return percent >= xcde_cheats_minimum_percent(kind) && percent <= 2000 ? percent : 100;
}
#ifdef __cplusplus
}
#endif
