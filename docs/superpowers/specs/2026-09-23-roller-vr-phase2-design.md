# ROLLER VR — Phase 2: Stereo Cockpit Camera

**Status:** Implemented as a stereo rendering pipeline that shows the game's attract-mode race in both eyes. Hardware run on `fatal-gpu` (Monado simulated HMD) passed the render-path criteria with caveats -- see "Outcome". **The goal below (an actual drivable track) is NOT met**: `roller-vr` has no input and no audio (see "Known gaps"). Only part of the scene is head-tracked. No physical headset was ever used. A final whole-branch review fix wave (2026-09-23) corrected the eye-separation maths, tracking fallback, CLI/frame-limiter leakage and this document's claims; see "Final review fix wave".
**Date:** 2026-09-23
**Branch:** `roller-vr`
**Depends on:** `docs/superpowers/specs/2026-09-22-roller-vr-design.md` (overall VR design), Phase 1 (`docs/superpowers/plans/2026-09-22-vr-phase1-walking-skeleton.md`, complete/merged)

## Goal

Render the real, live gameplay scene — an actual drivable track with car and scenery — in stereo to both eyes via OpenXR, with the view driven by live head-pose tracking each frame. This is the first phase where real game content (not solid-color clears) reaches the HMD swapchains, and the first phase where the render path has to coexist safely with SDL_GPU's own Vulkan queue usage.

**What was actually delivered:** the stereo render/copy/submit pipeline, run against the game's intro/attract-mode race (a replay, not player-driven). The "drivable" part of the goal was not delivered: the design reuses the game loop but the VR boot path (`InitSDL()`'s `g_bVRMode` branch) returns before `InputInit()`/MIDI, drops `SDL_INIT_AUDIO`, and has no window (so no keyboard focus), so nobody can drive the car, and there is no way to quit from inside the headset except the runtime tearing the session down (which skips teardown, see Follow-ups). This is a plan-level gap (the plan's task that created the VR boot path never included input), not something exercised-but-untested.

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
- Its module now links `roller-core` sources (the same set `roller-core.srclist` marks `KEEP`, including `scene_render_gpu.c`), reusing the existing track-loading and car-physics-step functions unchanged. (Input handling is *not* reused in practice: the VR boot path never initialises it -- see "Known gaps".)
- Only the *presentation* step differs from the main `roller` executable: instead of one windowed SDL swapchain present, `roller-vr` does two windowless `scene_render_gpu` renders per frame (one per eye) and presents each via its OpenXR swapchain.
- Considered and rejected: a runtime `--vr` flag on the main `roller` executable instead of a separate binary. Rejected to preserve Phase 1's isolation guarantee — folding VR into the default executable would put OpenXR/raw-Vulkan dependency risk into every build, for no benefit at this stage.

### 2. Camera: `build_mvp_vr_eye()`

`calculateview()`/`calculatetransform()` (`view.c`/`transfrm.c`) run unchanged each frame, producing the existing cockpit/follow camera pose in the `viewx/viewy/viewz` + `vk1..vk9` globals exactly as the non-VR path does. Instead of one `build_mvp()` call, `roller-vr` computes two:

```c
void build_mvp_vr_eye(float mvp_out[16],
                       const SceneRenderCamera *base_cam,
                       const SceneRenderProjection *base_proj,
                       const XrView *xr_view,
                       int vpW, int vpH);
```

Composition order: base cockpit pose (unchanged car-physics-driven pose) → rotated/translated by the head-pose delta from `xr_view->pose`. `xr_view` is one eye's `XrView`, so its pose already contains that eye's own position including the runtime's IPD.

> **Plan error, corrected in the final review wave:** this section (and the plan) originally prescribed a further per-eye offset of ±`eye_offset_m` (31.5 mm, `VR_EYE_OFFSET_M`) along the head's right axis. That counted the IPD twice: `xrLocateViews` returns per-eye poses that already include the IPD, so the effective eye separation was the runtime's IPD plus 63 mm, and the pose rendered no longer matched the pose submitted in `XrCompositionLayerProjectionView.pose` (which is the unmodified `xr_view->pose`). The `eye_offset_m` parameter and `VR_EYE_OFFSET_M` were removed; the head-pose position delta is kept. `vr_mvp_math_test.c` now checks that the two eyes end up exactly one runtime-IPD apart.

`xr_view` comes from `xrLocateViews()` called once per frame against the `XR_REFERENCE_SPACE_TYPE_LOCAL` space Phase 1 already created (no space-type change — Phase 1 created it but never located views against it; Phase 2 starts actually using it). Near/far planes move from ROLLER's fixed defaults to tighter VR-appropriate values (exact numbers set during implementation, revisited in Phase 3 comfort testing). Resolution mismatch between the XR swapchain size and ROLLER's native 640×400 reference is handled by the existing `scene_render_gpu_set_projection_reference_height()` mechanism — no new scaling logic required.

### 3. Render/copy pipeline (fence-serialized, GPU-to-GPU)

Per eye, per frame:

1. Set the VR-eye camera/projection (above) on a windowless `SceneRendererGPU` (`scene_render_gpu_create_windowless`).
2. Run the existing render passes (3D scene, signs, HUD — same structure as `scene_render_gpu_end_frame_internal()`) but stop after the resolve step instead of doing the CPU-readback download. CRT filter and other windowed-only post-processing are correctly excluded, since the windowless path never reaches that branch.
3. Wait for that render's GPU fence (`SDL_WaitForGPUFences`) — serializing rather than overlapping with SDL_GPU's other queue usage, per Phase 1's finding that concurrent submission to the shared queue is the unresolved risk.
4. Issue a raw `vkCmdCopyImage` from the resolved offscreen texture into the acquired OpenXR swapchain image, using the same barrier pattern Phase 1's clear-color code already established (`UNDEFINED`→`TRANSFER_DST_OPTIMAL`→XR's expected layout).
5. Submit, then continue with `xrReleaseSwapchainImage`/`xrEndFrame` as Phase 1 already does.

