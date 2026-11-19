# Windows async vertex and effect corruption (T-VCACHE-ASYNC, T-FRAME-LAG-VCACHE)

Two separate defects in the Windows async renderer produced the same class of
symptom: draws read vertex data that belonged to a different frame, so geometry
and effect/particle streams produced wrong meshes or huge stretched quads. The
first is an eviction bug in the vertex/index cache; the second is a refresh bug
in the one-frame-in-flight ("frame lag") path.

Both are specific to the Windows renderer configuration. They are inert on
Linux: RADV imports guest memory through the dma-buf GTT path and `pt_init()`
returns false without `MEM_WRITE_WATCH`, so neither the vertex cache nor frame
lag is active there. The code paths are shared, but the faulty states cannot be
reached on Linux.

## T-VCACHE-ASYNC — premature vertex/index cache eviction

The eviction probe and LRU budget loop in `vcache_lookup`
(`runtime/nvn/vulkan/exec.c`) used `used_seq < vk_pending_seq()` to decide
whether a device-local buffer entry was safe to reuse or evict.

With the async render worker (`XCDE_ASYNC_RENDER=1`), the game thread records
frame N+1 while the worker is still processing and submitting frame N.
`vk_pending_seq()` returns `submit_seq + 1`, but `submit_seq` is only
incremented inside the worker's `vk_submit_extra`, so it has not advanced for
frame N's job yet. Frame N's vertex-cache entries therefore appeared evictable
immediately, and their device-local `VkBuffer`s could be reused or freed while
the worker's command buffer still referenced them. The result was wrong
geometry, wrong textures and graphical corruption with async enabled.

Fix: in `vcache_lookup`, when the async worker is active, eviction eligibility
uses `vk_completed_seq()` (the GPU timeline semaphore value) instead of the
pre-submission `pend - 1`. A buffer is only eligible for reuse once the GPU has
signalled completion of the sequence that used it. `vc_free_dead` already used
`vk_completed_seq()` correctly; only the eviction probe and LRU loop were wrong.
The fix gates on `backend_async()`, forward-declared before the vertex-cache
section. The synchronous path is unchanged.

## T-FRAME-LAG-VCACHE — stale vertex-cache refresh under frame lag

Report: with the async worker enabled, large battle VFX (beams, glows, auras)
stretch across the screen. Setting `XCDE_ASYNC_RENDER=0` removes the corruption
but costs about 50 FPS. The report was for the Windows ZIP.

Cause is the one-frame-in-flight path (`nvnSyncWait` in
`runtime/nvn/objects.c`), which is only enabled with the async worker, the
vertex cache and above 60 FPS. Frame lag deliberately stops waiting for the
current frame's GPU work, so the CPU starts writing the next frame's data while
the GPU is still a frame behind. That is safe only while everything the GPU
reads for that frame has already been copied out of guest memory by the CPU.
`vc_flush` was not: it recorded a *GPU-time* copy from guest memory for the
vertex-cache pages the CPU had just written, so the copy could execute after
the game rewrote those pages with the next frame's contents. Effect and
particle streams are exactly the buffers that change completely between frames,
so the affected draws produced huge stretched quads instead of a small
one-frame offset.

Fix: when frame lag is latched, `vc_flush` copies the written pages into the
transient ring at submit time and records the cache refresh from there. The ring
chunk is not reused before its submission completes, so the GPU reads the bytes
that belonged to the frame it is drawing. Entries with GPU-written pages keep
the original GPU-time copy (those bytes are only current on the GPU timeline).
`XCDE_VC_STAGE=0` restores the previous behaviour for an A/B. With frame lag off
the code path is unchanged.

Follow-up (`T-FRAME-LAG-HOST-READS`, 2026-11-19): the compute path's `gfast`
copy-in and the creation fill of the vertex mirrors now snapshot their
CPU-written ranges through the same transient ring, so a frame-behind GPU no
longer reads the next frame's bytes for those. Ranges the GPU writes keep the
GPU-time copy. Graphics shader storage buffers (bound straight to the imported
heap) are still read on the GPU timeline; if corruption survives, that is the
remaining path, and `XCDE_FRAME_LAG=0` remains the safe fallback.

## Relation to open reports

Issue #171 ("Vertex Explosion in Gaur Plains on v16.0.0") describes the same
symptom class on Windows: geometry exploding after a camera turn. Both fixes
above are present in the `v16.0.0` source tree, so a report that survives on
v16 may be one of the remaining lag-time host reads or a separate vertex-mirror
issue.

Useful diagnostics for a new report, in order of increasing cost:

- `XCDE_ASYNC_RENDER=0` — confirms the async worker is involved (large FPS cost).
- `XCDE_FRAME_LAG=0` — disables the one-frame-in-flight path.
- `XCDE_VC_STAGE=0` — restores the old `vc_flush` behaviour for an A/B.

Ask for `build/last_game.log`; the renderer logs whether async and frame lag are
active.

## Verification status

Both changes were reviewed without a local C toolchain at the time, so no
build, package or gameplay run was performed for these write-ups. The user A/B
for `XCDE_FRAME_LAG=0` and `XCDE_VC_STAGE=0` remained pending. The source at tag
`v16.0.0` contains both code paths (`vc_lag`, `XCDE_VC_STAGE`, and the
`vk_completed_seq()` eviction check).
