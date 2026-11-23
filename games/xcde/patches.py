"""XCDE instruction patches (contract: docs/contracts/patch-format.md)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "lifter"))
from patchfmt import JumpTable, Patch  # noqa: E402,F401


def _hook_then_original(fn):
    """Call a runtime hook, then the original instruction (which must lift to plain C)."""
    def emit(gen, addr):
        ins = gen.lift(addr)
        if ins.kind != 'op':
            raise ValueError(f'{addr:#x}: hook patch needs a plain instruction, got {ins.kind}')
        return f'{{ extern void {fn}(void); {fn}(); }} {ins.code}'
    return emit


# T157: the game's frame limiter (nnMain+0x12150, f_70005bcfe0) busy-waits for the end of the 33.3 ms frame: a loop
# that reads the system tick through a vtable until enough time passed, no sleep (a dedicated core on the Switch). On
# PC that spin was ~0.3 of a core. Hooks: the loop head (sleep while the frame end is still far, runtime/hle/framepace.c)
# and the function's exit (a frame boundary: learns the frame interval).
PATCHES = {
    0x70005bd068: Patch(0x320003e0, _hook_then_original('xcde_limiter_spin'), 'T157', 'mov w0, #1 - limiter loop head'),
    0x70005bd168: Patch(0x6d4123e9, _hook_then_original('xcde_limiter_done'), 'T157', 'ldp d9, d8, [sp, #0x10] - limiter exit'),
}


# T210 (party mod session): frame-count thresholds at the game's own 30 fps above 30 fps (runtime/hle/party.c
# xcde_frames_gt: "more than n frames at 30 fps" -> the same time at the current frame rate). A field follower restarts
# its "follow while running" move once its movement component (+0x200) counted more than 9 frames of a condition; at
# 60 fps that happened every ~11 frames and the party fell behind walking (2 -> 17.5 m in 200 frames; 30 fps: a steady
# 6.5 m). Both checks of that counter (f_7000443760, f_70004439f0) are `cmp w8, #9`.
def _frames_gt(n):
    def emit(gen, addr):
        return ('{ extern uint32_t xcde_frames_gt(uint32_t n); '
                f'(void)(subs32(c, (uint32_t)c->x[8], xcde_frames_gt({n}))); }}')
    return emit


PATCHES[0x700044394c] = Patch(0x7100251f, _frames_gt(9), 'T210', 'cmp w8, #9 - follower move counter (f_7000443760)')
PATCHES[0x7000443b9c] = Patch(0x7100251f, _frames_gt(9), 'T210', 'cmp w8, #9 - follower move counter (f_70004439f0)')


# T222 (party mod session): save / load list thumbnails. f_700057da00 copies a slot's loaded .tmb picture (320x180 RGB)
# into the buffer it maps (bl memcpy at 0x700057dab4: x0 destination, x1 source, x2 size) - only one of the slot's two
# alternating picture buffers. The hook records which buffer holds the game's own picture (runtime/nvn/vulkan/exec.c
# slot_thumb_fix copies it into the twin): the twin is not always empty - in-game its memory held leftovers, shown
# every other frame as pink / green garbage.
def _thumb_loaded(gen, addr):
    return ('c->x[30] = 0x700057dab8ull; { extern void xcde_thumb_loaded(uint64_t d, uint64_t n); '
            'uint64_t d_ = c->x[0], n_ = c->x[2]; f_70008b6b48(c); xcde_thumb_loaded(d_, n_); }')


PATCHES[0x700057dab4] = Patch(0x940ce425, _thumb_loaded, 'T222', 'bl memcpy - slot thumbnail picture loaded (f_700057da00)')

# T262: Colony 9 affinity chart. The byte-offset switch crosses lifter partitions.
PATCHES[0x7000183f48] = JumpTable(
    '7e1df8e08d60544bbdca1e333c153c97', 0xd61f0140, 0x9d5324, 0x7000183f4c,
    (0x7000183f4c, 0x7000183f94, 0x7000183fb8, 0x7000183fdc, 0x7000183f70), 'T262',
)

# T265: the main perspective camera stores width / height before calculating both projection and culling.
def _scene_aspect(gen, addr):
    return ('{ extern uint32_t xcde_scene_aspect(uint32_t, uint32_t); '
            'ST32(c->x[19] + 0x4c0, xcde_scene_aspect((uint32_t)c->v[4], (uint32_t)c->x[20])); }')


PATCHES[0x70006f7ea0] = Patch(0xbd04c264, _scene_aspect, 'T265', 'store scene projection aspect')


# [XCDE] GitHub #182: targeting reticle screen-bounds clamp. The battle HUD projects the targeted enemy to
# 1280x720 UI coordinates ([sp+0x30], [sp+0x34]) and clamps to [0, screen_w] x [0, screen_h] (or snaps to
# 0 / screen_w when behind the camera). On ultrawide / taller aspect ratios, widen those bounds to match
# the active scene aspect ratio so the reticle stays on the enemy outside the central 16:9 area.
def _reticle_clamp(axis, sp_off, dst_v):
    def emit(gen, addr):
        return (f'{{ extern float xcde_reticle_clamp_{axis}(float, float); '
                f'uint32_t raw_bits_ = LD32(c->sp + {sp_off:#x}ull), lim_bits_ = VS32(8), out_bits_; '
                f'float raw_, lim_, out_; '
                f'memcpy(&raw_, &raw_bits_, 4); memcpy(&lim_, &lim_bits_, 4); '
                f'out_ = xcde_reticle_clamp_{axis}(raw_, lim_); '
                f'memcpy(&out_bits_, &out_, 4); c->v[{dst_v}] = out_bits_; '
                f'ST32(c->sp + {sp_off:#x}ull, out_bits_); }}')
    return emit


def _reticle_edge_x(gen, addr):
    return ('{ extern float xcde_reticle_edge_x(float); '
            'uint32_t edge_bits_ = VS32(0), out_bits_; float edge_, out_; '
            'memcpy(&edge_, &edge_bits_, 4); out_ = xcde_reticle_edge_x(edge_); '
            'memcpy(&out_bits_, &out_, 4); c->v[0] = out_bits_; '
            'ST32(c->sp + 0x30ull, out_bits_); }')


PATCHES[0x70003bf090] = Patch(0xbd0033e0, _reticle_clamp('x', 0x30, 0), 'T274',
                              'str s0, [sp, #0x30] - reticle X clamp (in front of camera)')
PATCHES[0x70003bf0d0] = Patch(0xbd0037e9, _reticle_clamp('y', 0x34, 9), 'T274',
                              'str s9, [sp, #0x34] - reticle Y clamp (in front of camera)')
PATCHES[0x70003bf398] = Patch(0xbd0033e0, _reticle_edge_x, 'T274',
                              'str s0, [sp, #0x30] - reticle X edge snap (behind camera)')
PATCHES[0x70003bf3dc] = Patch(0xbd0037e9, _reticle_clamp('y', 0x34, 9), 'T274',
                              'str s9, [sp, #0x34] - reticle Y clamp (behind camera)')


# [XCDE] GitHub #182: UI pane 2D frustum cull in main_174.c. Every UI pane subclass culls its normalized
# 16:9 bounding box ([+0x184]=max_x, [+0x188]=max_y, [+0x18c]=min_x, [+0x190]=min_y) against [0, 1] x [0, 1],
# hiding world-space UI (targeting reticle, enemy icons) once completely outside the central 16:9 region.
# Shift the loaded bound by +/- 0.5*(wide_factor - 1) so the existing 0.0 / 1.0 comparisons match the widened view.
def _ui_cull_adjust(axis, is_min, vreg):
    def emit(gen, addr):
        ins = gen.lift(addr)
        if ins.kind != 'op':
            raise ValueError(f'{addr:#x}: ui cull patch needs a plain instruction, got {ins.kind}')
        return (ins.code + f' {{ extern float xcde_ui_cull_adjust_{axis}(float, int); '
                f'uint32_t b_ = VS32({vreg}); float v_; memcpy(&v_, &b_, 4); '
                f'v_ = xcde_ui_cull_adjust_{axis}(v_, {is_min}); '
                f'memcpy(&b_, &v_, 4); c->v[{vreg}] = b_; }}')
    return emit


# 19 sites with: ldr s0,[xN,#0x184]; ... ldr s0,[xN,#0x188]; ... ldr s1,[xN,#0x18c]; ... ldr s1,[xN,#0x190]
for _base, _rn in (
    (0x7000756594, 0), (0x700075662c, 1), (0x7000757fec, 0), (0x70007580e4, 1),
    (0x7000758544, 0), (0x70007585ec, 1), (0x700075a7ac, 0), (0x700075a9ac, 1),
    (0x700075ad9c, 0), (0x700075ae3c, 1), (0x700075b7dc, 0), (0x700075b87c, 1),
    (0x700075bf04, 0), (0x700075bfc4, 1), (0x700075cc3c, 0), (0x700075cd0c, 1),
    (0x700075f70c, 0), (0x700075fadc, 1), (0x7000760f7c, 0),
):
    PATCHES[_base + 0x00] = Patch(0xbd418400 | (_rn << 5), _ui_cull_adjust('x', 0, 0), 'T274', 'UI cull max_x')
    PATCHES[_base + 0x0c] = Patch(0xbd418800 | (_rn << 5), _ui_cull_adjust('y', 0, 0), 'T274', 'UI cull max_y')
    PATCHES[_base + 0x18] = Patch(0xbd418c01 | (_rn << 5), _ui_cull_adjust('x', 1, 1), 'T274', 'UI cull min_x')
    PATCHES[_base + 0x28] = Patch(0xbd419001 | (_rn << 5), _ui_cull_adjust('y', 1, 1), 'T274', 'UI cull min_y')

# 3 sites with s8=1.0f: ldr s0,[xA,#0x184]; ... ldr s0,[xB,#0x188]; ... ldr s0,[xA,#0x18c]; ... ldr s0,[xB,#0x190]
for _base, _ra, _rb in (
    (0x7000761040, 0, 0), (0x70007610c8, 26, 24), (0x70007611e8, 0, 0),
):
    PATCHES[_base + 0x00] = Patch(0xbd418400 | (_ra << 5), _ui_cull_adjust('x', 0, 0), 'T274', 'UI cull max_x')
    PATCHES[_base + 0x0c] = Patch(0xbd418800 | (_rb << 5), _ui_cull_adjust('y', 0, 0), 'T274', 'UI cull max_y')
    PATCHES[_base + 0x18] = Patch(0xbd418c00 | (_ra << 5), _ui_cull_adjust('x', 1, 0), 'T274', 'UI cull min_x')
    PATCHES[_base + 0x24] = Patch(0xbd419000 | (_rb << 5), _ui_cull_adjust('y', 1, 0), 'T274', 'UI cull min_y')


# [XCDE] Grass draw distance presets (runtime/hle/foliage_distance.c, foliage_pool.c).
# T-FOLIAGE-DISTANCE: apply the preset's separate MPF visibility and shading
# ranges. Original retains High shading at 1x visibility; Low halves visibility.
# Only bounded authored local ranges change; the broad
# 7000/8000 fallback, terrain/tree/cliff LODs and instance placement stay intact.
def _foliage_fade(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'op':
        raise ValueError('foliage fade patch needs a plain instruction')
    return ('{ extern float xcde_foliage_shading_distance(float, float); '
            'float near_, far_; uint32_t bits_; '
            'memcpy(&near_, &c->v[0], 4); memcpy(&far_, &c->v[1], 4); '
            'far_ = xcde_foliage_shading_distance(near_, far_); '
            'memcpy(&bits_, &far_, 4); c->v[1] = bits_; } ' + ins.code)


def _foliage_radius(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'op':
        raise ValueError('foliage radius patch needs a plain instruction')
    return (ins.code + ' { extern float xcde_foliage_distance(float, float); '
            'float radius_; uint32_t bits_; memcpy(&radius_, &c->v[8], 4); '
            'radius_ = xcde_foliage_distance(0.f, radius_); '
            'memcpy(&bits_, &radius_, 4); c->v[8] = bits_; }')


PATCHES[0x7000655ab0] = Patch(0xbd0023e0, _foliage_fade, 'T-FOLIAGE-DISTANCE',
                          'str s0, [sp, #0x20] - extend MPF material far fade')
for _site in (0x7000655e80, 0x7000655f8c):
    PATCHES[_site] = Patch(0xbd400908, _foliage_radius, 'T-FOLIAGE-DISTANCE',
                          'ldr s8, [x8, #8] - extend MPF CPU patch-selection radius')

# The type-3 default selector discards that input and loads a fixed 50-unit
# radius for its 5-unit cells. Scale AFTER this replacement as well; otherwise
# increasing the upstream radius leaves this grass edge unchanged. The original
# load still executes and is scaled once, rather than multiplying its caller's
# already-expanded radius again.
PATCHES[0x7000659ca8] = Patch(0xbd4a71a8, _foliage_radius, 'T-FOLIAGE-DISTANCE',
                          'ldr s8, [x13, #0xa70] - extend fixed 50-unit foliage radius')


# Commit a live request once for the whole serial foliage pass. Reuse the
# game's all-groups rebuild and material-upload paths; never clear live pools.
PATCHES[0x700067ea80] = Patch(0xa9bc5ff8, _hook_then_original('xcde_foliage_begin'),
                          'T-FOLIAGE-LIVE', 'stp x24, x23, [sp, #-0x40]! - foliage pass boundary')


def _foliage_live_selection(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'op':
        raise ValueError('foliage refresh patch needs a plain instruction')
    return ('{ extern void xcde_foliage_refresh_selection(Cpu *c); '
            'xcde_foliage_refresh_selection(c); } ' + ins.code)


def _foliage_live_materials(gen, addr):
    return ('{ extern void f_70006d8fd0(Cpu *c); '
            'extern _Bool xcde_foliage_refreshing(void); '
            'c->x[30] = 0x700067eae8ull; f_70006d8fd0(c); '
            'if (xcde_foliage_refreshing()) c->x[0] |= 1; }')


PATCHES[0x7000655d30] = Patch(0xd10243ff, _foliage_live_selection, 'T-FOLIAGE-LIVE',
                          'sub sp, sp, #0x90 - refresh cached stationary foliage selection')
PATCHES[0x700067eae4] = Patch(0x9401693b, _foliage_live_materials, 'T-FOLIAGE-LIVE',
                          'bl f_70006d8fd0 - request normal material upload after live change')


# T-FOLIAGE-STREAMING: high presets exhausted the authored 3.5 MiB patch pool.
# Provision the complete contiguous native allocation for 3x coverage (9x the
# authored area) with 24x pool capacity and a 2x bitmap for dense zones. Keep the
# bitmap, free-list pool, metadata and scratch/table offsets coherent; no live
# pool is resized, reset or freed outside the guest's normal lifetime.
def _foliage_pool(reg, part, base=False):
    def emit(gen, addr):
        ins = gen.lift(addr)
        if ins.kind != 'op': raise ValueError('foliage layout requires a plain instruction')
        return (ins.code + ' { extern uint32_t xcde_foliage_pool_layout(int part); '
                f'c->x[{reg}] = ' + ('c->x[0] + ' if base else '') +
                f'xcde_foliage_pool_layout({part}); }}')
    return emit
for _site, _word, _reg, _part, _base in (
    (0x700068751c, 0xb81a03a8, 1, 0, False),
    (0x7000687524, 0x91400808, 8, 1, True),
    (0x700068752c, 0x914e8808, 8, 2, True),
    (0x7000687538, 0x72a00748, 8, 3, False),
    (0x7000687544, 0x914e0809, 9, 4, True),
    (0x7000687580, 0x320d0bea, 10, 5, False),
):
    PATCHES[_site] = Patch(_word, _foliage_pool(_reg, _part, _base), 'T-FOLIAGE-STREAMING',
                           'coherent MPF bitmap/pool allocation for all live grass presets')


# The game's fixed map heap cannot hold the extended coverage. Own a bounded
# private guest-address arena and release it at the original MPF destructor boundary.
def _foliage_allocate(gen, addr):
    return ('{ extern uint64_t xcde_foliage_pool_alloc(uint64_t owner); '
            'c->x[30]=0x7000687524ull; c->x[0]=xcde_foliage_pool_alloc(c->x[19]); }')
def _foliage_release(gen, addr):
    return ('{ extern void xcde_foliage_pool_free(uint64_t address); '
            'c->x[30]=0x7000687718ull; xcde_foliage_pool_free(c->x[0]); }')
PATCHES[0x7000687520] = Patch(0x97fdbb78, _foliage_allocate, 'T-FOLIAGE-STREAMING',
                           'allocate MPF scratch/pool outside the fixed map heap')
PATCHES[0x7000687714] = Patch(0x97fdbb2b, _foliage_release, 'T-FOLIAGE-STREAMING',
                           'release MPF private storage on native destruction')


# [XCDE] Follower footsteps above 30 FPS: keep the normal marker eligibility
# check, then reject repeated follower foot contacts across locomotion
# animation restarts (runtime/hle/party.c xcde_footstep_marker).
def _footstep_marker(gen, addr):
    return ('{ extern _Bool xcde_footstep_marker(Cpu *c, uint64_t action, uint64_t marker); '
            'if (!(c->x[0] & 1) || !xcde_footstep_marker(c, c->x[20], c->x[19])) '
            'goto L_70003f4c5c; }')


PATCHES[0x70003f4c0c] = Patch(0x36000280, _footstep_marker, 'T-FOOTSTEP-TIMING',
                              'tbz w0, #0 - follower foot marker replay guard')


# [XCDE] Menu hold/repeat timers above 30 FPS. f_7000578b50 advanced every pad
# hold/repeat timer by the fixed simulation step at UpdateInfo+4 (1/30 s), once
# per presented frame, so at 120 FPS a short tap repeated and held directions
# scrolled 4x too fast. UpdateInfo+8 already holds the elapsed seconds of the
# frame (+12 is elapsed authored frames). Use it only for these 27 timer loads;
# button edge detection, input polling and layout rendering stay intact.
def _menu_input_elapsed(gen, addr):
    return '{ uint64_t ea = c->x[20] + 8; c->v[1] = LD32(ea); }'


_MENU_HOLD_TIME_LOADS = (
    0x7000578ebc, 0x7000578eec, 0x7000578f18, 0x7000578f44,
    0x7000578f70, 0x7000578f9c, 0x7000578fc8, 0x7000578ff4,
    0x7000579020, 0x700057904c, 0x7000579078, 0x70005790a4,
    0x70005790d0, 0x70005790fc, 0x7000579128, 0x7000579154,
    0x7000579180, 0x70005791ac, 0x70005791d8, 0x7000579204,
    0x7000579230, 0x700057925c, 0x7000579288, 0x70005792b4,
    0x70005792e0, 0x7000579308, 0x70005793b4,
)
for _site in _MENU_HOLD_TIME_LOADS:
    PATCHES[_site] = Patch(0xbd400681, _menu_input_elapsed, 'T-MENU-INPUT',
                           'ldr s1, [x20, #4] -> elapsed seconds at +8 for menu hold timers')

# [XCDE] Graphics options (runtime/hle/graphics.c, graphics_options.h, prop_lod.h).
# T-GRAPHICS-SCALABILITY: f_70006b24e0 has just copied the complete 0x1220
# render configuration. Invalidate any previous per-view overlay backup.
# The final camera-setup boundary applies settings without changing the source.
def _graphics_copy(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'op': raise ValueError('graphics copy requires a plain epilogue instruction')
    return ('{ extern void xcde_graphics_copy(uint64_t destination); '
            'xcde_graphics_copy(c->x[19]); } ' + ins.code)
PATCHES[0x70006b24f8] = Patch(0xa9417bfd, _graphics_copy, 'T-GRAPHICS-SCALABILITY',
                          'ldp x29,x30,[sp,#0x10] - apply live render-config overrides after copy')

# Camera-mode setup can restore bloom/AO/blur from the authored singleton AFTER
# the copy. Apply the request at the final per-view setup boundary as well.
def _graphics_finish(gen, addr):
    return _graphics_copy(gen, addr).replace('xcde_graphics_copy', 'xcde_graphics_finish')
PATCHES[0x70006b2918] = Patch(0xa9437bfd, _graphics_finish, 'T-GRAPHICS-SCALABILITY',
                          'ldp x29,x30,[sp,#0x30] - apply overrides after camera-mode setup')


# T-SHADOW-MAPS: size both native pool-layout calculations and all three
# cascade defaults. The independent 256px target remains authored.
def _shadow_pool_dimension(reg):
    def emit(gen, addr):
        return ('{ extern uint32_t xcde_shadow_map_size(void); '
                f'c->x[{reg}] = xcde_shadow_map_size(); }}')
    return emit
for _site,_word,_reg in ((0x70006a2234,0x321603e2,2),(0x70006a2238,0x321603e3,3)):
    PATCHES[_site]=Patch(_word,_shadow_pool_dimension(_reg),'T-SHADOW-MAPS','native cascade pool dimensions')
def _shadow_default(reg):
    def emit(gen,addr):
        ins=gen.lift(addr)
        if ins.kind!='op':raise ValueError('shadow defaults require a plain load')
        return (ins.code+' { extern uint32_t xcde_shadow_map_size(void); '
                f'c->v[{reg}]=(c->v[{reg}] & ~((__uint128_t)0xffffffffu<<32)) | ((__uint128_t)xcde_shadow_map_size()<<32); }}')
    return emit
for _site,_word,_reg in ((0x7000696a60,0xfd440100,0),(0x7000696a80,0xfd440901,1),(0x7000696aa0,0xfd440d01,1)):
    PATCHES[_site]=Patch(_word,_shadow_default(_reg),'T-SHADOW-MAPS','native cascade resolution default; preserve near/far floats')

# T-PROP-LOD: decoded prop buffer and its size from the selected 64-byte map
# stream record are available before the native prop constructor consumes it.
def _prop_lod_prepare(gen,addr):
    ins=gen.lift(addr)
    if ins.kind!='op':raise ValueError('prop LOD hook requires a plain load')
    return ('{ extern void xcde_prop_lod(uint64_t data,uint32_t size); '
            'xcde_prop_lod(c->x[22],LD32(c->x[8]+0x38)); } '+ins.code)
PATCHES[0x700067b77c]=Patch(0xb9403908,_prop_lod_prepare,'T-PROP-LOD','ldr w8,[x8,#0x38] - guarded prop stream preparation')


# Undo the prior frame's overlay even when the working configuration is reused.
def _graphics_begin(gen, addr):
    ins=gen.lift(addr)
    if ins.kind!='op': raise ValueError('graphics begin requires a plain prologue')
    return ('{ extern void xcde_graphics_begin(uint64_t destination); '
            'xcde_graphics_begin(c->x[0]); } '+ins.code)
PATCHES[0x70006b2720] = Patch(0xa9bc5ff8, _graphics_begin, 'T-GRAPHICS-SCALABILITY',
                           'restore previous overrides before native per-view camera setup')


# Reused/freed native configuration objects must not retain a prior view backup.
def _graphics_construct(gen, addr):
    ins=gen.lift(addr)
    if ins.kind!='op': raise ValueError('graphics constructor requires a plain prologue')
    return ('{ extern void xcde_graphics_forget(uint64_t destination); '
            'xcde_graphics_forget(c->x[0]); } '+ins.code)
def _graphics_destroy(gen, addr):
    return '{ extern void xcde_graphics_forget(uint64_t destination); xcde_graphics_forget(c->x[0]); } return;'
PATCHES[0x70006b1b00] = Patch(0xf81e0ff3, _graphics_construct, 'T-GRAPHICS-SCALABILITY',
                           'forget prior overlays on native configuration construction')
PATCHES[0x70006b24d0] = Patch(0xd65f03c0, _graphics_destroy, 'T-GRAPHICS-SCALABILITY',
                           'forget prior overlays on native configuration destruction')


# T270 (GitHub #100): the game's max-horizontal-velocity rate. f_70004463b0 multiplies the rate it selected from the
# character state (s8: 1.0 normally, 0.5 or a movement value in other states) by the movement speed at +0x11c and
# stores the product. The XCDE 2.0.0 community cheat for this build (BID 7E1DF8E08D60544B, GBAtemp) replaces this
# multiply with a fixed 2.0 for "Speed mod x2"; scaling the product instead keeps the game's own state-dependent rate
# and lets the game move itself (collisions and ground handling keep working). The function also runs for followers and
# NPCs: the runtime scales only when the character controller (x20) is the field leader's controller or has the
# player-controller class (used after story switches), which it tracks each frame (runtime/hle/party.c). Jumping is not
# scaled (horizontal rate only).
def _run_speed(gen, addr):
    return ('{ extern uint32_t xcde_run_speed_mul(uint32_t, uint32_t, uint64_t); '
            'c->v[0] = xcde_run_speed_mul((uint32_t)c->v[8], (uint32_t)c->v[0], c->x[20]); }')


PATCHES[0x7000446520] = Patch(0x1e200900, _run_speed, 'T270',
                              'fmul s0, s8, s0 - scale max horizontal velocity rate (leader only)')


# ---------------------------------------------------------------------------------------------------------------------
# [XCDE] Optional cheats and mods ported from Monado II (github.com/brendonbusker/Monado-II, MIT). Same DE 2.0.0 main,
# so its checked hook sites and words apply unchanged; every Patch still verifies the original instruction word.
def _plain(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'op':
        raise ValueError(f'{addr:#x}: hook requires a plain instruction')
    return ins.code


def _native_call(gen, addr, target):
    ins = gen.lift(addr)
    if ins.kind != 'bl' or ins.target != target:
        raise ValueError(f'{addr:#x}: hook requires its checked native call to {target:#x}')
    return (f'extern void f_{target:x}(Cpu *c); '
            f'c->x[30] = 0x{addr + 4:x}ull; f_{target:x}(c); ')


def _prefix(helper, returns=False):
    """Call helper(c) before the original plain instruction; returns=True: a true result returns from the function."""
    def emit(gen, addr):
        original = _plain(gen, addr)
        if returns:
            return f'{{ extern _Bool {helper}(Cpu *c); if ({helper}(c)) return; }} ' + original
        return f'{{ extern void {helper}(Cpu *c); {helper}(c); }} ' + original
    return emit


def _after(helper):
    def emit(gen, addr):
        return _plain(gen, addr) + f' {{ extern void {helper}(Cpu *c); {helper}(c); }}'
    return emit


# LINUX-072 (runtime/hle/cheats_rewards.c): scale new EXP / AP / SP awards once at the award calls, retaining native
# eligibility, level-ups, skill bonuses and caps. Expert Mode's bonus-EXP transfer is deliberately not patched.
def _cheats_reward_award(gen, addr):
    original = _native_call(gen, addr, 0x70000ba860)
    return ('{ extern _Bool xcde_cheats_rewards_call(Cpu *c); '
            f'c->x[30] = 0x{addr + 4:x}ull; '
            'if (!xcde_cheats_rewards_call(c)) { ' + original + '} }')


CHEATS_REWARD_CALLS = {
    0x70000b5d90: 0x940012b4, 0x70000b5dc0: 0x940012a8, 0x70000b5df0: 0x9400129c, 0x70000b5e20: 0x94001290,
    0x70000b5e50: 0x94001284, 0x70000b5e80: 0x94001278, 0x70000b5eb0: 0x9400126c, 0x70000b5edc: 0x94001261,
    0x70000bb60c: 0x97fffc95, 0x70000bbe20: 0x97fffa90, 0x70000bc09c: 0x97fff9f1,
}
for _site, _word in CHEATS_REWARD_CALLS.items():
    assert _site not in PATCHES, f'Conflicting cheat patch at {_site:#x}'
    PATCHES[_site] = Patch(_word, _cheats_reward_award, 'LINUX-072',
                           'new EXP/AP/SP award; original call when off/1x, owned scaled record otherwise')
PATCHES[0x70000bc830] = Patch(0xf81e0ff3, _prefix('xcde_cheats_gold'), 'LINUX-072',
                              'positive gold-gain amount only; retain original add/cap behavior and prologue')

# LINUX-072 (runtime/hle/cheats_combat.c): scale positive native event-9 HP loss once, after the native source/target
# lookup. Normal and linked damage dispatch keep their own immunity / presentation.
for _site, (_word, _helper) in {0x700049eabc: (0xb0003788, 'cheats_combat_hit'),
                                0x700049d148: (0xf9400fe8, 'cheats_combat_linked_hit')}.items():
    assert _site not in PATCHES, f'Conflicting cheat patch at {_site:#x}'
    PATCHES[_site] = Patch(_word, _after(_helper), 'LINUX-072',
                           'optional party damage magnitude; native event dispatch and HP consumer remain')

# LINUX-072: no fall damage jumps to the function's own no-damage path. Its saved arguments and false-return epilogue
# are checked too, so a different native layout fails generation.
CHEATS_FALL_CONTEXT = {
    0x700044678c: 0x2a0103f6,  # original forced-event argument -> w22
    0x7000446858: 0xaa0003f7,  # native status-kind result -> w23
    0x700044691c: 0x2a1f03f6,  # existing no-damage/false path
    0x7000446924: 0x2a1603e0,  # return original native false result
    0x700044693c: 0xd65f03c0,  # native epilogue returns normally
}


def _cheats_fall(gen, addr):
    for site, expected in CHEATS_FALL_CONTEXT.items():
        if gen.word(site) != expected:
            raise ValueError(f'{site:#x}: LINUX-072 native fall context changed')
    return (_plain(gen, addr) + ' { extern _Bool cheats_skip_fall_damage(Cpu *c); '
            'if (cheats_skip_fall_damage(c)) goto L_700044691c; }')


PATCHES[0x7000446864] = Patch(0xb9400815, _cheats_fall, 'LINUX-072',
                              'optional ordinary PC fall immunity before nonlethal/lethal split; forced events remain native')


# T276: optional high jump. The XCDE community "jump x8" code for 1.1.2 (BID 92C78BB3DCBBC3F7: `040B0000 004E2F08
# 1E241000`) replaces the jump-state height scale 1.0 with 8.0. The DE 2.0.0 main used here keeps the same jump state:
# 0x7000458fac..0x7000458fc8 loads the jump power, stores it at +0x1c, passes the height scale in s0 to the checked
# sqrt(2*g*h*scale) impulse helper (f_7000429b00) and resets the jump state. Only the address moved (1.1.2 0x4E2F08 ->
# 2.0.0 0x7000458fb8); the hook keeps the native instruction when the option is off and returns 8.0 when it is on.
CHEATS_JUMP_CONTEXT = {
    0x7000458fac: 0xaa1303e0,  # native jump power query argument
    0x7000458fb0: 0x9400002c,  # bl 0x7000459060: jump power -> s0
    0x7000458fb4: 0xbd001e80,  # str s0, [x20, #0x1c]: character jump power
    0x7000458fbc: 0xaa1403e0,  # mov x0, x20: impulse helper argument
    0x7000458fc0: 0x97ff42d0,  # bl 0x7000429b00: native sqrt(2*g*h*scale) jump impulse
    0x7000458fc4: 0xaa1403e0,  # mov x0, x20: native jump-state reset argument
    0x7000458fc8: 0x97ff3f1a,  # bl 0x7000428c30: native jump-state reset
}


def _cheats_high_jump(gen, addr):
    for site, expected in CHEATS_JUMP_CONTEXT.items():
        if gen.word(site) != expected:
            raise ValueError(f'{site:#x}: T276 native jump context changed')
    return ('{ extern uint32_t xcde_cheats_jump_scale(void); '
            'c->v[0] = xcde_cheats_jump_scale(); }')


PATCHES[0x7000458fb8] = Patch(0x1e2e1000, _cheats_high_jump, 'T276',
                              'fmov s0, #1.0 - jump-height scale (optional x8); native impulse and landing kept')


# LINUX-033 (runtime/hle/title_exit.c): an "Exit to desktop" row in the title menu, in the title controller's spare
# preallocated row, through the game's own confirmation dialog; rendering, navigation and cleanup stay native.
def _title_rows(gen, addr):
    return ('{ extern _Bool xcde_title_exit_configure(uint64_t ui); '
            'if (c->x[24] == 0) (void)xcde_title_exit_configure(c->x[19]); } ' + _plain(gen, addr))


def _title_render_action(gen, addr):
    return (_plain(gen, addr) + ' { extern _Bool xcde_title_exit_row_is_exit(uint64_t ui, uint64_t row); '
            'if ((uint32_t)c->x[8] == 7 && xcde_title_exit_row_is_exit(c->x[19], c->x[25])) c->x[8] = 4; }')


def _title_label(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'bl' or ins.target != 0x700057c160:
        raise ValueError(f'{addr:#x}: title label requires the original native text setter')
    return ('{ extern _Bool xcde_title_exit_label(Cpu *c); '
            f'extern void f_700057c160(Cpu *c); c->x[30] = 0x{addr + 4:x}ull; '
            'if (!xcde_title_exit_label(c)) f_700057c160(c); }')


def _title_confirm_action(gen, addr):
    return ('{ extern uint32_t xcde_title_exit_action(uint64_t sequence, uint32_t action); '
            'c->x[0] = xcde_title_exit_action(c->x[19], (uint32_t)c->x[0]); } ' + _plain(gen, addr))


def _title_force_prompt(gen, addr):
    return (_plain(gen, addr) + ' { extern _Bool xcde_title_exit_pending(uint64_t sequence); '
            'if (xcde_title_exit_pending(c->x[19])) goto L_xcde_title_exit_prompt; }')


def _title_prompt_label(gen, addr):
    return 'L_xcde_title_exit_prompt:; ' + _plain(gen, addr)


def _title_dialog_text(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'bl' or ins.target != 0x70000f4520:
        raise ValueError(f'{addr:#x}: title confirmation requires the original native dialog call')
    return ('{ extern void xcde_title_exit_dialog(Cpu *c); '
            f'extern void f_70000f4520(Cpu *c); c->x[30] = 0x{addr + 4:x}ull; '
            'xcde_title_exit_dialog(c); f_70000f4520(c); }')


def _title_dispatch_guard(gen, addr):
    return ('{ extern void xcde_title_exit_check_dispatch(uint64_t sequence, uint32_t action); '
            'xcde_title_exit_check_dispatch(c->x[19], (uint32_t)c->x[0]); } ' + _plain(gen, addr))


TITLE_EXIT_PATCHES = {
    0x70002979f8: (0xb8787b56, _title_rows, 'first native row-label iteration; insert into verified spare capacity'),
    0x7000297a50: (0xb9400328, _title_render_action, 'native label-render action; preserve stored Exit action'),
    0x7000297c80: (0x940b9138, _title_label, 'native row text setter; authored Exit label'),
    0x70003c48e0: (0x7100181f, _title_confirm_action, 'confirmed selection; enter existing native confirmation route'),
    0x70003c4be4: (0xaa0003f3, _title_force_prompt, 'checked title owner; Exit always asks, including Future Connected'),
    0x70003c4c54: (0x12800014, _title_prompt_label, 'native confirmation construction after original bypass conditions'),
    0x70003c4ff8: (0x97f4bd4a, _title_dialog_text, 'native dialog call; authored prompt with original localized choices'),
    0x70003c5ef0: (0xf9400408, _prefix('xcde_title_exit_reply', True), 'native reply entry; Cancel bookkeeping then host exit on Yes'),
    0x70003c5248: (0x7100181f, _title_dispatch_guard, 'native action dispatch; Exit must not become a story-mode transition'),
}
for _site, (_word, _emit, _note) in TITLE_EXIT_PATCHES.items():
    assert _site not in PATCHES, f'Conflicting title exit patch at {_site:#x}'
    PATCHES[_site] = Patch(_word, _emit, 'LINUX-033', _note)


# LINUX-033 r2 (runtime/hle/system_exit.c): keep all six System menu rows and attach the resource's unused seventh
# native widget as Exit; the original controller still owns selection, permissions and cleanup.
def _system_construct(gen, addr):
    _native_call(gen, addr, 0x7000293c00)
    return ('{ extern void xcde_system_exit_construct(Cpu *c); '
            f'c->x[30] = 0x{addr + 4:x}ull; xcde_system_exit_construct(c); }}')


def _system_action(gen, addr):
    return ('{ ' + _native_call(gen, addr, 0x7000294680) +
            'extern uint32_t xcde_system_exit_action(uint64_t sequence, uint32_t action); '
            'c->x[0] = xcde_system_exit_action(c->x[19], (uint32_t)c->x[0]); }')


def _system_child(gen, addr):
    return ('{ ' + _native_call(gen, addr, 0x70002ef680) +
            'extern void xcde_system_exit_child(Cpu *c); xcde_system_exit_child(c); }')


def _system_dialog(gen, addr):
    return ('{ extern void xcde_system_exit_dialog(Cpu *c); '
            'xcde_system_exit_dialog(c); ' + _native_call(gen, addr, 0x7000331840) + '}')


SYSTEM_EXIT_PATCHES = {
    0x70003b570c: (0x97fb793d, _system_construct, 'native System constructor; separate extended guest storage with seventh widget'),
    0x70002941e0: (0xf81e0ff3, _prefix('xcde_system_exit_destroy'), 'native System destructor; release seventh widget before original six'),
    0x7000294690: (0xa9be4ff4, _prefix('xcde_system_exit_update'), 'native System frame update; update seventh widget, mirror Title Screen permission'),
    0x7000294070: (0xd10103ff, _prefix('xcde_system_exit_help', True), 'native help setter; authored Exit help only on the seventh row'),
    0x7000294680: (0xb9402808, _prefix('xcde_system_exit_selected', True), 'native selected row getter; accept verified seventh row'),
    0x70003b5a18: (0x97fb7b1a, _system_action, 'confirmed System selection; route Exit through native Title Screen confirmation'),
    0x70003b5a48: (0x97fb7b0e, _system_action, 'native System action validation; temporary Exit action alias'),
    0x70003b5aac: (0x97fb7af5, _system_action, 'native System dispatch; retain original Title Screen enabled check'),
    0x70003b5d0c: (0x97fce65d, _system_child, 'native Title Screen confirmation child; bind pending Exit owner'),
    0x70003b6138: (0x97fdedc2, _system_dialog, 'native System confirmation; authored prompt, default No and bound Exit caption'),
    0x70003b6350: (0xf9400408, _prefix('xcde_system_exit_reply', True), 'native System confirmation reply; Cancel bookkeeping before host exit'),
    0x70002f1e08: (0x910043e0, _prefix('xcde_exit_caption_render'), 'native MsgInfo UI post-build; caption only the bound Exit confirmation'),
}
for _site, (_word, _emit, _note) in SYSTEM_EXIT_PATCHES.items():
    assert _site not in PATCHES, f'Conflicting System exit patch at {_site:#x}'
    PATCHES[_site] = Patch(_word, _emit, 'LINUX-033', _note)


# LINUX-129 (runtime/hle/cheats_enemy_health.c): true enemy HP uses the fresh derived maximum and native
# actor lifetime/update paths. Preserve damage/protection/event machinery. Exact
# module, build, layout methods and surrounding instructions fail closed.
ENEMY_HEALTH_BUILD_ID = '7e1df8e08d60544bbdca1e333c153c97'
ENEMY_HEALTH_CONTEXT = {
    0x700047f780: 0x321e03e0,  # EnemyStatus kind4
    0x700047f784: 0xd65f03c0,
    0x700047f2b0: 0x52829388,  # stats at status+0x149c
    0x700047f2b4: 0x8b080000,
    0x700047f2b8: 0xd65f03c0,
    0x700047dbe4: 0x91004108,  # constructor installs checked EnemyStatus vtable
    0x700047dbe8: 0xf9000268,
    0x700047eba8: 0x97f03bea,  # native base stats finish before initial derived HP
    0x700047ebac: 0xf9482261,
    0x700047ebb0: 0xb950b262,
    0x700047ebb4: 0xaa1503e0,  # fresh stats argument
    0x700047ebb8: 0xaa1303e3,  # owning EnemyStatus argument
    0x700047ebbc: 0x97f03f3d,  # derived call; LR0x47ebc0 identifies initial completion
    0x700047ebc0: 0xf9400268,
    0x70004b9a44: 0xf942ad08,  # nondeleting status destructor rewrites vtable
    0x70004b9a48: 0xaa0003f3,
    0x70004b9a54: 0xf8010668,
    0x70004b9938: 0xf942ad29,  # deleting component destructor has status at+0x60
    0x70004b9948: 0x5282c708,
    0x70004b9950: 0xf9003009,
    0x700008e8b4: 0xa9056ffc,  # native recompute frame0xb0
    0x700008ed30: 0x7100051f,  # original minimum1/max calculation remains
    0x700008ed34: 0x1a9f8508,
    0x700047fb90: 0xf9401d08,  # native per-actor update before dirty gate
    0x700047fb94: 0xaa1303e0,
    0x700047d388: 0xb9400808,  # current HP read before ordinary delta ADD
    0x700047d38c: 0xaa1303e0,
    0x700047d418: 0xb9400808,  # alternate protected delta branch
    0x700047d41c: 0xaa1303e0,
    0x700047d4f4: 0xb9400808,  # direct delta branch
    0x700047d4f8: 0xaa1303e0,
    0x700041f50c: 0xb940e808,  # effect0xc3 floor reads effective maximum
    0x700041f510: 0x794087e9,
    0x700041f514: 0x1b097d08,  # keep native W8 product; widen conversion only
    0x700041f51c: 0x90002da8,
    0x700041f520: 0xbd49f901,  # original0.01 scalar
    0x700041f524: 0x1e210800,
}


def _enemy_health_hook(helper, mode='before'):
    def emit(gen, addr):
        if gen.name != 'main' or not gen.nso.build_id.startswith(ENEMY_HEALTH_BUILD_ID):
            raise ValueError('enemy health cheat requires checked DE 2.0.0 main')
        for site, expected in ENEMY_HEALTH_CONTEXT.items():
            if gen.word(site) != expected:
                raise ValueError(f'{site:#x}: LINUX-129 native enemy health context changed')
        original = _plain(gen, addr)
        if mode == 'replace_if_owned':
            return (f'{{ extern _Bool {helper}(Cpu *c); '
                    f'if (!{helper}(c)) {{ {original} }} }}')
        call = f'{{ extern void {helper}(Cpu *c); {helper}(c); }}'
        return original + ' ' + call if mode == 'after' else call + ' ' + original
    return emit


ENEMY_HEALTH_PATCHES = {
    0x700047dbb0: (0xa9be4ff4, _enemy_health_hook('xcde_cheats_enemy_health_construct'),
                   'EnemyStatus construction begins a new host lifetime'),
    0x700047eb70: (0xd101c3ff, _enemy_health_hook('xcde_cheats_enemy_health_initialize'),
                   'native enemy initialization resets HP ownership before base stats'),
    0x70004b9a30: (0xa9bd57f6, _enemy_health_hook('xcde_cheats_enemy_health_destroy'),
                   'nondeleting status destructor invalidates HP lifetime'),
    0x70004b9920: (0xa9be4ff4, _enemy_health_hook('xcde_cheats_enemy_health_destroy_component'),
                   'deleting component destructor invalidates embedded status lifetime'),
    0x700047fb8c: (0xf9400268, _enemy_health_hook('xcde_cheats_enemy_health_update'),
                   'current actor requests native dirty refresh only on an option change'),
    0x700008e8b0: (0xd102c3ff, _enemy_health_hook('xcde_cheats_enemy_health_recompute_begin'),
                   'capture HP fraction before native recomputation overwrites derived stats'),
    0x700008ed38: (0xb900ea68, _enemy_health_hook('xcde_cheats_enemy_health_max'),
                   'scale freshly calculated enemy maximum exactly once'),
    0x700047d390: (0x0b140114, _enemy_health_hook('xcde_cheats_enemy_health_add', 'replace_if_owned'),
                   'owned enemy delta uses widened sum before original clamp and side effects'),
    0x700047d420: (0x0b140114, _enemy_health_hook('xcde_cheats_enemy_health_add', 'replace_if_owned'),
                   'owned protected enemy delta retains original protection branch'),
    0x700047d4fc: (0x0b140114, _enemy_health_hook('xcde_cheats_enemy_health_add', 'replace_if_owned'),
                   'owned direct enemy delta avoids signed overflow before native clamp'),
    0x700041f518: (0x1e230100, _enemy_health_hook('xcde_cheats_enemy_health_floor_convert', 'replace_if_owned'),
                   'owned enemy protection-floor conversion uses the full product and native FP'),
    0x700041f528: (0x1e390016, _enemy_health_hook('xcde_cheats_enemy_health_floor_clamp', 'after'),
                   'owned enemy protection floor cannot exceed its signed-safe maximum'),
}
for _site, (_word, _emit, _note) in ENEMY_HEALTH_PATCHES.items():
    assert _site not in PATCHES, f'Conflicting enemy health patch at {_site:#x}'
    PATCHES[_site] = Patch(_word, _emit, 'LINUX-129', _note)


# LINUX-035 (runtime/hle/expanded_saves.c): 99 manual save slots. Keep the native four-card viewport and async job identities;
# expand only logical metadata and the separately appended CPU thumbnail arena.
# Every ABI anchor is checked against the selected DE 2.0.0 input by Patch.
def _save_constant(register, value):
    def emit(gen, addr):
        _plain(gen, addr)
        return f'c->x[{register}] = {value}u;'
    return emit


def _save_prefix(helper, argument):
    def emit(gen, addr):
        original = _plain(gen, addr)
        parameter = 'Cpu *c' if argument == 'c' else 'uint64_t state'
        return (f'{{ extern void {helper}({parameter}); {helper}({argument}); }} '
                + original)
    return emit


def _save_initialize(gen, addr):
    _plain(gen, addr)
    return ('{ extern void xcde_expanded_saves_initialize(Cpu *c); '
            'xcde_expanded_saves_initialize(c); }')


def _save_legacy(helper, target):
    def emit(gen, addr):
        ins = gen.lift(addr)
        if ins.kind != 'bl' or ins.target != target:
            raise ValueError(f'{addr:#x}: expanded saves need the checked native slot helper')
        return (f'{{ extern void {helper}(Cpu *c); '
                f'c->x[30] = 0x{addr + 4:x}ull; {helper}(c); }}')
    return emit


def _save_auto_image(gen, addr):
    _plain(gen, addr)
    word = gen.word(addr)
    destination, source = word & 31, (word >> 5) & 31
    return f'c->x[{destination}] = (uint32_t)((uint32_t)c->x[{source}] + 99u);'


def _save_initial_max(gen, addr):
    _plain(gen, addr)
    return '(void)subs32(c, (uint32_t)c->x[20], 99u);'


EXPANDED_SAVE_PATCHES = {
    0x7000282ddc: (0x528e2a00, _save_constant(0, 0x1d50), 'extended native state allocation, low half'),
    0x7000282de0: (0x72a00b00, _save_constant(0, 0x1601d50), 'extended native state allocation, complete size'),
    0x70002836f0: (0x321e03e9, _save_constant(9, 100), '100 logical records; four visible cards stay unchanged'),
    0x7000283704: (0xaa1303e1, _save_initialize, 'initialize additional empty records and supply real record sort end'),
    0x70002822e0: (0xd100c3ff, _save_prefix('xcde_expanded_saves_forget', 'c->x[0]'), 'native state destructor; drop live ownership before free'),
    0x70002839e0: (0xb940a800, _save_prefix('xcde_expanded_saves_capture', 'c'), 'confirmed Save/Load selection; remember its stable logical identity'),
    0x700028286c: (0x97f7fc45, _save_legacy('xcde_expanded_saves_legacy_set', 0x7000081980), 'preserve original serialized slot nibble and neighboring flags'),
    0x7000396f18: (0x97f3aaae, _save_legacy('xcde_expanded_saves_default', 0x70000819d0), 'save dialog default; session extended selection or vanilla nibble'),
    0x7000396f80: (0x71000e9f, _save_initial_max, 'native default selection upper clamp is logical99'),
    0x7000396f84: (0x320007e9, _save_constant(9, 99), 'native default selection maximum is logical99'),
}

# [XCDE] LINUX-035: all seven old thumbnail-base constructions, including native
# constructor/reset fills, I/O destinations, missing images and visible binding.
for _low, _register in ((0x70002821ac, 8), (0x70002827dc, 9),
                        (0x70002829e4, 10), (0x7000282ab4, 9),
                        (0x7000283390, 10), (0x70002834f4, 8),
                        (0x70002835d4, 8)):
    EXPANDED_SAVE_PATCHES[_low] = (0x52886500 | _register,
                                  _save_constant(_register, 0x7150), 'relocated thumbnail base, low half')
    EXPANDED_SAVE_PATCHES[_low + 4] = (0x72a002a0 | _register,
                                      _save_constant(_register, 0x587150), 'relocated appended thumbnail base')

# [XCDE] LINUX-035: exactly the three whole-arena fills. Per-thumbnail transfer
# and fallback fill sizes remain the original 320*180*3 bytes.
for _low in (0x70002821b8, 0x70002834d4, 0x70002835b0):
    EXPANDED_SAVE_PATCHES[_low] = (0x52918002, _save_constant(2, 0xac00), '100-thumbnail arena fill, low half')
    EXPANDED_SAVE_PATCHES[_low + 4] = (0x72a00142, _save_constant(2, 0x107ac00), '100-thumbnail arena fill extent')

# [XCDE] LINUX-035: autosave remains logical row0 and filename00a; only its
# independent CPU picture-buffer index moves after the99 manual pictures.
for _site, _word in ((0x70002827c8, 0x11000d2a), (0x70002829d4, 0x11000d09),
                     (0x7000282a9c, 0x11000d2a), (0x7000283384, 0x11000d48)):
    EXPANDED_SAVE_PATCHES[_site] = (_word, _save_auto_image, 'autosave image follows99 manual thumbnail buffers')

for _site, (_word, _emit, _note) in EXPANDED_SAVE_PATCHES.items():
    assert _site not in PATCHES, f'Conflicting expanded save patch at {_site:#x}'
    PATCHES[_site] = Patch(_word, _emit, 'LINUX-035', _note)


# [XCDE] LINUX-035: title Continue and title-feature presence use separate native
# four-slot scanners. The adapter scans extra IDs through their existing async
# reader, with one stable job identity and one retained winning payload.
def _save_scan_hook(helper, argument, after=False):
    def emit(gen, addr):
        ins = gen.lift(addr)
        if ins.kind == 'bl':
            original = _native_call(gen, addr, ins.target)
        elif ins.kind == 'op':
            original = ins.code
        else:
            raise ValueError(f'{addr:#x}: expanded save scanner needs an instruction or native call')
        parameter = 'Cpu *c' if argument == 'c' else 'uint64_t owner'
        hook = f'{{ extern void {helper}({parameter}); {helper}({argument}); }} '
        return original + hook if after else hook + original
    return emit


def _save_scan_wait(gen, addr):
    original = _plain(gen, addr)
    return ('{ extern _Bool xcde_expanded_scan_continue_wait(Cpu *c); '
            'if (xcde_expanded_scan_continue_wait(c)) goto L_700039a0e0; } ' + original)


EXPANDED_SAVE_SCAN_PATCHES = {
    0x7000399d90: (0xd10103ff, _save_scan_hook('xcde_expanded_scan_continue_begin', 'c->x[0]'),
                   'Continue scan begins; bind the native reader owner'),
    0x7000399b20: (0xa9be4ff4, _save_scan_hook('xcde_expanded_scan_continue_drop', 'c->x[0]'),
                   'Continue owner cleanup; release retained extended winner'),
    0x7000399d30: (0xa9be4ff4, _save_scan_hook('xcde_expanded_scan_continue_drop', 'c->x[0]'),
                   'Continue owner destructor; release retained extended winner'),
    0x7000399f78: (0x39426268, _save_scan_wait,
                   'wait for serial extended reads before choosing Continue payload'),
    0x700039a048: (0xb82a6928, _save_scan_hook('xcde_expanded_scan_continue_selected', 'c', after=True),
                   'confirmed Continue selection; preserve full logical identity'),
    0x70003c3c8c: (0x97fffafd, _save_legacy('xcde_expanded_scan_presence', 0x70003c2880),
                   'title presence checker; aggregate extended main and Future Connected saves'),
    0x70003c2de0: (0xa9be4ff4, _save_scan_hook('xcde_expanded_scan_presence_drop', 'LD64(c->x[0] + 0x278)'),
                   'title owner destructor; remove checker ownership'),
    0x70003c2d94: (0x97f2c6bf, _save_scan_hook('xcde_expanded_scan_presence_drop', 'c->x[21]'),
                   'title checker replacement; drop old ownership before native free'),
    0x70003c31fc: (0x52880788, _save_scan_hook('xcde_expanded_scan_presence_reset', 'c->x[21]'),
                   'title recheck; drain any pending extended read before native reuse'),
}
for _site, (_word, _emit, _note) in EXPANDED_SAVE_SCAN_PATCHES.items():
    assert _site not in PATCHES, f'Conflicting expanded save patch at {_site:#x}'
    PATCHES[_site] = Patch(_word, _emit, 'LINUX-035', _note)


# [XCDE] LINUX-036: preserve the original update and its caller CPU results;
# only afterwards permit bounded extra cold metadata transitions. Rendering,
# thumbnail reads, writes and ordinary gameplay retain their native cadence.
def _save_metadata_pump(gen, addr):
    original = _native_call(gen, addr, 0x70002823a0)
    return ('{ uint64_t scan_ui_ = c->x[19], scan_state_ = c->x[0]; '
            + original +
            'extern void xcde_expanded_saves_pump(Cpu *c, uint64_t ui, uint64_t state); '
            'xcde_expanded_saves_pump(c, scan_ui_, scan_state_); }')


PATCHES[0x7000283200] = Patch(0x97fffc68, _save_metadata_pump, 'LINUX-036',
                            'native UI metadata update; original call then bounded owned cold-read work')


# LINUX-040 (runtime/hle/quickstep_*.c): dedicated Quick Step gem slot, opt-in (xcde.ini quickstep_slot). Independently owned native Quick Step control and filtered
# native picker. The original six/eight/six indices and ordinary sockets stay intact.
def _quickstep_prefix(helper, parameter='Cpu *c', argument='c'):
    def emit(gen, addr):
        ins = gen.lift(addr)
        prefix = f'{{ extern void {helper}({parameter}); {helper}({argument}); }} '
        if ins.kind == 'op':
            return prefix + ins.code
        if ins.kind == 'bcond' and isinstance(ins.target, int):
            # Retain the original conditional branch and its already-discovered
            # native label; no return value or native success path is replaced.
            return prefix + f'if ({ins.cond}) goto L_{ins.target:x};'
        raise ValueError(f'{addr:#x}: Quick Step hook requires a plain/conditional instruction')
    return emit


def _quickstep_construct(gen, addr):
    original = _plain(gen, addr)
    return (original + ' { extern void quickstep_native_construct(Cpu *c); '
            'quickstep_native_construct(c); }')


def _quickstep_row_style(gen, addr):
    # Keep the genuine row's disabled-style call and link register. Only an
    # exactly owned eligible bonus row may replace its disabled W1 argument.
    original = _native_call(gen, addr, 0x700020b070)
    return ('{ extern void quickstep_native_picker_row_style(Cpu *c); '
            'quickstep_native_picker_row_style(c); ' + original + '}')


def _quickstep_background(gen, addr):
    original = _plain(gen, addr)
    # Only the private cloned row's baked wide background is omitted. Keep the
    # common native mask-release/frame cleanup; its caller draws child nodes.
    return ('{ extern _Bool quickstep_native_filter_background(Cpu *c); '
            'if (quickstep_native_filter_background(c)) goto L_700075e99c; } '
            + original)


def _quickstep_chain(*helpers):
    def emit(gen, addr):
        original = _plain(gen, addr)
        calls = ' '.join(f'{{ extern void {helper}(Cpu *c); {helper}(c); }}'
                         for helper in helpers)
        return calls + ' ' + original
    return emit


def _quickstep_input(gen, addr):
    original = _plain(gen, addr)
    # This is inside a live native stack frame, after child consumers declined.
    # Follow the native handled epilogue; a C return would strand its guest frame.
    return ('{ extern _Bool quickstep_native_input_event(Cpu *c); '
            'if (quickstep_native_input_event(c)) goto L_70002f9430; } ' + original)


def _quickstep_handled(helper):
    def emit(gen, addr):
        # These hooks are exclusively function entries, before frame allocation.
        original = _plain(gen, addr)
        return (f'{{ extern _Bool {helper}(Cpu *c); '
                f'if ({helper}(c)) return; }} ' + original)
    return emit


def _quickstep_picker_cancel(gen, addr):
    original = _plain(gen, addr)
    # Bounds belong to the runtime adapter: generated C intentionally includes
    # only the CPU contract, not host/HLE address-space policy definitions.
    return ('{ extern void quickstep_native_picker_cancel_callback(Cpu *c); '
            'quickstep_native_picker_cancel_callback(c); } ' + original)


def _quickstep_picker_closed(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'b' or ins.target != 0x70002fea30:
        raise ValueError(f'{addr:#x}: Quick Step close requires its native tail call')
    # The native frame is already restored. Keep LR from its caller unchanged.
    return ('{ extern void quickstep_native_picker_closed(Cpu *c); '
            'quickstep_native_picker_closed(c); } '
            'extern void f_70002fea30(Cpu *c); f_70002fea30(c); return;')


QUICKSTEP_PATCHES = {
    0x70002f67f4: (0xf90e2674, _quickstep_construct, 'publish checked native equipment UI owner'),
    0x7000208d30: (0xf81e0ff3, _quickstep_chain('quickstep_native_tick', 'quickstep_stats_ui_tick'), 'native equipment update then owned effect-row layout/lifetime'),
    0x7000208530: (0xf81e0ff3, _quickstep_chain('quickstep_stats_ui_release_ui', 'quickstep_native_release'), 'release extra effect row before equipment owner destruction'),
    0x7000208590: (0xf81e0ff3, _quickstep_chain('quickstep_stats_ui_release_ui', 'quickstep_native_release'), 'idempotent extra-row cleanup before deleting equipment owner'),
    0x70002f6130: (0xf81b0ff9, _quickstep_chain('quickstep_stats_ui_release_sequence', 'quickstep_native_release_sequence'), 'restore native row vector before sequence resources/owner destruction'),
    0x70002f87ac: (0xb9400288, _quickstep_input, 'bonus control input after native child consumers; native handled epilogue'),
    0x7000183a60: (0xb94fb808, _quickstep_prefix('quickstep_native_picker_filter'), 'owned bonus list filter before native sorts and category rebuilds'),
    0x7000241b40: (0xb9002001, _quickstep_prefix('quickstep_native_picker_count'), 'native bonus row count and genuine No data state'),
    0x7000316604: (0x97fbd29b, _quickstep_row_style, 'owned bonus gem row styling independent of ordinary boot socket eligibility'),
    0x7000232e2c: (0x2a0003f4, _quickstep_prefix('quickstep_slot_native_marker'), 'LINUX-044 display-only native E state for validated bonus ownership; retain original row status move'),
    0x7000237898: (0xb943be68, _quickstep_prefix('quickstep_slot_native_portrait_owner'), 'LINUX-044 R2 display-only portrait owner after native lookup; retain native field load'),
    0x70002378d0: (0x6b00035f, _quickstep_prefix('quickstep_slot_native_portrait_owner'), 'LINUX-044 R2 gem portrait E compares genuine portrait character with validated bonus owner'),
    0x7000237940: (0x6b00035f, _quickstep_prefix('quickstep_slot_native_portrait_owner'), 'LINUX-044 R2 alternate portrait path retains original character comparison'),
    0x70002fa178: (0xf941f7e8, _quickstep_prefix('quickstep_native_picker_menu_item'), 'bonus Remove eligibility after native ordinary-socket checks; retain original load'),
    0x70002f8cd8: (0x37003ac0, _quickstep_prefix('quickstep_native_picker_menu_gate'), 'active bonus context menu ignores ordinary parent socket restriction; retain native branch'),
    0x70002f9970: (0x910b63e9, _quickstep_prefix('quickstep_native_picker_menu_item'), 'active-list bonus Remove item flag; retain native item pointer calculation'),
    0x70002f9a3c: (0x910a43e0, _quickstep_prefix('quickstep_native_picker_menu_opened'), 'bind exact completed native bonus context popup before restoring caller stack'),
    0x700075e868: (0xf941d268, _quickstep_background, 'private bonus row baked background only; native cleanup and child drawing retained'),
    0x7000302640: (0xd10643ff, _quickstep_handled('quickstep_native_picker_confirm'), 'bonus selection before native detach or staged socket writes'),
    0x70003020d0: (0xf81d0ff5, _quickstep_handled('quickstep_native_picker_remove'), 'remove exact owned bonus association only'),
    0x70003004e0: (0xf81d0ff5, _quickstep_handled('quickstep_native_picker_uid'), 'bonus UID for native selected-item actions'),
    0x70002ff870: (0xf81c0ffc, _quickstep_handled('quickstep_native_picker_details'), 'native gem details and comparison for actual bonus item'),
    0x70002fae60: (0xd10103ff, _quickstep_handled('quickstep_native_picker_preview'), 'bonus picker does not preview ordinary boots socket replacement'),
    0x70002fe000: (0xa9bb67fc, _quickstep_handled('quickstep_native_picker_commit'), 'suppress ordinary equipment commit while bonus picker owns selection'),
    0x70002ff5fc: (0x17fffd0d, _quickstep_picker_closed, 'restore original native pane before close tail call'),
    0x70002ffd90: (0x52914288, _quickstep_handled('quickstep_native_picker_eligible'), 'bonus-only native Remove eligibility; other queries unchanged'),
    0x70003035d0: (0xd10603ff, _quickstep_handled('quickstep_native_picker_submenu'), 'bonus Remove and safe handling of ordinary bulk/swap actions'),
    0x7000303270: (0xa9bd57f6, _quickstep_picker_cancel, 'cancel bonus picker before native character change and ordinary commit'),
    0x70003022b0: (0xa9bd57f6, _quickstep_picker_cancel, 'cancel bonus picker before native pane change'),
    0x700008c2bc: (0x710002bf, _quickstep_prefix('quickstep_slot_effects'), 'append owned Quick Step before native effect merge/caps'),
    0x700008c2e8: (0x3600031c, _quickstep_prefix('quickstep_slot_effects_end'), 'release effect scratch after native apply; retain original branch'),
    0x700008d57c: (0x910903ff, _quickstep_prefix('quickstep_slot_effects_finish'), 'final stats epilogue; repair playing effects after a temporary calculation'),
    # [XCDE] LINUX-040: BDEE0 changes favourite, not equipped ownership.
    # Intercept the actual accepted socket write, after all native reject paths.
    0x700017b81c: (0xb8296913, _quickstep_prefix('quickstep_slot_native_equip'), 'actual ordinary gem socket write clears matching bonus; retain native store'),
    0x70000bdf40: (0xd100c3ff, _quickstep_prefix('quickstep_slot_native_consume'), 'ordinary quantity use retains native behavior and clears matching bonus'),
    0x700009bbd0: (0xb40001e1, _quickstep_prefix('quickstep_slot_native_destroy'), 'actual item destruction clears matching bonus; retain null branch'),
}
for _site, (_word, _emit, _note) in QUICKSTEP_PATCHES.items():
    PATCHES[_site] = Patch(_word, _emit, 'LINUX-040', _note)


# [XCDE] LINUX-039: native payloads remain unchanged; optional character-slot
# metadata binds to exact serialized bytes and confirmed load intent.
_QUICKSTEP_CONTINUE_BASE = PATCHES[0x700039a048].emit


def _quickstep_continue_intent(gen, addr):
    return (_QUICKSTEP_CONTINUE_BASE(gen, addr)
            + ' { extern void quickstep_save_continue_intent(Cpu *c); '
            'quickstep_save_continue_intent(c); }')


QUICKSTEP_SAVE_PATCHES = {
    0x70000af760: (0xd10203ff, _quickstep_prefix('quickstep_save_serializing'), 'reconcile active character references before actual save serialization'),
    0x70000afa80: (0xfd400be8, _quickstep_prefix('quickstep_save_serialized'), 'bind optional metadata to completed native serialized payload'),
    0x70000afe10: (0xd102c3ff, _quickstep_prefix('quickstep_save_loading', 'void', ''), 'suspend old character references before actual playing-data load'),
    0x70000afe60: (0x910043e0, _quickstep_prefix('quickstep_save_adopt'), 'adopt only confirmed native playing-data decode'),
    0x700054df24: (0xa9497bfd, _quickstep_prefix('quickstep_save_loaded'), 'reconcile only after native story finalization'),
    0x7000087040: (0xd10503ff, _quickstep_prefix('quickstep_save_read_intent'), 'LINUX-044 actual native save read entry covers all Load and Continue callers; retain native stack frame'),
    0x70005c4234: (0xb9402a88, _quickstep_prefix('quickstep_save_header_copy'), 'LINUX-044 R2 bind actual split-read header copy to raw buffer; retain original loader field load'),
    0x700039a424: (0xaa1303e0, _quickstep_prefix('quickstep_save_cache_copy'), 'LINUX-044 R2 preserve validated save provenance after completed typed cache copy'),
    0x70003992b4: (0xb9400a68, _quickstep_prefix('quickstep_save_selected_copy'), 'LINUX-044 R2 preserve exact selected save provenance after typed constructor copy'),
    0x700039a048: (0xb82a6928, _quickstep_continue_intent, 'confirmed Continue; compose after extended-slot logical correction'),
    0x70000adcc0: (0xd10243ff, _quickstep_prefix('quickstep_save_newgame_begin'), 'actual New Game reset; suspend old references and load intent'),
    0x70000ae860: (0xa9487bfd, _quickstep_prefix('quickstep_save_newgame_end'), 'actual New Game completion; adopt empty slot state after story setup'),
}
for _site, (_word, _emit, _note) in QUICKSTEP_SAVE_PATCHES.items():
    PATCHES[_site] = Patch(_word, _emit, 'LINUX-039', _note)


# [XCDE] LINUX-040 r3: the native summaries have eight fixed records, while
# their renderer supports eleven lines. Supply an independently owned extra
# row at its local-copy boundary, never beyond the original summary allocation.
def _quickstep_stats_count(gen, addr):
    original = _plain(gen, addr)
    return (original + ' { extern void quickstep_stats_count(Cpu *c); '
            'quickstep_stats_count(c); }')


def _quickstep_stats_row(gen, addr):
    original = _plain(gen, addr)
    return ('{ extern int quickstep_stats_row(Cpu *c); '
            'int row_ = quickstep_stats_row(c); '
            'if (row_ > 0) goto L_70002393cc; '
            'if (row_ < 0) goto L_7000239508; } ' + original)


def _quickstep_stats_resume(gen, addr):
    # This address is not an original branch target, so its C label must be
    # explicit. The stale-frame path uses the original native cleanup label.
    return 'L_70002393cc:; ' + _plain(gen, addr)


QUICKSTEP_STATS_PATCHES = {
    0x70002391f0: (0xfc190fe8, _quickstep_prefix('quickstep_stats_begin'),
                   'capture exact owned equipment summary before native frame allocation'),
    0x70002393a4: (0xf940baa8, _quickstep_stats_count,
                   'original bounded count then owned virtual ninth-row count'),
    0x70002393b0: (0x52800508, _quickstep_stats_row,
                   'owned local summary copy or original copy; stale lease uses native cleanup'),
    0x70002393cc: (0xf940bac9, _quickstep_stats_resume,
                   'explicit local-row resume label, retaining original baseline count load'),
    0x7000239420: (0x3945c3e8, _quickstep_prefix('quickstep_stats_compare'),
                   'bonus-aware native comparison flags, retaining native type load'),
    0x70002395d8: (0x910903ff, _quickstep_prefix('quickstep_stats_end'),
                   'release owned summary lease before native frame restoration'),
}
for _site, (_word, _emit, _note) in QUICKSTEP_STATS_PATCHES.items():
    PATCHES[_site] = Patch(_word, _emit, 'LINUX-040', _note)


QUICKSTEP_STATS_UI_PATCHES = {
    0x700023e700: (0xa9be4ff4, _quickstep_prefix('quickstep_stats_ui_release_panel'),
                   'destroy owned extra tail row and restore counts before native panel destructor'),
}
for _site, (_word, _emit, _note) in QUICKSTEP_STATS_UI_PATCHES.items():
    PATCHES[_site] = Patch(_word, _emit, 'LINUX-040', _note)


# [XCDE] T224 / GitHub #181: in-engine cutscenes (rtmevt) have three 30 Hz quantization gates:
#  1. L_70004e6b84 (0x70004e6b9c): rounds the continuous cutscene clock [sp+0x14] to the nearest
#     1/30 s frame before calling f_70004e84c0 (sequence / pose update).
#  2. f_70004f55b0 (0x70004f55f8): camera sequence skips its built-in floor/ceil lerp+slerp path
#     and rounds to an integer 30 Hz frame whenever playback speed == 1.0f.
#  3. f_700050c150 (0x700050c374): sequence track time lookup rounds local track time to 1/30 s
#     when the track's snap flag (+0x220) is set.
# When cutscenes run above 30 fps (cutscenes = same), gates 1 and 2 are bypassed so cameras and
# character poses evaluate smoothly every presented frame at identical playback speed. At 30 fps
# (and in cutscenes = 30 / 30fg) xcde_cutscene_unquantize() returns false so 30 fps stays
# bit-identical.
# Gate 3 is NOT bypassed (GitHub #216): snap-flagged tracks are authored on the exact 30 Hz grid
# and the sequence evaluator matches their key / trigger times against it. Feeding it continuous
# time missed those exact times, so cutscene tracks that switch effects on (the Monado's ether
# glow / ring lens, the sparkle emitters) never fired and the FX were missing in every 60 FPS
# cutscene. The clock and camera gates above keep the smoothness of #181.
def _cutscene_unquantize(gen, addr):
    original = _plain(gen, addr)
    return ('{ extern _Bool xcde_cutscene_unquantize(void); '
            'if (xcde_cutscene_unquantize()) { '
            'fp_op_s(c, FOP_ADD, 8, 0, 13); goto L_70004e6bc0; } } ' + original)


def _cutscene_cam_interp(gen, addr):
    return ('{ extern _Bool xcde_cutscene_unquantize(void); '
            'if (!xcde_cutscene_unquantize() && COND_EQ(c)) goto L_70004f5814; }')


PATCHES[0x70004e6b9c] = Patch(0x1e2c1800, _cutscene_unquantize, 'T224',
                              'fdiv s0, s0, s12 - skip 30 Hz cutscene clock quantization above 30 fps')
PATCHES[0x70004f55f8] = Patch(0x540010e0, _cutscene_cam_interp, 'T224',
                              'b.eq L_70004f5814 - use built-in sub-frame camera lerp+slerp above 30 fps')

# [XCDE] GitHub #179: volumetric cloud scroll speed above 30 FPS.
# f_700062f860 (ml::Scn::update) passes Scene+0x3848 (Scene+0x37d4 * Scene+0x3844)
# to CloudManager::update (f_700061cb70) before per-view setup (f_7000631a20)
# multiplies by the slot's elapsed authored frames at Scene+0x37e0 (UpdateInfo+0xc:
# 1.0 at 30 fps, 0.5 at 60 fps, 0.25 at 120 fps). Multiply by Scene+0x37e0 here so
# volumetric clouds advance at 1x real-time speed at any FPS while staying
# bit-identical at 30 fps (where Scene+0x37e0 == 1.0f) and still pausing when
# Scene+0x37d4 == 0.0f.
def _cloud_dt_scale(gen, addr):
    return ('{ uint32_t step_bits_ = LD32(c->x[19] + 0x3848ull), '
            'dt_bits_ = LD32(c->x[19] + 0x37e0ull), out_bits_; '
            'float step_, dt_, out_; '
            'memcpy(&step_, &step_bits_, 4); memcpy(&dt_, &dt_bits_, 4); '
            'out_ = step_ * dt_; memcpy(&out_bits_, &out_, 4); '
            'c->v[0] = out_bits_; }')


PATCHES[0x700062fa60] = Patch(0xbd784a60, _cloud_dt_scale, 'GH-179',
                              'ldr s0, [x19, #0x3848] -> scale CloudManager dt by Scene+0x37e0 (UpdateInfo+0xc)')


# Native C replacements for hot math/culling kernels (runtime/hle/collision_bvh.c)
PATCHES[0x70006f5460] = Patch(
    0xb9400828,
    lambda gen, addr: '{ extern void xcde_frustum_sphere(Cpu *c); xcde_frustum_sphere(c); return; }',
    'PERF', 'native sphere-vs-frustum test (f_70006f5460)',
)
PATCHES[0x700001cf30] = Patch(
    0xd10143ff,
    lambda gen, addr: '{ extern void xcde_bone_trs_mat(Cpu *c); xcde_bone_trs_mat(c); return; }',
    'PERF', 'native bone TRS-to-matrix (f_700001cf30)',
)
PATCHES[0x7000726950] = Patch(
    0xfc1f0fe8,
    lambda gen, addr: '{ extern void xcde_col_seg_seg(Cpu *c); xcde_col_seg_seg(c); return; }',
    'PERF', 'native segment-segment closest points (f_7000726950)',
)
PATCHES[0x700077d430] = Patch(
    0xd10203ff,
    lambda gen, addr: '{ extern void xcde_anim_eval_vec3(Cpu *c); xcde_anim_eval_vec3(c); return; }',
    'PERF', 'native cubic vec3 keyframe eval (f_700077d430)',
)
PATCHES[0x700077d710] = Patch(
    0xd10203ff,
    lambda gen, addr: '{ extern void xcde_anim_eval_quat(Cpu *c); xcde_anim_eval_quat(c); return; }',
    'PERF', 'native cubic quat keyframe eval + normalize (f_700077d710)',
)
