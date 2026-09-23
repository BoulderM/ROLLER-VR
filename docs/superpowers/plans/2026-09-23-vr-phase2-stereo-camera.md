# ROLLER VR Phase 2: Stereo Cockpit Camera Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the real, live gameplay scene (an actual drivable track, car, and scenery) in stereo to both OpenXR eye swapchains on `fatal-gpu`, with the view driven by live head-pose tracking each frame — the first VR phase where real game content, not solid-color clears, reaches the HMD.

**Architecture:** A new `roller-vr` executable (renamed from Phase 1's `vr_skeleton`, still `-Dvr`-gated) links the real game sources (not the `roller-core.srclist` editor-library subset, which excludes the game entry point) and boots directly into `eFRONTEND_STATE_RACING` on a given track, reusing the exact CLI mechanism `--track-path`/`g_bSnapshotMode` already use for direct-race-start. A new `g_bVRMode` flag makes `InitSDL()` create a real `SDL_GPUDevice` with no window (mirroring Phase 1's own device creation), which makes `game_render_create()`'s existing `window == NULL` branch produce a windowless `GameRenderer` for free. Each tick, a new `frontend_run_game_loop()` VR branch brackets `main_loop_iteration()` with `xrWaitFrame`/`xrBeginFrame`/`xrLocateViews`/`xrEndFrame`; inside that tick, `updatescreen()`'s single `draw_road()` + `game_render_end_frame()` call is replaced (when `g_bVRMode`) with a new per-eye path that calls `draw_road()` twice (once per eye, so `calculateview()` runs fresh each time), overrides the resulting MVP with `build_mvp_vr_eye()` (base car-cockpit pose × live head-pose delta × per-eye IPD offset), and submits via a new windowless, no-CRT, no-readback `scene_render_gpu` end-frame variant that waits its GPU fence and `vkCmdCopyImage`s the resolved texture straight into that eye's OpenXR swapchain image — reusing Phase 1's already-validated Vulkan/OpenXR call sequence and clear→copy pattern.

**Tech Stack:** C, Zig 0.15.2, SDL3 (`Zizin13/SDL` fork), Vulkan 1.4, OpenXR 1.1, Monado (on `fatal-gpu`, 192.168.14.92).

**Spec:** `docs/superpowers/specs/2026-09-23-roller-vr-phase2-design.md` (this plan implements it; a mechanical refinement to that spec's Section 1/3 was discovered during planning and is documented in the note below — read both).

**Note on a spec refinement found while planning:** the approved spec described Phase 2 as creating two independent windowless `SceneRendererGPU` instances and driving them directly. Reading the actual per-frame call chain (`main_loop_iteration()` → `frontend_update()` → `race_draw()` → `updatescreen()` → `draw_road()` + `game_render_end_frame()`) showed that `draw_road()` itself is what computes the camera (`calculateview()`) and builds the frame's CPU-side draw queue into `GameRenderer`'s *own* internal scene instance — there's no separate place to feed prebuilt draw data into a second instance. The corrected, equivalent-intent design below calls `draw_road()` twice per frame (once per eye) against the *same* windowless `GameRenderer`, with the MVP overridden between calls. Same architecture (fence-wait + `vkCmdCopyImage`, live head tracking, separate `-Dvr` executable, no default-build impact) — only the "how many renderer instances / where the draw list comes from" mechanic changed. This is called out so review can specifically check that this refinement doesn't quietly break something the spec assumed.

## Global Constraints

- Default (`-Dvr` absent) `zig build` and all its outputs (`roller`, `test-snapshots`, `gpu-parity`) must be byte-for-byte unaffected — every new branch touching a shared file (`3d.c`, `roller.c`, `roller.h`, `game_render.c`, `game_render.h`, `scene_render_gpu.c`, `scene_render_gpu.h`) is gated behind the new `g_bVRMode` runtime flag or is a pure additive function nothing else calls.
- Reuse Phase 1's already-validated OpenXR call sequence verbatim where unchanged: `xrGetVulkanGraphicsDevice2KHR` before `xrCreateSession`, `xrCreateReferenceSpace` before the first `xrEndFrame`, the GPU-mismatch safety check comparing `xrGetVulkanGraphicsDevice2KHR`'s result against SDL_GPU's physical device. Don't regress these.
- Author every new OpenXR/Vulkan call against the spec's Valid Usage sections and the Khronos `hello_xr` sample directly, not from memory — both of Phase 1's hardware bugs came from skipping this.
- Development/validation runs on `fatal-gpu` go through `XR_APILAYER_LUNARG_core_validation` (`XR_ENABLE_API_LAYERS=XR_APILAYER_LUNARG_core_validation`).
- No physical HMD is attached to `fatal-gpu` — Monado falls back to its "Simulated HMD." Validation is exit-code/log/validation-layer correctness, never a human visual check.
- SDL fork changes go on `/root/sdl-vr-fork` branch `vr-gpu-properties` (still local-only — the "known limitation" about pushing it to a real remote is explicitly out of scope for this plan, tracked separately).

## Review Focus

- **Non-VR build regression**: any `g_bVRMode`-guarded change to `3d.c`/`roller.c`/`game_render.c`/`scene_render_gpu.c` must leave the default build's behavior identical — Task 9's final step re-runs `zig build test-snapshots` (no `-Dvr`) and diffs against the pre-change baseline captured in Task 1.
- **Tracking loss mid-frame**: if `xrLocateViews()` reports `viewStateFlags` missing `XR_VIEW_STATE_POSITION_VALID_BIT` or `XR_VIEW_STATE_ORIENTATION_VALID_BIT` for either eye, `build_mvp_vr_eye()` must not receive a garbage pose — Task 6 adds an explicit last-known-good fallback and a synthetic-input test of that branch.
- **`shouldRender == false` frames**: must not call `draw_road()`/acquire swapchain images, and must still submit a valid `layerCount = 0` `xrEndFrame` (Phase 1 already handles this in the old single-loop shape; Task 7 must preserve it in the new split acquire/render/release-inside-`vr_present_stereo_frame` shape).
- **GPU/device mismatch check surviving the refactor**: Phase 1's safety check (OpenXR-required physical device vs. SDL_GPU's) must still run against the *real* game's `SDL_GPUDevice` (via `ROLLERGetGPUDevice()`), not silently get skipped now that device creation moved into `InitSDL()`'s new VR branch — Task 2 keeps the check but Task 6 must call it against the right device handle.
- **SDL fork property accessor misuse**: the new texture-VkImage property must fail loudly (return `NULL`/log an error, not `0`/garbage) if called on a non-Vulkan backend or a texture that isn't yet created — Task 1's manual verification step exercises this.

---

### Task 1: SDL fork — expose a texture's `VkImage` via a new property

**Files:**
- Modify (on `/root/sdl-vr-fork`, branch `vr-gpu-properties`): `include/SDL3/SDL_gpu.h`, `src/gpu/vulkan/SDL_gpu_vulkan.c`
- Modify (ROLLER repo, baseline capture): none yet — this task's own verification is a local build against the patched fork

**Interfaces:**
- Produces: `SDL_PROP_GPU_TEXTURE_VULKAN_IMAGE_POINTER` (new string constant) and `SDL_GetGPUTextureProperties(SDL_GPUTexture *texture)` returning an `SDL_PropertiesID` whose `SDL_PROP_GPU_TEXTURE_VULKAN_IMAGE_POINTER` property holds the texture's `VkImage`, `(void*)0` if unavailable.

- [ ] **Step 1: Capture the pre-change non-VR baseline**

```bash
cd /config/projects/fatal/ROLLER
zig build test-snapshots 2>&1 | tee /tmp/vr-phase2-baseline-snapshots.log
```
Save `/tmp/vr-phase2-baseline-snapshots.log` — Task 9's final step diffs against this.

- [ ] **Step 2: Add the property constant**

In `/root/sdl-vr-fork/include/SDL3/SDL_gpu.h`, find where `SDL_PROP_GPU_DEVICE_VULKAN_QUEUE_POINTER` (or the nearest existing `SDL_PROP_GPU_DEVICE_VULKAN_*` constant Phase 1 added) is declared, and add immediately after it:

```c
/**
 * Vulkan-backend-only: the underlying VkImage for an SDL_GPUTexture, as a
 * (void*)VkImage. NULL if the texture is not backed by Vulkan or the handle
 * is otherwise unavailable.
 */
#define SDL_PROP_GPU_TEXTURE_VULKAN_IMAGE_POINTER "SDL.gpu.texture.vulkan.image"

extern SDL_DECLSPEC SDL_PropertiesID SDLCALL SDL_GetGPUTextureProperties(SDL_GPUTexture *texture);
```

- [ ] **Step 3: Implement it in the Vulkan backend**

In `/root/sdl-vr-fork/src/gpu/vulkan/SDL_gpu_vulkan.c`, find the existing `VULKAN_INTERNAL_...DeviceProperties`-style function Phase 1 added for `SDL_GetGPUDeviceProperties` and add a sibling for textures. The Vulkan backend's texture struct (`VulkanTexture`, holding `.image`) is the same one `VULKAN_CreateTexture`/`VULKAN_ReleaseTexture` already use — locate it via `grep -n "VulkanTexture {" src/gpu/vulkan/SDL_gpu_vulkan.c` and confirm the field name before writing this (it may be `image` or `texture->image` through a wrapper struct):

