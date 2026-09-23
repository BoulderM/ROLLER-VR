# ROLLER VR — Phase 2: Stereo Cockpit Camera

**Status:** Implemented; hardware run on `fatal-gpu` (Monado simulated HMD) passed with caveats -- see "Outcome" at the end. No physical headset was ever used.
**Date:** 2026-09-23
**Branch:** `roller-vr`
**Depends on:** `docs/superpowers/specs/2026-09-22-roller-vr-design.md` (overall VR design), Phase 1 (`docs/superpowers/plans/2026-09-22-vr-phase1-walking-skeleton.md`, complete/merged)

## Goal

Render the real, live gameplay scene — an actual drivable track with car and scenery — in stereo to both eyes via OpenXR, with the view driven by live head-pose tracking each frame. This is the first phase where real game content (not solid-color clears) reaches the HMD swapchains, and the first phase where the render path has to coexist safely with SDL_GPU's own Vulkan queue usage.

## Non-goals (this phase)

- Motion controllers / menu interaction (Phase 4).
- Comfort mitigations, frame-pacing tuning, spectator/mirror window (Phase 3).
- Physical-headset visual validation — `fatal-gpu` has no HMD attached; validation is Monado-runtime + log/exit-code correctness only (see Testing).
- Any change to `view.c`/`transfrm.c`'s existing camera-pose computation — Phase 2 consumes that output, it doesn't change it.
- Perf optimization of the copy pipeline (semaphore-based overlap). Deferred to Phase 3 once real frame-time data exists.

## Success criteria

Real game scene (an actual track, loaded and simulated) renders to both eye swapchains via Monado on `fatal-gpu`, with per-eye view driven by live `xrLocateViews()` poses each frame (not fixed/identity). Validated via `XR_APILAYER_LUNARG_core_validation`, clean exit code, and a log-level check that head-pose values actually change frame to frame. No human visual/headset check performed.

## Architecture

### 1. Executable: `roller-vr`

Phase 1's `vr_skeleton_main.c` was a fully standalone executable — no `roller-core` linkage, not present in `roller-core.srclist`. Phase 2 needs real game state (track, car physics, input), so the `-Dvr` build target is restructured into a proper separate build product:

- Renamed from `vr_skeleton` to **`roller-vr`** (`roller-vr.exe` on Windows), mirroring how ROLLER-X was previously structured as its own distinct build rather than a runtime flag on the main executable.
- Still gated entirely behind `-Dvr` in `build.zig` — the default non-VR build has zero OpenXR/raw-Vulkan dependency, unchanged from Phase 1.
- Its module now links `roller-core` sources (the same set `roller-core.srclist` marks `KEEP`, including `scene_render_gpu.c`), reusing the existing track-loading, car-physics-step, and input-handling functions unchanged.
- Only the *presentation* step differs from the main `roller` executable: instead of one windowed SDL swapchain present, `roller-vr` does two windowless `scene_render_gpu` renders per frame (one per eye) and presents each via its OpenXR swapchain.
- Considered and rejected: a runtime `--vr` flag on the main `roller` executable instead of a separate binary. Rejected to preserve Phase 1's isolation guarantee — folding VR into the default executable would put OpenXR/raw-Vulkan dependency risk into every build, for no benefit at this stage.

### 2. Camera: `build_mvp_vr_eye()`

`calculateview()`/`calculatetransform()` (`view.c`/`transfrm.c`) run unchanged each frame, producing the existing cockpit/follow camera pose in the `viewx/viewy/viewz` + `vk1..vk9` globals exactly as the non-VR path does. Instead of one `build_mvp()` call, `roller-vr` computes two:

```c
void build_mvp_vr_eye(float mvp_out[16],
                       const SceneRenderCamera *base_cam,
                       const SceneRenderProjection *base_proj,
                       const XrView *xr_view,
                       float eye_offset_m,
                       int vpW, int vpH);
```

Composition order: base cockpit pose (unchanged car-physics-driven pose) → rotated/translated by the head-pose delta from `xr_view->pose` → offset again along the head's local right axis by `eye_offset_m` (per-eye IPD half-distance). `xr_view` comes from `xrLocateViews()` called once per frame against the `XR_REFERENCE_SPACE_TYPE_LOCAL` space Phase 1 already created (no space-type change — Phase 1 created it but never located views against it; Phase 2 starts actually using it). Near/far planes move from ROLLER's fixed defaults to tighter VR-appropriate values (exact numbers set during implementation, revisited in Phase 3 comfort testing). Resolution mismatch between the XR swapchain size and ROLLER's native 640×400 reference is handled by the existing `scene_render_gpu_set_projection_reference_height()` mechanism — no new scaling logic required.