**New SDL fork addition required:** SDL_GPU has no existing way to obtain the raw `VkImage` backing an `SDL_GPUTexture`. This phase adds one more small property to `Zizin13/SDL` branch `vr-gpu-properties` — a texture-level `SDL_GetPointerProperty` accessor, following the exact pattern Phase 1 already used for device-level properties (`VkInstance`/`VkPhysicalDevice`/`VkDevice`/`VkQueue`). No other SDL surface area changes. The property is a snapshot the backend must keep current: `VULKAN_CreateTexture` set it once, but the backend later re-points `container->activeTexture` (cycling, `VULKAN_INTERNAL_DefragmentMemory`, which then releases the old image), which left a stale `VkImage` (a latent use-after-destroy on the eye copy; found by the final review). Fixed in the fork (`VULKAN_INTERNAL_UpdateContainerImageProperty()`, called wherever `activeTexture` is re-pointed), and `vr_runtime.c` re-reads the property every eye. `roller-vr` therefore needs SDL fork commit `d3eeba3` or later, and the property-refresh commit on top of it (`gpu/vulkan: keep SDL_PROP_GPU_TEXTURE_VULKAN_IMAGE_POINTER current across cycling and defragmentation`); the `.sdl` dependency is an unpinned symlink to the fork working tree, so a stale fork checkout silently builds the old behaviour.

Semaphore-based overlap (avoiding the CPU fence-wait stall) was considered and explicitly deferred to Phase 3: it requires more fork surgery and is the kind of premature optimization Phase 1's own lessons warn against attempting without real frame-time data first.

### 4. Error handling

Carries forward Phase 1's hardening unchanged: eye/view-count bounds checks, and the GPU-mismatch safety check (`xrGetVulkanGraphicsDevice2KHR` result vs. SDL_GPU's physical device). New for Phase 2:

- Validate `xrLocateViews()`'s returned `viewStateFlags` each frame. Orientation and position validity are handled independently (`vr_merge_view()` in `vr_view_merge.h`): each component comes from the located view when its valid bit is set, else from the last-known-good view, else identity/zero. The FOV is always the one the runtime returned from the successful call (never zeroed), so an orientation-only (3DoF) headset renders, and the first frame cannot produce a zero-FOV `tanf(0)` divide-by-zero. The tick is skipped only if `xrLocateViews` itself fails and there is no last-good view. (The original version required both bits and, with no last-good pose, built an identity view with zero FOV -> inf/NaN MVP; that was fixed in the final review wave.)
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