```c
static SDL_PropertiesID VULKAN_GetGPUTextureProperties(SDL_GPUTexture *texture)
{
    VulkanTexture *vulkanTexture = (VulkanTexture *)texture;
    SDL_PropertiesID props = SDL_CreateProperties();
    if (props) {
        SDL_SetPointerProperty(props, SDL_PROP_GPU_TEXTURE_VULKAN_IMAGE_POINTER,
                                (void *)vulkanTexture->image);
    }
    return props;
}
```

Wire it into the backend's function table (the same struct that has `.GetGPUDeviceProperties = VULKAN_GetGPUDeviceProperties` from Phase 1 — add `.GetGPUTextureProperties = VULKAN_GetGPUTextureProperties` alongside it) and add the public `SDL_GetGPUTextureProperties()` dispatcher in `src/gpu/SDL_gpu.c` next to the existing `SDL_GetGPUDeviceProperties()` dispatcher, following that function's exact shape (device-agnostic driver lookup, NULL-texture guard, returns `0` and logs `SDL_InvalidParamError` on a NULL/non-Vulkan input — mirror `SDL_GetGPUDeviceProperties`'s own guard exactly so the "fail loudly, not silently" behavior matches Phase 1's established pattern for this property surface).

- [ ] **Step 4: Verify it compiles standalone**