### 3. Render/copy pipeline (fence-serialized, GPU-to-GPU)

Per eye, per frame:

1. Set the VR-eye camera/projection (above) on a windowless `SceneRendererGPU` (`scene_render_gpu_create_windowless`).
2. Run the existing render passes (3D scene, signs, HUD — same structure as `scene_render_gpu_end_frame_internal()`) but stop after the resolve step instead of doing the CPU-readback download. CRT filter and other windowed-only post-processing are correctly excluded, since the windowless path never reaches that branch.
3. Wait for that render's GPU fence (`SDL_WaitForGPUFences`) — serializing rather than overlapping with SDL_GPU's other queue usage, per Phase 1's finding that concurrent submission to the shared queue is the unresolved risk.
4. Issue a raw `vkCmdCopyImage` from the resolved offscreen texture into the acquired OpenXR swapchain image, using the same barrier pattern Phase 1's clear-color code already established (`UNDEFINED`→`TRANSFER_DST_OPTIMAL`→XR's expected layout).
5. Submit, then continue with `xrReleaseSwapchainImage`/`xrEndFrame` as Phase 1 already does.

**New SDL fork addition required:** SDL_GPU has no existing way to obtain the raw `VkImage` backing an `SDL_GPUTexture`. This phase adds one more small property to `Zizin13/SDL` branch `vr-gpu-properties` — a texture-level `SDL_GetPointerProperty` accessor, following the exact pattern Phase 1 already used for device-level properties (`VkInstance`/`VkPhysicalDevice`/`VkDevice`/`VkQueue`). No other SDL surface area changes.

Semaphore-based overlap (avoiding the CPU fence-wait stall) was considered and explicitly deferred to Phase 3: it requires more fork surgery and is the kind of premature optimization Phase 1's own lessons warn against attempting without real frame-time data first.

### 4. Error handling

Carries forward Phase 1's hardening unchanged: eye/view-count bounds checks, and the GPU-mismatch safety check (`xrGetVulkanGraphicsDevice2KHR` result vs. SDL_GPU's physical device). New for Phase 2:

- Validate `xrLocateViews()`'s returned `viewStateFlags` each frame. If `XR_VIEW_STATE_POSITION_VALID_BIT` or `XR_VIEW_STATE_ORIENTATION_VALID_BIT` is unset for either eye (tracking loss), fall back to the last-known-good pose rather than feeding an invalid pose into `build_mvp_vr_eye()`.
- Development builds run against `XR_APILAYER_LUNARG_core_validation`.
- New OpenXR/Vulkan calls are authored directly against the spec's Valid Usage sections and the Khronos `hello_xr` sample, not from recall — per Phase 1's explicit lesson (both of Phase 1's hardware bugs came from writing calls from memory).

## Testing / validation strategy

Deploy `roller-vr` to `fatal-gpu`, run against Monado with a real track loaded, for a bounded frame count (following Phase 1's 120-frame precedent). Pass criteria:

- Exit code 0.
- `XR_APILAYER_LUNARG_core_validation` output clean (no validation errors).
- Both eyes' `xrEndFrame` submissions succeed every frame.
- Log-level check confirming per-frame head-pose values actually change frame to frame (proves live tracking is wired end-to-end, not merely present-but-ignored as in Phase 1).

No human visual/headset check is performed — `fatal-gpu` has no physical HMD attached, consistent with how Phase 1 was validated.

## Open items for implementation planning

- Exact near/far plane values and eye-offset (IPD) default — set during implementation, not blocking design approval.
- Precise integration point in `roller-core`'s existing per-tick simulation loop that `roller-vr` calls into (identify exact function during plan-writing, not re-derived here).
- The already-known "known limitation" from the overall VR design spec (SDL fork hosting: `vr-gpu-properties` branch is local-only at `/root/sdl-vr-fork`, not gated behind `-Dvr` in `build.zig.zon`, no hash pin) remains unresolved and orthogonal to this phase's design — tracked separately, not a blocker for writing or executing the Phase 2 plan on this dev LXC.

## Lessons carried into Phase 2 (from Phase 1)

- Author new OpenXR/Vulkan calls against spec Valid Usage sections + `hello_xr`, not from memory.
- Run against `XR_APILAYER_LUNARG_core_validation`.
- Resolve VkQueue external synchronization between SDL_GPU's own submits and VR rendering before submitting real scene content to the shared queue — addressed here via fence-wait serialization (Section 3).

## Outcome (2026-09-23, `fatal-gpu` + Monado simulated HMD)

**Result: PASS on the spec'd criteria, with caveats below. No human wore or looked at a headset.**

Setup: `zig build -Dvr -Doptimize=ReleaseSafe` -> `roller-vr` copied to `fatal-gpu`, real Xorg :1 (DRI3), `monado-service` running (the runtime is an IPC client -- without the service `xrCreateInstance` fails; note `monado-service` needs a non-/dev/null stdin, e.g. `sleep infinity | monado-service`), then
`roller-vr --vr --vr-frames 300 --whiplash-root <dir> --track-path <dir>/fatdata/TRACK1.TRK`.

- **Exit code 0**, game-initiated quit (`--vr-frames N` sets `quit_game`) reached `frontend_shutdown_finish()` -> `vr_shutdown()` -> clean process exit. (`--vr-frames N` is a Phase 2 addition; without it `roller-vr --vr` runs until closed.)
- **Both eyes submitted every rendered tick**: 300/300 (and 2500/2500 in a longer run) `xrEndFrame` calls with `viewCount = 2`, `layerCount = 1`; the eyes are 896x1007 each.
- **Live head pose**: Monado's simulated HMD sways, and `view[0]` position/orientation change frame to frame in the log (e.g. pos x from 0.020 to 0.120 to -0.079 over 300 frames), so `xrLocateViews()` is wired end-to-end.
- **Actual pixels seen** (a screenshot of Monado's desktop compositor window, not a headset): a side-by-side stereo pair of the live game scene (sky, track, walls, cars), both eyes populated and slightly different viewpoints.
- **`XR_APILAYER_LUNARG_core_validation`**: ran (initialize/destroy debug lines confirm it was active); zero warnings/errors.
- **`VK_LAYER_KHRONOS_validation` with synchronization validation** (extra, not in the original criteria): found and fixed two real bugs in `vr_runtime.c` (see below). Remaining messages are not from Phase 2 code: 12+12 `VUID-VkImageMemoryBarrier-None-09120`/`image-09118` (Monado's own client-side release barrier to `VK_QUEUE_FAMILY_EXTERNAL` on a `VkInstance` that SDL creates at `apiVersion` 1.0 without the external-memory extensions the OpenXR runtime asks for via `xrGetVulkanDeviceExtensionsKHR`) and 2x `VUID-VkShaderModuleCreateInfo-pCode-08737` (pre-existing `HUDPixelUniforms` std140 alignment in `game_hud_pixel.hlsl`).

**Bugs found by the first real run and fixed:**
1. `roller-vr` segfaulted in the software car renderer: VR mode has no menu renderer, so `game_render_set_mode(GPU)` was never called and the game renderer stayed in its software default. `3d.c` now forces GPU mode when `g_bVRMode`.
2. Sync-validation hazards on the eye copy: all barrier access masks were 0. Now `TRANSFER_READ`/`TRANSFER_WRITE` in the pre-copy `dstAccessMask` and post-copy `srcAccessMask`. Also added `XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT` to the eye swapchains (Monado transitions to `COLOR_ATTACHMENT_OPTIMAL`; the missing usage bit caused 20 `oldLayout-01208` errors).

**Caveats / not covered:**
- The run plays the game's intro/attract-mode race (`intro = -1`, `replaytype = 2` in `main()`), not a player-driven race. After ~800 rendered frames the demo ends and the frontend falls through RESULTS -> TITLE -> the intro race again; for that stretch (~1000 ticks) no menu is rendered in VR and empty (`layerCount = 0`) frames are submitted. Player-controlled racing in VR was not exercised.
- Head-pose-driven view was checked in logs and via one screenshot pair only; comfort, IPD (`VR_METERS_TO_WORLD` = 100.0 is a placeholder), HUD (not rendered), and 2-player (player 1 only) are unchanged Phase 2 limitations.
- Only Monado's simulated HMD/desktop compositor was tested. The SDL-owned Vulkan device not enabling the runtime-requested extensions (above) is likely to matter for other runtimes (SteamVR, Windows) -- untested.
- Windows: `zig build -Dvr -Dtarget=x86_64-windows-gnu` panics without `OPENXR_SDK_PATH`/`VULKAN_SDK` (by design); with dummy paths set it reported no C compile errors and failed only at link on the missing `openxr_loader`/`vulkan-1` libs. No Windows binary was produced or run.
- The two Task 7 deferred items (eye MVP override left set if swapchain acquire/wait fails; runtime-initiated STOPPING never reaching `vr_shutdown()`) were not tripped by the run.