- **Plain `roller` vs the phase base (`3e511db`), the check that was originally done against the wrong base:** the first check compared against `51efa3f`, which already contained the shared-file changes of Tasks 2-8, so it proved nothing about them. Redone in the final review wave: a plain `roller` (default `zig build`, no `-Dvr`, `-Doptimize=ReleaseSafe`) was built from a clean checkout of `3e511db` and another from the final branch HEAD (with all fixes), then the snapshot harness's 21 invocations (7 replays, 14 scenes; the same arguments `zig build test-snapshots` uses) were run serially with each binary and the outputs compared with `diff -rq`. **Result: both produced the same 40 PNGs (two scenes, `menu-main` and `menu-configure`, write no image on this host with either binary) and all 40 are byte-identical.** The two binaries themselves differ (they contain different code). What that shows: the rendering paths the snapshot harness drives (replays and menu scenes) are pixel-unchanged. What it does not show: anything the harness does not exercise (interactive input, audio, the windowed swapchain path, `--gpu-parity`, multiplayer). Both builds used the same SDL fork working tree.
  The complete list of source changes visible to a plain (non-`-Dvr`) build, all runtime-gated on `g_bVRMode` (which is 0 in every plain run: `--vr` is only parsed under `ROLLER_VR_BUILD`) or purely additive: the globals `g_bVRMode`, `g_iVRMaxFrames`, `g_szVRTrackPath` (the last is defined and declared but not read anywhere); the `InitSDL()` VR branch; audio-init-flag and `ROLLERGetAudioInfo()` gating on `!g_bVRMode`; the start-state ternary in `main()` (`g_bSnapshotMode || g_bVRMode`); the `--gpu-parity` rejection of `g_bVRMode`; the `UpdateSDL()` limiter/background-cap `!g_bVRMode` checks; the `scene_render_gpu_end_frame_internal()` fence handoff, which is keyed on a *windowless* renderer (`bWantFence = bReadback || !r->window`) rather than on VR (non-readback end-frames on a windowless renderer now keep their submit fence for `get_last_fence()`; no such call site was identified in a plain build, but nothing tests it); new additive `scene_render_gpu`/`game_render` VR entry points and the `scene_render_gpu_build_vp()` override check (a `bool` that is never set in a plain run); and the `build.zig` refactor (`game_c_sources`/`desktop_c_sources` slices, `configureDesktopPlatform()`, extra parameter on `configureDependencies()`).