```bash
cd /root/sdl-vr-fork
git add -A && git status   # confirm only the two files above changed
```
(No SDL-level test suite is being added here — Phase 1 didn't add one for the device-properties addition either, and this is a single accessor with no branching logic beyond the existing dispatcher pattern it mirrors. Correctness is verified at integration time in Task 6, where a real call to it against a real texture either returns a usable `VkImage` or the loud failure path fires.)

- [ ] **Step 5: Commit on the fork**

```bash
cd /root/sdl-vr-fork
git add include/SDL3/SDL_gpu.h src/gpu/vulkan/SDL_gpu_vulkan.c src/gpu/SDL_gpu.c
git commit -m "gpu: add SDL_GetGPUTextureProperties for Vulkan image interop"
```

---

### Task 2: VR mode entry point — windowless device + direct race start

**Files:**
- Modify: `PROJECTS/ROLLER/roller.h`, `PROJECTS/ROLLER/roller.c`, `PROJECTS/ROLLER/3d.c`
- Test: manual run (see Step 5) — this codebase has no unit-test harness for CLI/init wiring; `--snapshot`/`--gpu-parity` establish the same manual-verification convention this task follows.

**Interfaces:**
- Consumes: `s_pGPUDevice`/`s_pWindow` (existing `roller.c` statics), `g_bSnapshotMode`/`g_szDirectTrackPath` (existing globals, `3d.c`), `race_set_track()`, `frontend_run_game_loop()`, `eFRONTEND_STATE_RACING` (existing, `3d.c`/`frontend.c`).
- Produces: `extern int g_bVRMode;` (declared `roller.h`, defined `roller.c`) and `extern char g_szVRTrackPath[260];` (declared `roller.h`, defined `roller.c`) — consumed by Task 7/9.

- [ ] **Step 1: Add the VR-mode global**

In `PROJECTS/ROLLER/roller.h`, near the existing `SDL_GPUDevice *ROLLERGetGPUDevice(void);` declaration (line 73), add:

```c
extern int g_bVRMode;
extern char g_szVRTrackPath[260];
```

In `PROJECTS/ROLLER/roller.c`, near the top with the other globals (e.g. near `s_pGPUDevice`, line ~111), add:

```c
int g_bVRMode = 0;
char g_szVRTrackPath[260] = { 0 };
```

- [ ] **Step 2: Branch `InitSDL()` for VR — windowless device, no audio/MIDI**

In `PROJECTS/ROLLER/roller.c`, `InitSDL()`, immediately after the existing `if (g_bSnapshotMode) { ... return 0; }` block (after line 818), add:

```c
  if (g_bVRMode) {
    /* Windowless like snapshot mode, but (unlike snapshot mode) we need a
     * real SDL_GPU device — OpenXR binds to it directly, matching the Phase 1
     * walking-skeleton's own SDL_CreateGPUDevice call exactly. */
    s_pGPUDevice = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
    if (!s_pGPUDevice) {
      ErrorBoxExit("Couldn't create GPU device: %s", SDL_GetError());
      return 1;
    }
    SDL_Log("GPU driver (VR mode): %s", SDL_GetGPUDeviceDriver(s_pGPUDevice));
    return 0;
  }
```

`ROLLERGetWindow()` continues to just `return s_pWindow;`, which stays `NULL` in VR mode since `SDL_CreateWindow` is never reached — this is what makes `game_render_create(ROLLERGetGPUDevice(), ROLLERGetWindow())` at `3d.c:3768` produce a windowless `GameRenderer` automatically, with zero changes to `game_render_create()` itself.

- [ ] **Step 3: Add the `--vr` CLI flag and reuse `--track-path` for direct race start**

In `PROJECTS/ROLLER/3d.c`'s argument loop (the same `for (int i = 1; i < argc;)` loop `--track-path` is already parsed in, near line 2963), add a new branch:

```c
    } else if (strcmp(argv[i], "--vr") == 0) {
      g_bVRMode = 1;
      consumed = 1;
```

Find the existing `--track-path` branch (line 2963) and read it to confirm exactly how it populates `g_szDirectTrackPath` (or equivalent — the exact variable name must be read from that branch, not assumed) before wiring Step 4 below, since `race_set_track(TrackLoad)` (line 3258) needs `TrackLoad` populated from whatever that flag sets. Also add, next to the existing `if (g_bSnapshotMode || g_szDirectTrackPath) { cli_fprintf(stderr, "ERROR: '--gpu-parity' cannot be combined with snapshot or track-path mode\n"); ... }` mutual-exclusivity check (line 3028), a matching guard so `--gpu-parity` and `--vr` can't be combined either — copy that check's exact shape with `g_bVRMode` added to the condition.

- [ ] **Step 4: Extend the direct-race-start condition to include VR mode**

At line 3259, change:

```c
  frontend_run_game_loop(g_bSnapshotMode ? eFRONTEND_STATE_RACING : eFRONTEND_STATE_COPYRIGHT);
```

to:

```c
  frontend_run_game_loop((g_bSnapshotMode || g_bVRMode) ? eFRONTEND_STATE_RACING : eFRONTEND_STATE_COPYRIGHT);
```

Also extend the `if (!g_bSnapshotMode) { InitFATDATA(...); InitREPLAYS(...); InitTRACKS(...); ROLLERGetAudioInfo(); }` block (line 3102) — VR mode still needs `InitFATDATA`/`InitTRACKS` (it's loading a real track, unlike snapshot's fixed-replay/scene modes) but should skip the interactive CD-image prompt and audio probe the same way snapshot mode does for headless operation. Change the condition so `InitFATDATA`/`InitTRACKS` still run for VR mode but `ROLLERGetAudioInfo()` — the audio-device probe — does not:

```c
  if (!g_bSnapshotMode) {
#if !defined(IS_ANDROID)
    InitFATDATA(whiplash_root);
#endif
    InitREPLAYS(whiplash_root);
    InitTRACKS(whiplash_root);
    if (!g_bVRMode)
      ROLLERGetAudioInfo();
  }
```

(`SDL_INIT_AUDIO` is already skipped for VR mode in Step 2's `InitSDL` branch, matching snapshot mode's `if (!g_bSnapshotMode) uiSdlInitFlags |= SDL_INIT_AUDIO;` at line 773 only if `g_bVRMode` is added there too — add `if (!g_bSnapshotMode && !g_bVRMode) uiSdlInitFlags |= SDL_INIT_AUDIO;` at that line.)

- [ ] **Step 5: Manual verification — boots to racing with no window, no OpenXR yet**

This step confirms Task 2 in isolation, before any OpenXR code exists (Task 6/7). Temporarily build the *existing* `roller` (non-VR) target with a throwaway debug flag is unnecessary — instead verify via the normal executable once `--vr` is wired, deferring the actual run to `fatal-gpu` in Task 9 where the full chain (including Task 6/7's OpenXR code) exists. Mark this step's verification as folded into Task 9 Step 1 (first real run) rather than duplicating a partial run here — note this explicitly in the commit message so reviewers know Step 5 is deliberately deferred, not skipped.

- [ ] **Step 6: Commit**

```bash
cd /config/projects/fatal/ROLLER
git add PROJECTS/ROLLER/roller.h PROJECTS/ROLLER/roller.c PROJECTS/ROLLER/3d.c
git commit -m "vr: add g_bVRMode entry point (windowless device, direct race start)"
```

---

### Task 3: `scene_render_gpu.c`/`.h` — VR-eye MVP override + windowless no-readback end-frame

**Files:**
- Modify: `PROJECTS/ROLLER/scene_render_gpu.c`, `PROJECTS/ROLLER/scene_render_gpu.h`
- Test: covered by Task 9's `zig build test-snapshots` regression re-run (Global Constraints) — these are pure additions plus one conditional inside `build_mvp`'s wrapper, gated by whether the override was ever set (default: never, for the non-VR path)

**Interfaces:**
- Consumes: existing `SceneRendererGPU`, `SDL_GPUTexture`, `build_mvp()` (`scene_render_gpu.c:809`), `scene_render_gpu_build_vp()` (`:850`), `scene_render_gpu_end_frame()`/`scene_render_gpu_end_frame_internal()` (`:3691`/`:2900`, read verbatim during planning — see Step 3).
- Produces: `void scene_render_gpu_set_vr_eye_mvp(SceneRendererGPU *r, const float mvp[16]);`, `SDL_GPUTexture *scene_render_gpu_end_frame_to_vr_eye(SceneRendererGPU *r, Uint32 uiWidth, Uint32 uiHeight);`, `SDL_GPUFence *scene_render_gpu_get_last_fence(SceneRendererGPU *r);` — consumed by Task 4/7.

- [ ] **Step 1: Add the MVP override field and setter**

In `scene_render_gpu.h`, near `scene_render_gpu_set_projection_reference_height`'s declaration, add:

```c
/* Overrides build_mvp()'s computed matrix for the next end-frame call only
 * (auto-clears after use). Used by VR to substitute a head-tracked, eye-
 * offset MVP in place of the normal single camera. NULL/never-called means
 * normal (non-VR) behavior is completely unaffected. */
void scene_render_gpu_set_vr_eye_mvp(SceneRendererGPU *r, const float mvp[16]);
```

In `scene_render_gpu.c`, find the `struct SceneRendererGPU { ... }` definition (search `grep -n "^struct SceneRendererGPU"`) and add two fields:

```c
    bool  vrEyeMvpOverrideSet;
    float vrEyeMvpOverride[16];
```

Implement the setter near `scene_render_gpu_set_projection_reference_height` (line 3747):

```c
void scene_render_gpu_set_vr_eye_mvp(SceneRendererGPU *r, const float mvp[16])
{
    if (!r) return;
    memcpy(r->vrEyeMvpOverride, mvp, sizeof(r->vrEyeMvpOverride));
    r->vrEyeMvpOverrideSet = true;
}
```

- [ ] **Step 2: Consume the override in `scene_render_gpu_build_vp()`**

At line 850, change:

```c
void scene_render_gpu_build_vp(const SceneRendererGPU *r, float vp[16])
{
    build_mvp(vp, &r->camera, &r->proj, r->viewportW, r->viewportH,
              r->fovMultiplier, effective_screen_scale(r, r->viewportH));
}
```

to:

```c
void scene_render_gpu_build_vp(SceneRendererGPU *r, float vp[16])
{
    if (r->vrEyeMvpOverrideSet) {
        memcpy(vp, r->vrEyeMvpOverride, sizeof(r->vrEyeMvpOverride));
        r->vrEyeMvpOverrideSet = false; /* one-shot, like the real spec intended */
        return;
    }
    build_mvp(vp, &r->camera, &r->proj, r->viewportW, r->viewportH,
              r->fovMultiplier, effective_screen_scale(r, r->viewportH));
}
```

(Dropping `const` on the parameter is required since this now mutates `r->vrEyeMvpOverrideSet`; update the header declaration to match and grep all call sites — `scene_render_gpu_end_frame_internal()`'s call at line 3271 — to confirm none pass a `const SceneRendererGPU*` that would now fail to compile. If one does, that call site already has a non-const `r` in scope there since `end_frame_internal` mutates other fields on `r` too — confirm by reading the function's own parameter type before assuming this compiles.)

- [ ] **Step 3: Add the windowless, no-CRT, no-readback end-frame variant**

Reading `scene_render_gpu_end_frame_internal()` (`scene_render_gpu.c:2900-3689`) verbatim shows this is smaller than it looks: `bool bReadback = pbyPixels != NULL;` (line 2954) already gates *every* readback-only step (the `SDL_DownloadFromGPUTexture` block, line 3528) and every window-only step (`if (!bReadback && r->window)`, lines 3553/3564/3599) independently. So calling the *existing* public `scene_render_gpu_end_frame(r)` (which is just `scene_render_gpu_end_frame_internal(r, NULL, 0, 0, 0, 0)`, i.e. `bReadback = false`) against a renderer whose `r->window` is already `NULL` **already does exactly what VR needs** for every pass up through the resolve — it runs the 3D scene/signs/HUD passes into `r->offscreenTex` (confirmed at line 3173: `SDL_GPUTexture *resolveTarget = r->offscreenTex;`, used unconditionally, not just under readback), and skips the download and all window/CRT/blit/debug-overlay code automatically. The only two gaps: (1) the non-readback submission branch (line 3618-3619) calls plain `SDL_SubmitGPUCommandBuffer` with no fence, and VR needs a fence to wait on before its `vkCmdCopyImage`; (2) there's no public getter for `r->offscreenTex`.

Fix both with small, additive changes. First, add a field to `struct SceneRendererGPU` (find its definition via `grep -n "^struct SceneRendererGPU"`) next to the other GPU-resource fields:

```c
    SDL_GPUFence *lastVrFence; /* set only for windowless, non-readback end_frame calls; one-shot, cleared by scene_render_gpu_get_last_fence */
```

Then change the submission block at lines 3609-3644 from:

```c
    bool bFrameOk = true;
    SDL_GPUFence *pFence = NULL;
    if (bReadback) {
        pFence = SDL_SubmitGPUCommandBufferAndAcquireFence(r->cmdBuf);
        bFrameOk = pFence != NULL;
        if (!pFence) {
            SDL_Log("scene_render_gpu: readback command submission failed: %s",
                    SDL_GetError());
        }
    } else {
        bFrameOk = SDL_SubmitGPUCommandBuffer(r->cmdBuf);
    }
    r->cmdBuf = NULL;

    if (pFence) {
        SDL_GPUFence *apFences[1] = { pFence };
        if (!SDL_WaitForGPUFences(r->device, true, apFences, 1)) {
            SDL_Log("scene_render_gpu: readback fence wait failed: %s",
                    SDL_GetError());
            bFrameOk = false;
        } else {
            const uint8 *pbyMapped = SDL_MapGPUTransferBuffer(r->device,
                                                               r->readbackXfer,
                                                               false);
            if (!pbyMapped) {
                SDL_Log("scene_render_gpu: readback transfer map failed: %s",
                        SDL_GetError());
                bFrameOk = false;
            } else {
                copy_readback_to_rgba(pbyPixels, uiRowPitch, pbyMapped,
                                      renderW, renderH, r->colorTargetFormat);
                SDL_UnmapGPUTransferBuffer(r->device, r->readbackXfer);
            }
        }
        SDL_ReleaseGPUFence(r->device, pFence);
    }
```

to:

```c
    bool bWantFence = bReadback || !r->window;
    bool bFrameOk = true;
    SDL_GPUFence *pFence = NULL;
    if (bWantFence) {
        pFence = SDL_SubmitGPUCommandBufferAndAcquireFence(r->cmdBuf);
        bFrameOk = pFence != NULL;
        if (!pFence) {
            SDL_Log("scene_render_gpu: command submission failed: %s",
                    SDL_GetError());
        }
    } else {
        bFrameOk = SDL_SubmitGPUCommandBuffer(r->cmdBuf);
    }
    r->cmdBuf = NULL;

    if (pFence && bReadback) {
        SDL_GPUFence *apFences[1] = { pFence };
        if (!SDL_WaitForGPUFences(r->device, true, apFences, 1)) {
            SDL_Log("scene_render_gpu: readback fence wait failed: %s",
                    SDL_GetError());
            bFrameOk = false;
        } else {
            const uint8 *pbyMapped = SDL_MapGPUTransferBuffer(r->device,
                                                               r->readbackXfer,
                                                               false);
            if (!pbyMapped) {
                SDL_Log("scene_render_gpu: readback transfer map failed: %s",
                        SDL_GetError());
                bFrameOk = false;
            } else {
                copy_readback_to_rgba(pbyPixels, uiRowPitch, pbyMapped,
                                      renderW, renderH, r->colorTargetFormat);
                SDL_UnmapGPUTransferBuffer(r->device, r->readbackXfer);
            }
        }
        SDL_ReleaseGPUFence(r->device, pFence);
    } else if (pFence) {
        /* Windowless non-readback (VR eye render): hand the fence to the
         * caller instead of waiting/releasing it here -- vr_present_stereo_frame
         * (Task 7) waits on it right before the vkCmdCopyImage that needs
         * this render's output, then releases it via the getter below. */
        r->lastVrFence = pFence;
    }
```

Then add three small public functions near the existing wrappers (line 3691):

```c
SDL_GPUTexture *scene_render_gpu_get_offscreen_texture(SceneRendererGPU *r)
{
    return r ? r->offscreenTex : NULL;
}

SDL_GPUFence *scene_render_gpu_get_last_fence(SceneRendererGPU *r)
{
    if (!r) return NULL;
    SDL_GPUFence *f = r->lastVrFence;
    r->lastVrFence = NULL; /* one-shot, mirrors the VR-eye MVP override's one-shot pattern from Step 2 */
    return f;
}

SDL_GPUTexture *scene_render_gpu_end_frame_to_vr_eye(SceneRendererGPU *r,
                                                      Uint32 uiWidth,
                                                      Uint32 uiHeight)
{
    if (!r || r->window) return NULL; /* VR eyes are windowless renderers only */
    r->viewportW = (int)uiWidth;
    r->viewportH = (int)uiHeight;
    if (!scene_render_gpu_end_frame(r)) return NULL;
    return scene_render_gpu_get_offscreen_texture(r);
}
```

(`r->renderScale`'s existing fallback — `rs = r->renderScale > 0.25f ? r->renderScale : 1.0f;`, line 2972 — already makes `renderW/renderH` come out to exactly `viewportW/viewportH` when `renderScale` is left at its zero-initialized default, so the eye texture's size matches `uiWidth`/`uiHeight` exactly with no extra field to set.)

Declare all three in `scene_render_gpu.h` next to `scene_render_gpu_set_vr_eye_mvp`.

- [ ] **Step 4: Compile-check**

```bash
cd /config/projects/fatal/ROLLER
zig build -Dvr 2>&1 | tail -60
```
Expected: fails only on missing symbols from Task 4 onward (`game_render_*` wrappers not yet added) or succeeds if this task is built in isolation via a temporary throwaway call site — either way, confirm no compile errors originating from `scene_render_gpu.c`/`.h` themselves.

- [ ] **Step 5: Commit**

```bash
cd /config/projects/fatal/ROLLER
git add PROJECTS/ROLLER/scene_render_gpu.c PROJECTS/ROLLER/scene_render_gpu.h
git commit -m "vr: add VR-eye MVP override and windowless no-readback end-frame to scene_render_gpu"
```

---

### Task 4: `game_render.c`/`.h` — thread the VR entry points through `GameRenderer`

**Files:**
- Modify: `PROJECTS/ROLLER/game_render.c`, `PROJECTS/ROLLER/game_render.h`

**Interfaces:**
- Consumes: `scene_render_gpu_set_vr_eye_mvp()`, `scene_render_gpu_end_frame_to_vr_eye()`, `scene_render_gpu_get_last_fence()` (Task 3), `GameRenderer.gpu` (existing cached `SceneRendererGPU*` field, `game_render.c:24`).
- Produces: `void game_render_set_vr_eye_mvp(GameRenderer *renderer, const float mvp[16]);`, `SDL_GPUTexture *game_render_end_frame_to_vr_eye(GameRenderer *renderer, Uint32 uiWidth, Uint32 uiHeight);`, `SDL_GPUFence *game_render_get_last_vr_fence(GameRenderer *renderer);` — consumed by Task 7.

- [ ] **Step 1: Add the thin wrappers**

In `game_render.h`, next to the existing `void game_render_end_frame(GameRenderer *renderer);` (line 69), add:

```c
void game_render_set_vr_eye_mvp(GameRenderer *renderer, const float mvp[16]);
SDL_GPUTexture *game_render_end_frame_to_vr_eye(GameRenderer *renderer, Uint32 uiWidth, Uint32 uiHeight);
SDL_GPUFence *game_render_get_last_vr_fence(GameRenderer *renderer);
```

In `game_render.c`, next to `game_render_end_frame`'s existing implementation (search `grep -n "^void game_render_end_frame"`), add:

```c
void game_render_set_vr_eye_mvp(GameRenderer *renderer, const float mvp[16]) {
    if (!renderer || !renderer->gpu) return;
    scene_render_gpu_set_vr_eye_mvp(renderer->gpu, mvp);
}

SDL_GPUTexture *game_render_end_frame_to_vr_eye(GameRenderer *renderer, Uint32 uiWidth, Uint32 uiHeight) {
    if (!renderer || !renderer->gpu) return NULL;
    return scene_render_gpu_end_frame_to_vr_eye(renderer->gpu, uiWidth, uiHeight);
}

SDL_GPUFence *game_render_get_last_vr_fence(GameRenderer *renderer) {
    if (!renderer || !renderer->gpu) return NULL;
    return scene_render_gpu_get_last_fence(renderer->gpu);
}
```

- [ ] **Step 2: Compile-check**

```bash
cd /config/projects/fatal/ROLLER
zig build -Dvr 2>&1 | tail -60
```

- [ ] **Step 3: Commit**

```bash
cd /config/projects/fatal/ROLLER
git add PROJECTS/ROLLER/game_render.c PROJECTS/ROLLER/game_render.h
git commit -m "vr: expose VR-eye MVP override and end-frame through GameRenderer"
```

---

### Task 5: `vr_runtime.c` — rename from `vr_skeleton_main.c`, OpenXR setup stays, `main()` becomes `vr_init()`

**Files:**
- Rename: `PROJECTS/ROLLER/vr_skeleton_main.c` → `PROJECTS/ROLLER/vr_runtime.c`
- Create: `PROJECTS/ROLLER/vr_runtime.h`

**Interfaces:**
- Produces: `bool vr_init(void);` (renamed from `main()`'s setup portion, steps 1–8 of the original file, everything through creating the reference space and command pool — Task 5 scope ends here), `void vr_shutdown(void);` (the original file's step 9 cleanup, minus the `frames_rendered == 0` check which no longer applies).

- [ ] **Step 1: Rename the file and restructure `main()`'s body into two functions**

```bash
cd /config/projects/fatal/ROLLER
git mv PROJECTS/ROLLER/vr_skeleton_main.c PROJECTS/ROLLER/vr_runtime.c
```

Edit `vr_runtime.c`: replace the file's top comment (lines 1–7) with:

```c
/* PROJECTS/ROLLER/vr_runtime.c
 *
 * VR Phase 2: opens the OpenXR session against SDL_GPU's Vulkan device
 * (unchanged from Phase 1) and drives the real game loop in stereo. vr_init()
 * is Phase 1's main()'s setup portion (steps 1-8: device, instance, system,
 * session, swapchains, reference space, command pool). Phase 1's own
 * 120-frame clear-color loop and shutdown are replaced by vr_run_frame()/
 * vr_present_stereo_frame() (this task's sibling tasks) and vr_shutdown().
 */
```

Move module-level state that was previously local to `main()` (lines 39–251 of the original: `gpu`, `vk_instance`, `vk_physical_device`, `vk_device`, `vk_queue`, `vk_queue_family_index`, `instance`, `system_id`, `session`, `view_count`, `view_configs`, `swapchains`, `local_space`, `command_pool`, `command_buffer`) to file-static globals (`static SDL_GPUDevice *s_gpu;` etc.), since Task 6/7's per-frame functions need to reach them without threading a giant struct through every call — this matches the codebase's own convention of file-static renderer state (e.g. `roller.c`'s `s_pWindow`/`s_pGPUDevice`).

Rename `int main(int argc, char **argv)` to `bool vr_init(void)`, keeping its body (the original lines 44–251, SDL_GPU device creation through reference-space/command-pool creation) unchanged except: assign to the new statics instead of locals, and `return true;` at the end instead of falling through into the frame loop (the frame loop moves to Task 6/7). Every `fail(...)`/`fail_xr(...)`/`fail_vk(...)` call in this portion changes from `exit(1)` to `return false;` (edit `fail`/`fail_xr`/`fail_vk` themselves to `return false` instead of calling `exit(1)`, and add `return false;` after each call site that doesn't already `return`).

Delete the original file's steps 8–9 (the `while (!should_quit...)` loop and shutdown) from `vr_init()` — they move to Task 6/7 (`vr_run_frame()`) and this task's Step 2 (`vr_shutdown()`) respectively.

Create `vr_runtime.h`:

```c
#ifndef VR_RUNTIME_H
#define VR_RUNTIME_H

#include <stdbool.h>

bool vr_init(void);
void vr_shutdown(void);

#endif /* VR_RUNTIME_H */
```

- [ ] **Step 2: `vr_shutdown()`**

Add to `vr_runtime.c`, using the original file's lines 442–457 (`vkDeviceWaitIdle` through `SDL_Quit`) unchanged except operating on the now-static variables:

```c
void vr_shutdown(void)
{
    vkDeviceWaitIdle(s_vk_device);
    vkFreeCommandBuffers(s_vk_device, s_command_pool, 1, &s_command_buffer);
    vkDestroyCommandPool(s_vk_device, s_command_pool, NULL);
    for (Uint32 i = 0; i < s_view_count; i++) {
        xrDestroySwapchain(s_swapchains[i]);
    }
    free(s_swapchains);
    free(s_view_configs);
    xrDestroySpace(s_local_space);
    if (s_session_running) {
        xrEndSession(s_session);
    }
    xrDestroySession(s_session);
    xrDestroyInstance(s_instance);
    SDL_DestroyGPUDevice(s_gpu);
    SDL_Quit();
}
```

(`s_session_running` also becomes a file-static, set by `vr_init()` to `false` initially — Task 6 sets it `true` once the session reaches `XR_SESSION_STATE_READY`, matching the original file's `session_running` local.)

- [ ] **Step 3: Compile-check in isolation**

`vr_runtime.c` isn't wired into `build.zig` under its new name/role yet (Task 8 does that) — confirm it at least parses correctly with a standalone compiler invocation matching Task 8's eventual flags:

```bash
cd /config/projects/fatal/ROLLER
clang -fsyntax-only -I /root/sdl-vr-fork/include -I /usr/include/openxr -I /usr/include/vulkan \
  PROJECTS/ROLLER/vr_runtime.c
```

- [ ] **Step 4: Commit**

```bash
cd /config/projects/fatal/ROLLER
git add PROJECTS/ROLLER/vr_runtime.c PROJECTS/ROLLER/vr_runtime.h
git rm PROJECTS/ROLLER/vr_skeleton_main.c 2>/dev/null || true
git commit -m "vr: rename vr_skeleton_main.c to vr_runtime.c, split main() into vr_init()/vr_shutdown()"
```

---

### Task 6: `vr_runtime.c` — live head tracking and `build_mvp_vr_eye()`

**Files:**
- Modify: `PROJECTS/ROLLER/vr_runtime.c`, `PROJECTS/ROLLER/vr_runtime.h`

**Interfaces:**
- Consumes: `SceneRenderCamera`, `SceneRenderProjection` (`scene_render_types.h`: `{viewX,viewY,viewZ,cosYaw,sinYaw,fovScale,renderChunkIdx}` / `{view[3][3],screenScale,centerX,centerY,texHalfRes}`), `game_render_set_vr_eye_mvp()` (Task 4).
- Produces: `void build_mvp_vr_eye(float mvp_out[16], const SceneRenderCamera *base_cam, const SceneRenderProjection *base_proj, const XrView *xr_view, float eye_offset_m, int vpW, int vpH);`, `bool vr_locate_views(XrTime predicted_display_time, XrView out_views[2]);` — both consumed by Task 7.

- [ ] **Step 1: Add live view-locating with the fallback for tracking loss**

```c
static XrView s_last_good_views[2];
static bool   s_have_last_good_views = false;

bool vr_locate_views(XrTime predicted_display_time, XrView out_views[2])
{
    XrViewLocateInfo locate_info = {
        .type = XR_TYPE_VIEW_LOCATE_INFO,
        .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        .displayTime = predicted_display_time,
        .space = s_local_space,
    };
    XrViewState view_state = { .type = XR_TYPE_VIEW_STATE };
    XrView views[2] = { { .type = XR_TYPE_VIEW }, { .type = XR_TYPE_VIEW } };
    Uint32 out_count = 0;
    XrResult xr_result = xrLocateViews(s_session, &locate_info, &view_state, 2, &out_count, views);
    if (XR_FAILED(xr_result) || out_count < 2) {
        fprintf(stderr, "vr_runtime: xrLocateViews failed or returned <2 views (XrResult=%d, count=%u)\n",
                (int)xr_result, out_count);
        if (!s_have_last_good_views) return false;
        memcpy(out_views, s_last_good_views, sizeof(s_last_good_views));
        return true;
    }

    bool position_valid = (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    bool orientation_valid = (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    if (!position_valid || !orientation_valid) {
        if (!s_have_last_good_views) {
            /* No prior good pose to fall back to yet (e.g. very first frame) --
             * use identity, matching Phase 1's original placeholder pose,
             * rather than feeding an explicitly-invalid pose downstream. */
            for (int i = 0; i < 2; i++) {
                out_views[i] = (XrView){ .type = XR_TYPE_VIEW, .pose = { .orientation = { .w = 1.0f } } };
            }
            return true;
        }
        memcpy(out_views, s_last_good_views, sizeof(s_last_good_views));
        return true;
    }

    memcpy(out_views, views, sizeof(views));
    memcpy(s_last_good_views, views, sizeof(views));
    s_have_last_good_views = true;
    return true;
}
```

- [ ] **Step 2: `build_mvp_vr_eye()`**

This composes the base cockpit pose (from `calculateview()`, unchanged) with the head-pose delta and eye offset, then feeds the standard perspective construction `build_mvp()` already uses in `scene_render_gpu.c:809`. Rather than re-deriving that matrix math independently (risking a subtle mismatch with the existing renderer's conventions — column-major, the documented "don't pre-negate Y" shadercross rule, the horizon/center-correction term), this function builds an adjusted `SceneRenderCamera`/`SceneRenderProjection` pair and calls the *existing* `scene_render_gpu_build_vp()` machinery indirectly: since that function now consults the one-shot override (Task 3 Step 2) instead, `build_mvp_vr_eye()`'s real job is producing the override matrix directly. Implement it as a genuinely new perspective build (OpenXR views use asymmetric FOV via `xr_view->fov.angleLeft/Right/Up/Down`, which `build_mvp()`'s symmetric-FOV path can't express, so this cannot simply delegate to `build_mvp()`):

```c
#include <math.h>

static void quat_to_mat3(const XrQuaternionf *q, float m[3][3])
{
    float x = q->x, y = q->y, z = q->z, w = q->w;
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] =     2 * (x * y - z * w); m[0][2] =     2 * (x * z + y * w);
    m[1][0] =     2 * (x * y + z * w); m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] =     2 * (y * z - x * w);
    m[2][0] =     2 * (x * z - y * w); m[2][1] =     2 * (y * z + x * w); m[2][2] = 1 - 2 * (x * x + y * y);
}

void build_mvp_vr_eye(float mvp_out[16],
                       const SceneRenderCamera *base_cam,
                       const SceneRenderProjection *base_proj,
                       const XrView *xr_view,
                       float eye_offset_m,
                       int vpW, int vpH)
{
    /* Base cockpit rotation (world -> car-follow-camera space), unchanged
     * from the existing non-VR camera -- base_proj->view is exactly the
     * vk1..vk9 basis calculateview()/calculatetransform() already computed
     * this frame. */
    float base_rot[3][3];
    memcpy(base_rot, base_proj->view, sizeof(base_rot));

    /* Head-pose rotation from the OpenXR runtime, applied on top of the base
     * cockpit orientation (not replacing it) -- this is what makes the view
     * actually turn with the player's head while still being anchored to the
     * car's cockpit, not free-floating in world space. */
    float head_rot[3][3];
    quat_to_mat3(&xr_view->pose.orientation, head_rot);

    float combined_rot[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            float sum = 0.0f;
            for (int k = 0; k < 3; k++) sum += head_rot[r][k] * base_rot[k][c];
            combined_rot[r][c] = sum;
        }

    /* Eye offset along the head's local right axis (row 0 of head_rot),
     * plus the head's own tracked position delta and the OpenXR view's own
     * per-eye pose position (both already relative to s_local_space's
     * origin, which Phase 1 pinned at the identity/LOCAL reference pose). */
    float eye_x = base_cam->viewX + xr_view->pose.position.x + head_rot[0][0] * eye_offset_m;
    float eye_y = base_cam->viewY + xr_view->pose.position.y + head_rot[0][1] * eye_offset_m;
    float eye_z = base_cam->viewZ + xr_view->pose.position.z + head_rot[0][2] * eye_offset_m;

    /* Asymmetric perspective from the OpenXR-reported per-eye FOV -- this is
     * the one place VR genuinely needs a different projection construction
     * than build_mvp()'s symmetric FOV, since HMD lenses are rarely centered. */
    float tan_left = tanf(xr_view->fov.angleLeft);
    float tan_right = tanf(xr_view->fov.angleRight);
    float tan_up = tanf(xr_view->fov.angleUp);
    float tan_down = tanf(xr_view->fov.angleDown);
    float near_plane = 0.05f;  /* 5cm -- tighter than SCENE_GPU_NEAR's 10.0f world-unit
                                 default, revisited in Phase 3 comfort tuning */
    float far_plane = 100000.0f;

    float mvp[16] = {0};
    mvp[0] = 2.0f / (tan_right - tan_left);
    mvp[8] = (tan_right + tan_left) / (tan_right - tan_left);
    mvp[5] = 2.0f / (tan_up - tan_down);   /* shadercross's single Y-negation still applies downstream --
                                              see build_mvp()'s own comment on this, unchanged here */
    mvp[9] = (tan_up + tan_down) / (tan_up - tan_down);
    mvp[10] = far_plane / (near_plane - far_plane);
    mvp[11] = -1.0f;
    mvp[14] = (far_plane * near_plane) / (near_plane - far_plane);

    /* Fold the combined rotation + eye-position translation into a view
     * matrix, then multiply by the projection built above -- same
     * translate-by-(-R*eye) convention build_mvp() already uses. */
    float view[16] = {0};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            view[c * 4 + r] = combined_rot[r][c];
    view[15] = 1.0f;
    view[12] = -(combined_rot[0][0] * eye_x + combined_rot[0][1] * eye_y + combined_rot[0][2] * eye_z);
    view[13] = -(combined_rot[1][0] * eye_x + combined_rot[1][1] * eye_y + combined_rot[1][2] * eye_z);
    view[14] = -(combined_rot[2][0] * eye_x + combined_rot[2][1] * eye_y + combined_rot[2][2] * eye_z);

    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) sum += mvp[k * 4 + r] * view[c * 4 + k];
            mvp_out[c * 4 + r] = sum;
        }
    (void)vpW; (void)vpH; /* reserved for Phase 3 comfort/resolution tuning, unused here --
                             scene_render_gpu_set_projection_reference_height already
                             handles the resolution-mismatch case per the design spec */
}
```

Declare both new functions in `vr_runtime.h`:

```c
#include <openxr/openxr.h>
struct SceneRenderCamera;
struct SceneRenderProjection;

bool vr_locate_views(XrTime predicted_display_time, XrView out_views[2]);
void build_mvp_vr_eye(float mvp_out[16],
                       const SceneRenderCamera *base_cam,
                       const SceneRenderProjection *base_proj,
                       const XrView *xr_view,
                       float eye_offset_m,
                       int vpW, int vpH);
```

- [ ] **Step 3: Synthetic test of the tracking-loss fallback**

No headset is attached to `fatal-gpu`, so real tracking-loss can't be triggered on demand — write a small standalone throwaway test that links only `vr_locate_views`'s logic against a fake `xrLocateViews` (won't compile against the real one without a real session; instead, extract the *branch logic* into a tiny pure function `bool vr_pose_is_usable(XrViewStateFlags flags)` returning `(flags & (XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT)) == (XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT)`, call it from `vr_locate_views`, and unit-test *that* pure function directly):

```c
/* Add near the top of vr_runtime.c, used by vr_locate_views's validity check: */
static bool vr_pose_is_usable(XrViewStateFlags flags)
{
    return (flags & (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT))
           == (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT);
}
```

Update `vr_locate_views`'s `if (!position_valid || !orientation_valid)` check to `if (!vr_pose_is_usable(view_state.viewStateFlags))` for consistency. Then, in a throwaway file `/tmp/vr_pose_test.c`:

```c
#include <assert.h>
#include <openxr/openxr.h>
#include <stdbool.h>
static bool vr_pose_is_usable(XrViewStateFlags flags) {
    return (flags & (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT))
           == (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT);
}
int main(void) {
    assert(vr_pose_is_usable(XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT) == true);
    assert(vr_pose_is_usable(XR_VIEW_STATE_POSITION_VALID_BIT) == false);
    assert(vr_pose_is_usable(XR_VIEW_STATE_ORIENTATION_VALID_BIT) == false);
    assert(vr_pose_is_usable(0) == false);
    return 0;
}
```

```bash
clang -I/usr/include/openxr /tmp/vr_pose_test.c -o /tmp/vr_pose_test && /tmp/vr_pose_test && echo PASS
rm /tmp/vr_pose_test.c /tmp/vr_pose_test
```
Expected: `PASS`, no assertion failures.

- [ ] **Step 4: Commit**

```bash
cd /config/projects/fatal/ROLLER
git add PROJECTS/ROLLER/vr_runtime.c PROJECTS/ROLLER/vr_runtime.h
git commit -m "vr: add live head-pose tracking with last-known-good fallback, build_mvp_vr_eye"
```

---

### Task 7: `vr_runtime.c` — per-frame OpenXR bracketing and the per-eye present path

**Files:**
- Modify: `PROJECTS/ROLLER/vr_runtime.c`, `PROJECTS/ROLLER/vr_runtime.h`, `PROJECTS/ROLLER/3d.c`

**Interfaces:**
- Consumes: `main_loop_iteration()` (`3d.c:965`), `draw_road()` (`3d.c:2701`), `game_render_set_vr_eye_mvp()`/`game_render_end_frame_to_vr_eye()`/`game_render_get_last_vr_fence()` (Task 4), `build_mvp_vr_eye()`/`vr_locate_views()` (Task 6), `g_pGameRenderer` (existing extern, `3d.c`).
- Produces: `bool vr_run_frame(void);` (called from `frontend_run_game_loop()`'s new VR branch), `void vr_present_stereo_frame(uint8 *pMainScrPtr, unsigned int uiViewType, int iDriveView);` (called from `updatescreen()`'s new VR branch).

- [ ] **Step 1: Per-tick frame state, shared between `vr_run_frame()` and `vr_present_stereo_frame()`**

```c
static XrFrameState s_frame_state;
static XrView       s_frame_views[2];
static bool         s_should_render_this_tick = false;

#define VR_EYE_OFFSET_M 0.0315f  /* half of a 63mm default IPD; revisited in Phase 3 */
```

- [ ] **Step 2: `vr_run_frame()` — event poll, wait/begin/end, no acquire (moved into Step 3)**

Adapted from the original `vr_skeleton_main.c`'s while-loop body (lines 258–301 and 416–436), minus the per-eye block (which moves to Step 3) and minus the outer `while`/`FRAME_COUNT_TO_RUN` limit (Phase 2 runs until the game itself quits, driven by `main_loop_iteration()`'s own return value, not a fixed frame count):

```c
bool vr_run_frame(void)
{
    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    bool should_quit = false;
    while (xrPollEvent(s_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged *state_event = (XrEventDataSessionStateChanged *)&event;
            if (state_event->state == XR_SESSION_STATE_READY && !s_session_running) {
                XrSessionBeginInfo begin_info = {
                    .type = XR_TYPE_SESSION_BEGIN_INFO,
                    .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                };
                if (XR_FAILED(xrBeginSession(s_session, &begin_info))) return false;
                s_session_running = true;
            } else if (state_event->state == XR_SESSION_STATE_STOPPING ||
                       state_event->state == XR_SESSION_STATE_EXITING ||
                       state_event->state == XR_SESSION_STATE_LOSS_PENDING) {
                should_quit = true;
            }
        }
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
    if (should_quit) return false;

    if (!s_session_running) {
        SDL_Delay(10);
        return true; /* keep looping, waiting for READY */
    }

    XrFrameWaitInfo frame_wait_info = { .type = XR_TYPE_FRAME_WAIT_INFO };
    s_frame_state = (XrFrameState){ .type = XR_TYPE_FRAME_STATE };
    if (XR_FAILED(xrWaitFrame(s_session, &frame_wait_info, &s_frame_state))) return false;

    XrFrameBeginInfo frame_begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    if (XR_FAILED(xrBeginFrame(s_session, &frame_begin_info))) return false;

    s_should_render_this_tick = s_frame_state.shouldRender;
    if (s_should_render_this_tick) {
        if (!vr_locate_views(s_frame_state.predictedDisplayTime, s_frame_views)) {
            s_should_render_this_tick = false; /* no pose at all yet (first frame edge case) -- skip this tick's render, still end the frame below */
        }
    }

    /* main_loop_iteration() runs the real game tick (input, physics,
     * frontend state machine). When it reaches race_draw()->updatescreen(),
     * updatescreen()'s g_bVRMode branch (Task 8) calls
     * vr_present_stereo_frame() synchronously, right here in the middle of
     * this function call -- s_frame_views/s_should_render_this_tick above
     * are what it reads. */
    bool keep_running = main_loop_iteration();

    XrCompositionLayerProjection projection_layer = {
        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
        .space = s_local_space,
        .viewCount = s_frame_view_count_for_layer,   /* set by vr_present_stereo_frame; 0 if it never ran this tick */
        .views = s_projection_views,                  /* filled by vr_present_stereo_frame */
    };
    const XrCompositionLayerBaseHeader *submitted_layers[1] = {
        (const XrCompositionLayerBaseHeader *)&projection_layer,
    };
    XrFrameEndInfo frame_end_info = {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime = s_frame_state.predictedDisplayTime,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = s_frame_view_count_for_layer > 0 ? 1u : 0u,
        .layers = s_frame_view_count_for_layer > 0 ? submitted_layers : NULL,
    };
    if (XR_FAILED(xrEndFrame(s_session, &frame_end_info))) return false;

    return keep_running;
}
```

Add the two new statics this references: `static Uint32 s_frame_view_count_for_layer = 0;` and `static XrCompositionLayerProjectionView s_projection_views[2];`, both reset to `s_frame_view_count_for_layer = 0;` at the top of `vr_run_frame()` (before calling `main_loop_iteration()`) so a tick where `updatescreen()`'s VR branch never runs (e.g. still in a non-racing frontend state for one tick) correctly submits `layerCount = 0`.

- [ ] **Step 3: `vr_present_stereo_frame()` — acquire, render, copy, release, per eye**

```c
void vr_present_stereo_frame(uint8 *pMainScrPtr, unsigned int uiViewType, int iDriveView)
{
    if (!s_should_render_this_tick) {
        s_frame_view_count_for_layer = 0;
        return;
    }

    for (Uint32 i = 0; i < s_view_count && i < 2; i++) {
        /* draw_road() recomputes calculateview()/calculatetransform() fresh
         * each call and queues this eye's draw list into g_pGameRenderer's
         * single windowless scene instance -- see this plan's top-of-file
         * note on why there are two draw_road() calls, not two renderer
         * instances. */
        draw_road(pMainScrPtr, uiViewType, iDriveView, -1, 0);

        SceneRenderCamera base_cam;
        SceneRenderProjection base_proj;
        game_render_get_camera(g_pGameRenderer, &base_cam);       /* read back what draw_road() just set */
        game_render_get_projection(g_pGameRenderer, &base_proj);

        float eye_mvp[16];
        float eye_sign = (i == 0) ? -1.0f : 1.0f;
        build_mvp_vr_eye(eye_mvp, &base_cam, &base_proj, &s_frame_views[i],
                          eye_sign * VR_EYE_OFFSET_M, (int)view_configs_width(i), (int)view_configs_height(i));
        game_render_set_vr_eye_mvp(g_pGameRenderer, eye_mvp);

        Uint32 image_index = 0;
        XrSwapchainImageAcquireInfo acquire_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (XR_FAILED(xrAcquireSwapchainImage(s_swapchains[i], &acquire_info, &image_index))) return;
        XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
        if (XR_FAILED(xrWaitSwapchainImage(s_swapchains[i], &wait_info))) return;

        Uint32 xr_image_count = 0;
        xrEnumerateSwapchainImages(s_swapchains[i], 0, &xr_image_count, NULL);
        XrSwapchainImageVulkan2KHR *xr_images = calloc(xr_image_count, sizeof(XrSwapchainImageVulkan2KHR));
        for (Uint32 k = 0; k < xr_image_count; k++) xr_images[k].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
        xrEnumerateSwapchainImages(s_swapchains[i], xr_image_count, &xr_image_count,
                                    (XrSwapchainImageBaseHeader *)xr_images);
        VkImage target_image = xr_images[image_index].image;
        free(xr_images);

        Uint32 eye_w = view_configs_width(i), eye_h = view_configs_height(i);
        SDL_GPUTexture *resolved = game_render_end_frame_to_vr_eye(g_pGameRenderer, eye_w, eye_h);
        SDL_GPUFence *fence = game_render_get_last_vr_fence(g_pGameRenderer);
        if (!resolved || !fence) return;

        /* Fence-wait serialization (Approach A from the design spec) --
         * simple and correct; semaphore-based overlap is explicitly deferred
         * to Phase 3 per the spec. */
        SDL_WaitForGPUFences(s_gpu, true, &fence, 1);
        SDL_ReleaseGPUFence(s_gpu, fence);

        SDL_PropertiesID tex_props = SDL_GetGPUTextureProperties(resolved);
        VkImage resolved_vk_image = (VkImage)SDL_GetPointerProperty(
            tex_props, SDL_PROP_GPU_TEXTURE_VULKAN_IMAGE_POINTER, NULL);
        if (!resolved_vk_image) {
            fprintf(stderr, "vr_runtime: SDL_GetGPUTextureProperties did not expose a VkImage "
                             "for the resolved eye texture (is this the patched SDL fork?)\n");
            return;
        }

        vkResetCommandBuffer(s_command_buffer, 0);
        VkCommandBufferBeginInfo cb_begin_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(s_command_buffer, &cb_begin_info);

        VkImageSubresourceRange subresource_range = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1,
        };
        VkImageMemoryBarrier src_to_transfer_src = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL, /* resolveTarget's steady-state layout after SDL_GPU's own submit -- verify against SDL_GPU's actual post-render layout for a sampled/color-target texture before relying on this */
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = resolved_vk_image, .subresourceRange = subresource_range,
        };
        VkImageMemoryBarrier dst_to_transfer_dst = src_to_transfer_src;
        dst_to_transfer_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        dst_to_transfer_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dst_to_transfer_dst.image = target_image;
        VkImageMemoryBarrier pre_barriers[2] = { src_to_transfer_src, dst_to_transfer_dst };
        vkCmdPipelineBarrier(s_command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, pre_barriers);

        VkImageCopy copy_region = {
            .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
            .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
            .extent = { eye_w, eye_h, 1 },
        };
        vkCmdCopyImage(s_command_buffer, resolved_vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

        VkImageMemoryBarrier dst_to_general = dst_to_transfer_dst;
        dst_to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dst_to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(s_command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &dst_to_general);
        vkEndCommandBuffer(s_command_buffer);

        VkSubmitInfo submit_info = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &s_command_buffer,
        };
        vkQueueSubmit(s_vk_queue, 1, &submit_info, VK_NULL_HANDLE);
        vkQueueWaitIdle(s_vk_queue); /* same fence-serialized simplicity as the copy's own producer wait above */

        XrSwapchainImageReleaseInfo release_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(s_swapchains[i], &release_info);

        s_projection_views[i] = (XrCompositionLayerProjectionView){
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
            .pose = s_frame_views[i].pose,
            .fov = s_frame_views[i].fov,
            .subImage = {
                .swapchain = s_swapchains[i],
                .imageRect = { .extent = { (int32_t)eye_w, (int32_t)eye_h } },
            },
        };
    }
    s_frame_view_count_for_layer = s_view_count;
}
```

This step's code references `game_render_get_camera`/`game_render_get_projection` (read-back accessors) and `view_configs_width`/`view_configs_height` helpers that don't exist yet in the codebase — add the two `game_render_get_*` accessors to `game_render.h`/`.c` now (trivial: `renderer->scene`'s already-stored camera/projection, mirroring the existing `game_render_set_camera`/`set_projection` setters' shape exactly but reading instead of writing — read those setters first to get the exact field-copy pattern right), and add `view_configs_width`/`_height` as two tiny static helpers in `vr_runtime.c` returning `s_view_configs[i].recommendedImageRectWidth`/`Height`.

The `VK_IMAGE_LAYOUT_GENERAL` assumed as `resolveTarget`'s pre-copy layout is flagged inline above as needing verification — read `ensure_offscreen_texture()`'s creation flags and whatever layout `scene_render_gpu_end_frame_internal()`'s existing resolve/copy-pass code already transitions `resolveTarget` to (SDL_GPU manages layouts somewhat opaquely; check whether `SDL_GPU_TEXTUREUSAGE_SAMPLER`/`COLOR_TARGET` combination SDL_GPU uses for this texture has a documented steady-state layout, or add an explicit `vkCmdPipelineBarrier` from `VK_IMAGE_LAYOUT_UNDEFINED` instead if SDL_GPU's actual layout can't be determined statically) before the fatal-gpu run in Task 9 — this is exactly the kind of Vulkan Valid Usage detail the Global Constraints section requires checking against real documentation, not memory.

- [ ] **Step 4: Wire `vr_run_frame()` into `frontend_run_game_loop()`**

In `3d.c`, change `frontend_run_game_loop()` (line 1006):

```c
static void frontend_run_game_loop(eFrontendState eInitialState)
{
  VIEWDIST = 270;
  frontend_set_state(eInitialState);

#if defined(__EMSCRIPTEN__)
  emscripten_set_main_loop(main_loop_iteration_wrapper, 0, 1);
#else
  if (g_bVRMode) {
    while (vr_run_frame()) { }
  } else {
    while (main_loop_iteration()) { }
  }
#endif
}
```

Add `#include "vr_runtime.h"` near `3d.c`'s other includes, guarded: `#if !defined(ROLLER_EDITOR_CORE)` (matching how `main()` itself is guarded at line 2806) is not appropriate here since `frontend_run_game_loop` is unconditionally compiled — instead guard the `#include` and the `if (g_bVRMode)` branch behind a new build-time macro `#if defined(ROLLER_VR_BUILD)` so the symbol `vr_run_frame`/`vr_runtime.h` is only referenced when the `-Dvr` build actually links `vr_runtime.c` (Task 9 defines `ROLLER_VR_BUILD` only for the `roller-vr` target, never for the default `roller` build):

```c
#if defined(ROLLER_VR_BUILD)
  if (g_bVRMode) {
    while (vr_run_frame()) { }
  } else
#endif
  {
    while (main_loop_iteration()) { }
  }
```

- [ ] **Step 5: Compile-check**

```bash
cd /config/projects/fatal/ROLLER
zig build -Dvr 2>&1 | tail -80
```
Fix any remaining signature mismatches Task 3/4's placeholder-named internal function (`scene_render_gpu_end_frame_internal_for_vr_eye`) surfaces now that it's actually called end-to-end.

- [ ] **Step 6: Commit**

```bash
cd /config/projects/fatal/ROLLER
git add PROJECTS/ROLLER/vr_runtime.c PROJECTS/ROLLER/vr_runtime.h PROJECTS/ROLLER/game_render.c PROJECTS/ROLLER/game_render.h PROJECTS/ROLLER/3d.c
git commit -m "vr: wire per-frame OpenXR bracketing and per-eye render/copy into the real game loop"
```

---

### Task 8: `updatescreen()`'s VR branch — replace the single present with `vr_present_stereo_frame()`

**Files:**
- Modify: `PROJECTS/ROLLER/3d.c`

**Interfaces:**
- Consumes: `vr_present_stereo_frame()` (Task 7), `g_bVRMode` (Task 2), existing `draw_road()`/`game_render_end_frame()` call site inside `updatescreen()` (`3d.c:2676`/`2686`/`2696`).

- [ ] **Step 1: Guard the existing call site**

At `3d.c:2676`, change:

```c
  draw_road(pMainScrPtr, ViewType[0], DriveView[0], -1, 0);// Draw main road view
  if (bCinemaActive && !bRenderNative && bGPUMode) {
    game_render_end_cinema_pass(g_pGameRenderer, winw * 2, winh * 2);
    game_render_composite_cinema_view(g_pGameRenderer, winx, winy, winw, winh);
  }
  // CHEAT_MODE_WIDESCREEN
  if ((cheat_mode & CHEAT_MODE_WIDESCREEN) == 0)
  {
  LABEL_59:
    init_animate_ads();
    game_render_end_frame(g_pGameRenderer);
    return;
  }
```

to:

```c
#if defined(ROLLER_VR_BUILD)
  if (g_bVRMode) {
    vr_present_stereo_frame(pMainScrPtr, ViewType[0], DriveView[0]);
    init_animate_ads();
    return;
  }
#endif
  draw_road(pMainScrPtr, ViewType[0], DriveView[0], -1, 0);// Draw main road view
  if (bCinemaActive && !bRenderNative && bGPUMode) {
    game_render_end_cinema_pass(g_pGameRenderer, winw * 2, winh * 2);
    game_render_composite_cinema_view(g_pGameRenderer, winx, winy, winw, winh);
  }
  // CHEAT_MODE_WIDESCREEN
  if ((cheat_mode & CHEAT_MODE_WIDESCREEN) == 0)
  {
  LABEL_59:
    init_animate_ads();
    game_render_end_frame(g_pGameRenderer);
    return;
  }
```

The VR branch deliberately skips the cinema/widescreen-cheat/mirror handling below this point (lines 2677–2696 and the mirror-buffer code earlier in the function) — none of that applies to a VR cockpit view, matching the design spec's non-goal that VR mode doesn't need pixel-parity with any non-VR display mode.

Read the function's second `game_render_end_frame(g_pGameRenderer)` call site (line 2696, the `CHEAT_MODE_WIDESCREEN` path) and confirm the VR early-return above (right after the first branch) makes that second call site unreachable in VR mode — it is, since the VR branch's own `return` fires before reaching either.

- [ ] **Step 2: Compile-check**

```bash
cd /config/projects/fatal/ROLLER
zig build -Dvr 2>&1 | tail -80
zig build 2>&1 | tail -40   # non-VR default build must still succeed unchanged
```

- [ ] **Step 3: Commit**

```bash
cd /config/projects/fatal/ROLLER
git add PROJECTS/ROLLER/3d.c
git commit -m "vr: replace the normal single present with the stereo VR path in updatescreen()"
```

---

### Task 9: `build.zig`/`build.zig.zon` — rename to `roller-vr`, link real game sources

**Files:**
- Modify: `build.zig`, `build.zig.zon`

**Interfaces:**
- Consumes: `vr_exe_mod`/`vr_exe` (existing `-Dvr` block, `build.zig:289-345`), `exe_mod`'s existing C source list (the same list the default `roller` target already compiles, `build.zig:90-190`).

- [ ] **Step 1: Rename the target and give it the same C sources as the main executable**

In `build.zig`, inside the `if (build_vr) { ... }` block (line 289), change:

```zig
        const vr_exe_mod = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        });
        vr_exe_mod.addIncludePath(sdl.builder.path("include"));
        vr_exe_mod.addCSourceFiles(.{
            .flags = c_flags,
            .files = &.{"PROJECTS/ROLLER/vr_skeleton_main.c"},
        });

        const vr_exe = b.addExecutable(.{
            .name = "vr_skeleton",
            .root_module = vr_exe_mod,
        });
```

to:

```zig
        const vr_exe_mod = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        });
        vr_exe_mod.addIncludePath(sdl.builder.path("include"));
        vr_exe_mod.addCMacro("ROLLER_VR_BUILD", "1");
        /* Same C sources as the main `roller` executable (exe_mod above) --
         * roller-vr needs the real game (track loading, car physics, the
         * frontend state machine), not the roller-core.srclist library
         * subset (which deliberately excludes roller.c's game entry point
         * and stubs sound/CD/comms for TrackEditor's library-consumption
         * use case -- verified in this plan's research, not assumed). */
        vr_exe_mod.addCSourceFiles(.{
            .flags = c_flags,
            .files = exe_mod_c_sources, /* the same slice exe_mod's own addCSourceFiles call
                                            at build.zig:186 uses -- extract that file list to a
                                            named local (e.g. `const exe_mod_c_sources = &.{...}`)
                                            right before exe_mod's own addCSourceFiles call, if it
                                            isn't already a named slice, so both targets share one
                                            literal list instead of duplicating it */
        });
        vr_exe_mod.addCSourceFiles(.{
            .flags = c_flags,
            .files = &.{"PROJECTS/ROLLER/vr_runtime.c"},
        });
        /* Same non-SDL include paths / other C macros exe_mod sets (assets
         * path, platform defines) -- copy whatever exe_mod configures beyond
         * its C source list itself; read exe_mod's full setup between
         * build.zig:90-200 and mirror every include path / macro that isn't
         * SDL-specific (SDL comes from the vr fork dependency instead of
         * exe_mod's own SDL setup). */

        const vr_exe = b.addExecutable(.{
            .name = "roller-vr",
            .root_module = vr_exe_mod,
        });
```

- [ ] **Step 2: Compile the renamed target**

```bash
cd /config/projects/fatal/ROLLER
zig build -Dvr 2>&1 | tee /tmp/vr-phase2-build.log
```
Work through the (expected, real) list of missing-include/undefined-macro errors this surfaces — `exe_mod`'s full include-path/macro setup has more in it than the VR module currently mirrors (asset path handling, platform defines like `__int16`/`_O_RDONLY` seen in `build.zig`'s non-wasm `else` branch at line 280). Add each one to `vr_exe_mod` as the errors identify them; this is expected iteration, not a sign the plan is wrong — `exe_mod`'s exact non-SDL configuration wasn't fully enumerated during planning (only its C source list and the platform-macro branch were read).

- [ ] **Step 3: Confirm `roller-vr.exe`'s name on Windows cross-compile**

```bash
cd /config/projects/fatal/ROLLER
zig build -Dvr -Dtarget=x86_64-windows-gnu 2>&1 | tail -40
ls zig-out/bin/
```
Expected: `roller-vr.exe` present (Windows compile-only per the spec's non-goals — no Windows GPU rig exists yet, this step only confirms it builds).

- [ ] **Step 4: Deploy and run on `fatal-gpu`**

```bash
cd /config/projects/fatal/ROLLER
zig build -Dvr 2>&1 | tail -20   # native Linux build for fatal-gpu
scp zig-out/bin/roller-vr fatal-gpu:/root/roller-vr
ssh fatal-gpu 'chmod +x /root/roller-vr'
ssh fatal-gpu 'cd /root && XR_ENABLE_API_LAYERS=XR_APILAYER_LUNARG_core_validation \
  DISPLAY=:1 SDL_VIDEODRIVER=x11 \
  ./roller-vr --vr --track-path /root/fatdata/TRACKS/TRACK1.TRK --whiplash-root /root/fatdata 2>&1 | tee /root/roller-vr-run.log'
```
(Confirm the exact `--track-path`/`--whiplash-root` argument names and a real track file path against Task 2 Step 3's findings and this LXC's `builds/linux/fatdata/` layout before running — adjust the command to match what's actually there.)

- [ ] **Step 5: Validate against the spec's success criteria**

Pull the log back and check:

```bash
scp fatal-gpu:/root/roller-vr-run.log /tmp/roller-vr-run.log
grep -c "core_validation" /tmp/roller-vr-run.log   # expect 0 error-level lines from the validation layer
grep "vr_runtime:" /tmp/roller-vr-run.log | tail -20
echo "exit code check:" && ssh fatal-gpu 'echo $?'
```

Confirm: exit code 0 (or a clean intentional shutdown, not a crash), no `XR_ERROR_*`/validation-layer error lines, and a log-level sanity check that head-pose values change frame to frame — add a temporary debug log line to `vr_locate_views()` (`fprintf(stderr, "vr_runtime: view[0] pos=(%.3f,%.3f,%.3f)\n", ...)`) for this run only if `s_have_last_good_views` visibility isn't already sufficient to confirm the values aren't frozen at identity across the whole log.

- [ ] **Step 6: Re-run the non-VR regression baseline**

```bash
cd /config/projects/fatal/ROLLER
zig build test-snapshots 2>&1 | tee /tmp/vr-phase2-final-snapshots.log
diff /tmp/vr-phase2-baseline-snapshots.log /tmp/vr-phase2-final-snapshots.log
```
Expected: no diff (or only expected non-deterministic noise like timestamps — confirm any diff is not a pixel/pass-count regression before proceeding).

- [ ] **Step 7: Update project docs**

Update `docs/superpowers/specs/2026-09-23-roller-vr-phase2-design.md`'s status line to reflect the outcome (PASS/details, mirroring how Phase 1's spec was updated after its own hardware run), and update `/config/projects/fatal/CLAUDE.md`'s VR section + the `fatal_project` memory with the Phase 2 outcome, per this repo's own documentation rule.

- [ ] **Step 8: Commit**

```bash
cd /config/projects/fatal/ROLLER
git add build.zig build.zig.zon docs/superpowers/specs/2026-09-23-roller-vr-phase2-design.md
git commit -m "vr: rename -Dvr target to roller-vr, link real game sources, Phase 2 hardware validation"
```
