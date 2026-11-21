# Cheats and mods ported from Monado II

These features come from [Monado II](https://github.com/brendonbusker/Monado-II) (MIT, commit `69e593a`). It forked
from the same Monado code base and targets the same Xenoblade Chronicles: Definitive Edition 2.0.0 executable, so its
hook sites apply unchanged. Every hook in `games/xcde/patches.py` still names the original instruction word it
expects, and code generation stops if one differs. The Monado II task tags (LINUX-033, -035/036, -039/040/044, -072,
-129) are kept in the comments so later upstream changes can be matched.

Native save formats are unchanged throughout.

## Cheats (F1 → Cheats / Trucos)

Each one starts off. Changes apply at once during play; **Apply** writes them to `xcde.ini` for the next start.

| Option | `xcde.ini` keys | Effect |
|---|---|---|
| Party damage dealt | `cheat_damage_dealt`, `cheat_damage_dealt_percent` | Scales positive hit damage from party members (AI included) to enemies. |
| Party damage received | `cheat_damage_received`, `cheat_damage_received_percent` | Scales positive hit damage from enemies to party members. |
| Enemy health | `cheat_enemy_health`, `cheat_enemy_health_percent` | Scales enemies' maximum HP (10–2000 %). Live changes keep the remaining HP fraction. Small health bars keep a safety minimum of 100 HP for scripted phases. |
| EXP / AP / SP gained | `cheat_exp`, `cheat_ap`, `cheat_sp` (+ `_percent`) | Scales new awards. Native eligibility, level-ups and caps still apply. Expert Mode's EXP transfer is not scaled again. |
| Gold gained | `cheat_gold`, `cheat_gold_percent` | Scales new positive gold gains. Spending is unchanged. |
| No fall damage | `cheat_no_fall_damage` | Ordinary landing damage for party characters is skipped. Forced or scripted deaths still happen. |
| High jump | `cheat_high_jump` | Jump height x8, the community "jump x8" code adapted to the 2.0.0 main. The game still computes the impulse, resolves collisions and lands the character. |

Multipliers are percentages: 100 = 1.00x, from 0 to 2000 (enemy health from 10). Anything malformed reads as 1x.
EXP, AP, SP and gold already earned stay in the save after a cheat is turned off. Damage over time and separate
status-effect paths are not covered.

The high jump is the XCDE community code for 1.1.2 (`040B0000 004E2F08 1E241000`, jump height x8). The 2.0.0 main
moved that jump-state instruction to `0x7000458fb8`; the recomp hooks the instruction instead of hardcoding 8.0, so
while the option is off the jump keeps its native 1.0 scale.

`XCDE_CHEAT_TRACE=1` logs each scaled hit, landing, jump-height scale, award, gold gain and enemy maximum HP as a
`[cheats]` line, with native and scaled values. With the cheats off it still logs native hits and landings at 1x.

Monado II's movement-speed cheat was not ported: this recomp already has its own run speed (T270, F1 → Cheats), with
the first (position-based) implementation kept as an opt-in *Old variant (more fun, experimental)* for comparison; it can break movement or collisions in some areas.

## Exit to desktop

* **Title menu:** an *Exit to desktop* row below Options, in the title controller's spare row.
* **System menu (+):** the menu's unused seventh row, below Title Screen. It is enabled whenever Title Screen is.

Both use the game's own confirmation dialog (default No). A confirmed exit ends the process like the F1 menu's
*Exit the game* (`runtime/hle/host_exit.h`). Monado II waits for its LockExit-aware applet close instead, which this
recomp does not have. The save journal (`runtime/services/fs.c`) still restores the last committed save if a write
were ever cut short.

## 99 manual save slots

On by default. The Save and Load lists hold 99 manual slots plus the autosave. The game keeps its four-card view. Only
the logical record list and the separate CPU thumbnail arena are larger. Title *Continue* and the title's save-presence
check scan the extra files too. Files keep the game's own names and format.

Turn it off in the launcher (Cheats → Save files) or with `save_slots_99 = 0` in `xcde.ini`, before starting the game.
The game's own list returns: the autosave plus three manual slots (files `00`–`02`), and *Continue* and the save-presence
check only look at those. Saves in slots 4–99 stay in `save/` but are not listed until the option is turned back on;
while it is off, new saves use the native slots. The larger buffers stay reserved, but every save-list, sort and
title-scan path uses the native records.

## Dedicated Quick Step gem slot (launcher → Controls)

Off by default (`quickstep_slot = 1` turns it on; turn it on before starting the game). In **Change Equipment**,
highlight the boots gem area and press Right: a small extra slot appears. Confirm opens the gem list, filtered to
Quick Step gems you own. Each character can hold one owned Quick Step gem there, separately for the main story
and Future Connected. The gem must not also be in ordinary equipment. *See Gem Effects* includes it. Use Submenu
→ Remove gem to clear the slot.

Assignments are stored per save file in a hidden `.monado-quickstep` folder inside the save mount
(`save/save_0/`). Back up the whole save folder, hidden files included, to keep them. Saves without that metadata
load normally with an empty slot.