- **Exit code 0**, game-initiated quit (`--vr-frames N` sets `quit_game`) reached `frontend_shutdown_finish()` -> `vr_shutdown()` -> clean process exit. (`--vr`/`--vr-frames N` exist only in `roller-vr`; the plain `roller` treats them as unknown options. Without `--vr-frames`, `roller-vr --vr` runs until the OpenXR runtime ends the session -- there is no way to quit from inside the headset.)
- **Both eyes submitted every rendered tick**: every tick that reached `xrEndFrame` with views submitted `viewCount = 2`, `layerCount = 1` (the counter only counts such ticks, so "N/N" is not itself evidence of coverage; the meaningful number is the empty-tick count: 1 empty tick in each ReleaseSafe 300-frame run (673 in the slower Debug run), 974 empty ticks out of 2474 in a 1500-frame run, see caveats); the eyes are 896x1007 each.
- **Live head pose**: Monado's simulated HMD sways, and `view[0]` position/orientation change frame to frame in the progress lines every 60 frames (e.g. `task-9-logs/roller-vr-fixwave-val.log`: `pos=(0.020,-0.050,-0.003)` at frame 1, `(0.129,-0.057,-0.008)` at 60, `(-0.012,0.039,0.005)` at 120, `(0.148,0.040,-0.015)` at 240), so `xrLocateViews()` is wired end-to-end. (An earlier draft of this document cited x = 0.020/0.120/-0.079 with no committed log behind them; removed.)
- **Actual pixels seen** (Monado's desktop compositor window, not a headset; screenshot `task-9-logs/fixwave_shot1.png`, taken after the eye-separation fix): the window shows two eye images side by side, both populated with the live game scene (sky, track, walls, cars). I could not open the image with the viewer tool in the fix session, so I analysed it numerically instead: the right eye is the left eye displaced by exactly the window layout offset (640 px), mean absolute difference 1.5/255 over the 384 px overlap, 3.3% of pixels differing by more than 8 levels and 1.2% by more than 32 (car/wall edges), with a sharp minimum at exactly that offset. So the two views are nearly identical with only a small disparity on near objects: expected, since a 63 mm IPD times `VR_METERS_TO_WORLD` (100, a placeholder) is only 6.3 world units. This screenshot shows both eyes being rendered with the same scene; it does not demonstrate convincing stereo depth. (An earlier draft said "slightly different viewpoints" and, in the task report, "large parallax"; the first was unsupported and the second contradicted by the numbers and by the doubled IPD, which was a bug.)
- **`XR_APILAYER_LUNARG_core_validation`**: ran (initialize/destroy debug lines confirm it was active); zero warnings/errors.
- **`VK_LAYER_KHRONOS_validation` with synchronization validation** (extra, not in the original criteria): found and fixed real bugs in `vr_runtime.c` (see below). Final counts, 300-frame ReleaseSafe run under both validation layers, re-run after the final fix wave (`task-9-logs/roller-vr-fixwave-val.log`; identical to `roller-vr-layout-val.log` and `roller-vr-final-val.log`): 12x `VUID-VkImageMemoryBarrier-None-09120`, 12x `VUID-VkImageMemoryBarrier-image-09118`, 2x `VUID-VkShaderModuleCreateInfo-pCode-08737`, zero `SYNC-HAZARD` (these are raw `grep -c` line counts; each validation message contributes two matching lines, so they are 6/6/1 distinct messages: 2 eyes x 3 swapchain images for the first two). The Debug/UBSan run (`roller-vr-debug.log`) shows 4x 08737 (2 distinct) and 0 `runtime error` lines; the second 08737 is the HUD pixel shader being compiled again when the game reloads the race after the demo ends (that run, unlike the ReleaseSafe 300-frame ones, got that far), the same pre-existing message, not a new one. A validated 1200-frame ReleaseSafe run (`roller-vr-fixwave-val1200.log`, which also crosses the demo end and reload) shows the same 12/12/4 raw counts, zero hazards, exit 0. The 09120/09118 pair is an **OPEN ITEM, an app-side gap, not runtime noise**: see "Open item: Vulkan instance/device extensions" below. 08737 is a pre-existing `HUDPixelUniforms` std140 alignment problem in `game_hud_pixel.hlsl` (not VR code).

**Bugs found by the first real run and fixed:**
1. `roller-vr` segfaulted in the software car renderer: VR mode has no menu renderer, so `game_render_set_mode(GPU)` was never called and the game renderer stayed in its software default. `3d.c` now forces GPU mode when `g_bVRMode`.
2. Sync-validation hazards on the eye copy: all barrier access masks were 0. Now `TRANSFER_READ`/`TRANSFER_WRITE` in the pre-copy `dstAccessMask` and post-copy `srcAccessMask`. Also added `XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT` to the eye swapchains (Monado transitions to `COLOR_ATTACHMENT_OPTIMAL`; the missing usage bit caused 20 `oldLayout-01208` errors).

3. Swapchain image layout at release: the eye copy used to leave the OpenXR swapchain image in `VK_IMAGE_LAYOUT_GENERAL`. The spec ("Swapchain Image Layout" under `XR_KHR_vulkan_enable`/`enable2`) says: on `xrReleaseSwapchainImage` the runtime must interpret the image as "Having a memory layout compatible with `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` for color images ... Being owned by the `VkQueue` specified in `XrGraphicsBindingVulkanKHR`", and "The application is responsible for transitioning the swapchain image back to the image layout and queue ownership that the OpenXR runtime requires. If the image is not in a layout compatible with the above specifications the runtime may exhibit undefined behavior." The post-copy barrier now transitions to `COLOR_ATTACHMENT_OPTIMAL` (src `TRANSFER_WRITE`, dst `COLOR_ATTACHMENT_READ|WRITE`, dst stage `COLOR_ATTACHMENT_OUTPUT`). Re-run: validation counts unchanged (12/12/2, zero hazards, nothing new), 300/300 ticks with `views=2 layerCount=1`, exit 0.

**Open item: Vulkan instance/device extensions requested by the OpenXR runtime are never enabled (real, app-side).** `vr_runtime.c` never calls `xrGetVulkanInstanceExtensionsKHR` / `xrGetVulkanDeviceExtensionsKHR` (verified by grep: it only loads `xrGetVulkanGraphicsRequirements2KHR` and `xrGetVulkanGraphicsDevice2KHR`), and never enables what those would return, because SDL creates the `VkInstance` (apiVersion 1.0, `SDL_gpu_vulkan.c`) and `VkDevice` itself. The spec's valid usage for `XrGraphicsBindingVulkanKHR` says the instance "must have enabled all the instance extensions specified by `xrGetVulkanInstanceExtensionsKHR`" and the device "must have enabled all the device extensions specified by" the device-extensions call; it also allows a runtime to require a specific API version. Monado on Mesa/Intel works anyway (only the two validation messages above), but this is an architecture-level gap in the Phase 1 design: fixing it needs the SDL fork to let the caller supply extra instance/device extensions and a newer `apiVersion` (or an alternative: create the instance/device via `xrCreateVulkanInstanceKHR`/`xrCreateVulkanDeviceKHR` and hand them to SDL). Deliberately not fixed in Phase 2. Expect it to matter on SteamVR and Windows runtimes; untested there.

**Caveats / not covered:**
- The 300-frame ReleaseSafe validated runs never reached the post-demo empty-frame stretch, but other validated runs did: the Debug/UBSan run under both layers (`roller-vr-debug.log`) went through frontend states 15 then 3, reloaded the race and rendered 47 more frames after frame 253 with no new validation message, and the 1200-frame validated run (`roller-vr-fixwave-val1200.log`) did the same (974 empty ticks, then layered frames again). An unvalidated 1500-frame run is `roller-vr-run4.log`. The earlier "2500/2500" claim has no log and has been dropped.
- The tracking-loss fallback (`viewStateFlags` not valid) was not exercised against a runtime (Monado's simulated HMD always reports valid poses); `vr_merge_view()` is unit-tested in isolation (`vr_mvp_math_test.c`).
- The run plays the game's intro/attract-mode race (`intro = -1`, `replaytype = 2` in `main()`), not a player-driven race. After ~800 rendered frames the demo ends and the frontend falls through RESULTS -> TITLE -> the intro race again; for that stretch (~1000 ticks) no menu is rendered in VR and empty (`layerCount = 0`) frames are submitted. Player-controlled racing in VR was not exercised.
- Head-pose-driven view was checked in logs and via one screenshot pair only; comfort, IPD (`VR_METERS_TO_WORLD` = 100.0 is a placeholder), HUD (not rendered), and 2-player (player 1 only) are unchanged Phase 2 limitations.
- Only Monado's simulated HMD/desktop compositor was tested. The unresolved extension-enabling gap (open item above) is likely to matter for other runtimes (SteamVR, Windows) -- untested.
- Windows: `zig build -Dvr -Dtarget=x86_64-windows-gnu` panics without `OPENXR_SDK_PATH`/`VULKAN_SDK` (by design); with dummy paths set it reported no C compile errors and failed only at link on the missing `openxr_loader`/`vulkan-1` libs. No Windows binary was produced or run.
- The two Task 7 deferred items (eye MVP override left set if swapchain acquire/wait fails; runtime-initiated STOPPING never reaching `vr_shutdown()`) were not tripped by the run. The first is harmless; the second is Follow-up 2.

## Known gaps

1. **No input and no audio in VR mode; the drivable-track goal is not met.** `InitSDL()`'s `g_bVRMode` branch returns right after creating the windowless GPU device: before `InputInit()`, before MIDI, `SDL_INIT_AUDIO` is dropped, and there is no window so no keyboard focus. Player-driven racing is therefore not possible, and the only ways out of `roller-vr --vr` are `--vr-frames` or the runtime ending the session (which does not run teardown or the save path). The original goal ("actual drivable track", input handling reused unchanged) is not achieved by this design. Input and audio need their own design (Follow-up 1).
2. **Only part of the scene is head-tracked.** The per-eye MVP override reaches the main world pass and the cars. Still computed from the flat game camera: the GPU frustum cull (`scene_render_gpu_quad_frustum_culled`), building near-plane culling (`building.c`) and car visibility (`car.c`), the sky/ground split (`game_render.c`, computed in screen pixels), and the screen-space quads for particles and name tags (`project_to_ndc` in `game_render_hardware.c`). Consequence: turning the head well away from the car's heading can drop geometry that the flat camera culls, and the horizon/sky split stays fixed to the game view instead of following the head; Monado's small simulated sway does not show either effect (Follow-up 3).
3. Attract-mode only, HUD not rendered, player 1 only, `VR_METERS_TO_WORLD` placeholder, empty (`layerCount = 0`) frames whenever the frontend is on a non-race state -- see Caveats above.

## Follow-ups (priority order; Phase 3 and later)

1. **Input + audio in VR mode**, and a way to exit from inside the headset (Known gap 1). Plan-level gap; nothing else in this list matters for playing until it is solved.
2. **Runtime-initiated session end** (`XR_SESSION_STATE_STOPPING`/`EXITING`/`LOSS_PENDING`, and `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame` failures) makes `vr_run_frame()` return false without ever reaching `vr_shutdown()`/`ShutdownSDL()`: no teardown, no save. Must land before player racing.
3. **Head-tracked culling, sky/ground split and screen-space passes** (Known gap 2).
4. **SDL fork must let the caller enable the runtime-requested Vulkan instance/device extensions and a newer `apiVersion`** (the open item above; explains the 09120/09118 messages).
5. **`vr_init()` failure UX**: `ErrorBoxExit()` shows a modal box and exits with status 0 (invisible/blocking on headless hosts and the Windows GUI subsystem); should print to stderr and exit non-zero.
6. Minor: fence handoff should be an explicit flag instead of `!r->window`; per-frame `xrEnumerateSwapchainImages` (enumerate once per swapchain); ignored `vk*`/`xr*` return values; swapchain format/gamma (UNORM vs sRGB) check on a real headset; far plane 1e5 world units vs the game's 2e7; dead getters (`game_render_get_camera`/`get_projection`, `scene_render_gpu_get_camera`/`get_projection`, and the unused `g_szVRTrackPath` global); move the maths under test into a Vulkan-free file so `vr_mvp_math_test.c` tests the real function instead of a copy (only `vr_merge_view()` is tested for real today) and wire it into `zig build`; SDL fork hosting and a `.zon` pin (the Phase 1 blocker).

## Final review fix wave (2026-09-23)

Fixes made after the whole-branch review of Phase 2 (`.superpowers/sdd/2026-09-23-vr-phase2-stereo-camera/final-fix-report.md` has the per-finding evidence):

- SDL fork VkImage property kept current across cycling and defragmentation (critical: latent use-after-destroy).
- IPD no longer counted twice (plan error, see section 2).
- Tracking fallback: per-component merge, FOV always the runtime's, no zero-FOV/NaN MVP, 3DoF headsets render.
- `--vr`/`--vr-frames` exist only in `roller-vr`; `--vr` with `--snapshot`/`--snapshot-scene` is rejected.
- `UpdateSDL()`'s frame limiter and background FPS cap are skipped in VR mode (they slept inside the XR frame and fought `xrWaitFrame`).
- The "plain roller unchanged" claim was re-verified against the real phase base (Outcome above); documentation claims corrected (log-cited head-pose values, "slightly different viewpoints", which validated runs reached the post-demo stretch); stale comments and the OpenXR application name (`ROLLER VR`) fixed.
- Documented, not fixed: no input/audio (Known gap 1), partial head tracking (Known gap 2), and the Follow-ups above.
