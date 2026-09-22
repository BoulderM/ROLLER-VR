# ROLLER VR Design

- **Status:** Approved for planning
- **Date:** 2026-09-22
- **Branch:** `roller-vr` (forked off `master`, upstream-able later)

## Goal

Add full immersive cockpit VR to ROLLER: head-tracked stereo rendering from
inside/behind the car, native 6DOF look-around, and OpenXR motion-controller
support for menus and secondary in-race actions. Driving itself stays on
existing gamepad/wheel/keyboard input (`rollerinput.c`) — VR adds head
tracking and controller input, it does not replace how the car is driven.

Target API is **OpenXR** (not legacy OpenVR) — runtime-agnostic, fully
SteamVR-compatible, matches Valve's own current recommendation. Target
platforms are **Windows and Linux x86_64 desktop SteamVR** first (Index,
Vive, Quest-via-Link/Airlink); **Steam Frame (ARM/standalone)** is an
explicit stretch goal on its own research track, not part of the initial
build (see Phase 5).

## Non-goals

- Replacing wheel/gamepad/keyboard as the primary driving input.
- Flat-screen-in-VR / virtual theater mode (rejected during brainstorming —
  cannot deliver head-tracked stereo from a monoscopic renderer, and doesn't
  meet the stated goal).
- Matching the legacy DOS software renderer's output pixel-for-pixel in VR
  mode (the rendering-snapshot test suite's parity guarantee applies to the
  existing flat renderers, not the new VR eye-render path).
- Any change to `ROLLER-matching`. This work stays on a `roller-vr` branch
  of `ROLLER` only.

## Architecture decision: device interop (Approach C)

Three approaches were evaluated for how OpenXR gets a GPU device to render
into:

- **B — own the device.** Reimplement the render pipeline directly against a
  raw Vulkan/D3D12 device, bypassing SDL_GPU. Rejected: forks the ~6000-line
  `scene_render_gpu.c` pipeline into a second full backend for no benefit
  over C.
- **C — patch the vendored SDL fork (chosen).** ROLLER already depends on a
  fork, `Zizin13/SDL`, not upstream SDL — the project already patches SDL
  where needed. `SDL_GPU`'s public API exposes device-creation *input*
  properties (debug mode, preferred driver, shader formats) but no *output*
  properties for the `VkDevice`/`VkPhysicalDevice`/`VkQueue` (or D3D12
  equivalents) it creates internally — confirmed by reading
  `include/SDL3/SDL_gpu.h` in the vendored dependency. The legacy
  `SDL_Renderer` API *does* expose Vulkan handles this way
  (`SDL_PROP_RENDERER_VULKAN_DEVICE_POINTER` etc.), so Approach C patches
  `SDL_GPU`'s Vulkan and D3D12 backends to expose the equivalent output
  properties, mirroring that existing pattern. OpenXR then binds to the same
  device SDL_GPU owns via `XR_KHR_vulkan_enable2` / `XR_KHR_D3D12_enable`,
  and the game's render pipeline is untouched.
- **D — separate OpenXR-owned device + cross-device texture sharing.**
  Stand up an independent Vulkan device for the XR session, share finished
  eye textures across devices via external-memory extensions. No SDL patch,
  but adds per-frame interop/synchronization complexity and a second device
  to manage. Not chosen; noted here as the fallback if Approach C's patch
  proves impractical to land or maintain.

**Consequence:** the SDL fork patch is an ongoing maintenance cost (rebasing
against upstream SDL_GPU changes). Accepted as smaller than B's rewrite or
D's per-frame interop risk.

## Camera/projection: an addition, not a rewrite

Research (Phase 0, see below) found the GPU render path already builds a
real 4×4 MVP matrix per frame in `build_mvp()`
(`PROJECTS/ROLLER/scene_render_gpu.c:809`), derived from the legacy
`SceneRenderCamera`/`SceneRenderProjection` structs (`fovScale`, a 3×3
rotation `view[3][3]`, position, and a `centerY`-driven vertical shift that
matches the original software renderer's horizon convention).

The VR integration point is a **parallel** matrix-construction function —
`build_mvp_vr_eye()` or similar — that takes an OpenXR `XrView` (pose +
asymmetric `fov` angles: `angleLeft/Right/Up/Down`) directly and produces the
same 16-float column-major MVP layout the shader already consumes, using the
standard OpenXR projection-matrix construction rather than force-fitting
asymmetric per-eye frustums into the legacy symmetric `fovScale`/`centerY`
representation. This is a small, isolated addition (the existing function is
~40 lines) — not a rewrite of the render pipeline.

`SCENE_GPU_NEAR`/`SCENE_GPU_FAR` (currently 10.0f / 20,000,000.0f) need
VR-specific values for stereo depth precision at cockpit distances; tuning
these is part of Phase 2, not a blocker.

## Resolution: no blocker

The fixed 640×400 `SDL_GPUTexture` found during research
(`PROJECTS/ROLLER/roller.c`) is the legacy 2D menu/HUD framebuffer feeding
the CRT post-filter — a separate subsystem from the 3D scene renderer, and
irrelevant to VR (VR bypasses the CRT filter and legacy 2D menu compositing
for the in-race view; see Phase 4 for VR menu handling). The 3D path already
has `scene_render_gpu_set_projection_reference_height()` to decouple the
legacy 200-line logical coordinate space from an arbitrary physical viewport
size, and **TrackEditor already exercises this in production** at arbitrary
Qt-viewport resolutions. Headset-native per-eye resolution (2000×2000+) is
supported by existing infrastructure.

## Scenery streaming: low risk, not a blocker

`CalcVisibleBuildings()` (and the equivalent track-chunk walk) drives the
runtime visible-set from a camera-position-derived `start_sect`, at
section/chunk granularity (tens of meters). A cockpit head offset from the
car's anchor point (sub-meter) is negligible at that granularity. Worth an
empirical check once real hardware testing starts (Phase 3); not an
architectural concern.

*(Note: `docs/adr/0005-camera-independent-scenery-traversal.md` was
initially assumed to cover this and does not — it documents TrackEditor's
static OBJ/glTF export traversal, unrelated to runtime rendering.)*

## Phases

### Phase 0 — Research spikes (complete)

All four spikes resolved above: SDL_GPU interop surface (→ Approach C),
camera/projection substitution point (→ `build_mvp_vr_eye()`), resolution
assumptions (→ no blocker), scenery streaming (→ low risk). No code was kept
from this phase; findings are recorded here.

### Phase 1 — Walking skeleton (Linux/Monado leg, 2026-09-22 — COMPLETE, PASS)

1. Patch `Zizin13/SDL`'s Vulkan and D3D12 `SDL_GPU` backends to expose
   `SDL_PROP_GPU_DEVICE_VULKAN_{INSTANCE,PHYSICAL_DEVICE,DEVICE}_POINTER` /
   D3D12 equivalents via `SDL_GetGPUDeviceProperties()`. Point
   `build.zig.zon` at a branch/commit of the fork for the VR build, gated
   behind a new `-Dvr` build option so non-VR builds are unaffected.
2. Use those handles to satisfy `XR_KHR_vulkan_enable2` /
   `XR_KHR_D3D12_enable`'s graphics-binding requirements; open an OpenXR
   instance/session; create eye swapchains against the same device.
3. Render a solid color to both eyes (no game content). **Deviation from
   the original plan, confirmed correct once implemented:** this does not
   go through the offscreen windowless GPU path
   (`scene_render_gpu_create_windowless`) as originally sketched here —
   `SDL_GPU` has no API to wrap an externally-provided `VkImage` (the
   per-eye swapchain image OpenXR hands back), so `vr_skeleton_main.c`
   instead issues raw Vulkan commands (`vkCmdClearColorImage` inside a
   one-off command buffer, submitted straight to the queue handle SDL_GPU
   exposed) directly against the `VkImage` from `xrEnumerateSwapchainImages`.
   `scene_render_gpu.c`'s SDL_GPU pipeline is untouched; this raw-Vulkan
   path is specific to writing into OpenXR-owned swapchain images and is
   expected to carry forward into Phase 2/3's real eye rendering.

**Result — Linux x86_64 / Monado, three runs against `fatal-gpu` (2026-09-22):**
**`vr_skeleton: PASS`, exit code 0.** OpenXR opens a session against the
exact `VkInstance`/`VkPhysicalDevice`/`VkDevice`/queue `SDL_GPU` created
internally (Approach C, confirmed end-to-end), renders 120 frames to two
independent per-eye swapchains (left eye cleared red, right eye blue —
cheap visual proof the swapchains are genuinely separate targets, not the
same image cleared twice), and shuts down cleanly. This is Phase 1's
walking-skeleton goal, fully achieved.

Three issues surfaced across the three runs, each a narrow, single-call
OpenXR-sequencing gap in `vr_skeleton_main.c` — never an architecture
problem; Approach C's device-interop premise held at every step:

1. *Run 1:* stopped at `xrCreateSession` —
   `XR_ERROR_VALIDATION_FAILURE: "Has not called xrGetVulkanGraphicsDevice2KHR"`.
   The code called `xrGetVulkanGraphicsRequirements2KHR` but never the
   companion `xrGetVulkanGraphicsDevice2KHR`, which `XR_KHR_vulkan_enable2`
   requires before `xrCreateSession` with an `XrGraphicsBindingVulkan2KHR`.
   **Fixed in commit `624c07e`.**
2. *Run 2 (after fix 1):* `xrCreateSession` succeeded, view count 2
   (896×1007 recommended per eye), both swapchains created, session state
   progressed correctly through the full healthy sequence — `IDLE`(1) →
   `READY`(2) → `xrBeginSession` → `SYNCHRONIZED`(3) → `VISIBLE`(4) →
   `FOCUSED`(5) (numeric values cross-checked against
   `/usr/include/openxr/openxr.h` on `fatal-gpu`; note `READY = 2`, not `3`
   as originally guessed in planning — harmless, corrected here). The first
   `xrWaitFrame`/`xrBeginFrame` succeeded and the per-eye Vulkan clear
   commands ran, but it then stopped at the first `xrEndFrame` —
   `XR_ERROR_VALIDATION_FAILURE: "xrEndFrame(frameEndInfo->layers[0]->space
   == XR_NULL_HANDLE) XrSpace must not be XR_NULL_HANDLE"`. The code built
   its `XrCompositionLayerProjection` but never called
   `xrCreateReferenceSpace` or set `projection_layer.space`, leaving it
   `XR_NULL_HANDLE`. **Fixed in commit `5ce666b`** (one-time
   `XR_REFERENCE_SPACE_TYPE_LOCAL` space created after swapchain creation,
   assigned to `projection_layer.space` each frame, destroyed at cleanup).
3. *Run 3 (after fix 2):* **all 120 frames rendered, `vr_skeleton: PASS`,
   exit code 0.** One residual, non-fatal finding in the log:
   `XR_ERROR_SESSION_NOT_STOPPING in xrEndSession: Session is not stopping`
   — the cleanup path calls `xrEndSession()` unconditionally whenever
   `session_running` is true, without checking the session is actually in
   `STOPPING` state (it's still `FOCUSED`, since the loop exits by hitting
   the 120-frame count rather than by receiving a `STOPPING` event from the
   runtime) and without checking the call's return value, so this doesn't
   fail the program. Not a blocker for Phase 1; flagged as a minor
   cleanup-path imperfection for Phase 2, not resolved as part of this plan.

See `task-6-report.md` in this plan's directory for the full verbatim
output of all three runs and the process-launch notes (Monado's IPC loop
needs a non-`/dev/null`, epoll-compatible stdin fd — e.g. a bash
process-substitution pipe — to start at all under a backgrounded SSH
launch; an environment quirk, unrelated to the OpenXR/Vulkan interop this
task set out to prove).

Windows/SteamVR validation (the "first, before Linux" ordering originally
specified) was not performed in any of the three runs — deferred to the
user, who has the hardware; see the existing "Windows validation gap"
self-review note. Separately, and more fundamentally: the Windows `-Dvr`
build code path itself (the `build.zig` linking logic against
`VULKAN_SDK`/`OPENXR_SDK_PATH`, added during Task 5) has never actually been
compiled even once, on any platform — this dev environment has no Windows
toolchain and no cross-compile toolchain with those SDKs available, so it is
unverified in a stronger sense than "no hardware to run it on"; it may not
even build. Phase 1's Linux/Monado leg is complete; Phase 2 can now build on
a confirmed-working device-interop foundation, but the Windows build path
still needs a first compile, not just a first run.

**Lessons for Phase 2 (from Task 6 hardware bugs + final review):** the two
real bugs found on hardware during Task 6 (missing
`xrGetVulkanGraphicsDevice2KHR`, missing `xrCreateReferenceSpace`), plus
several minor issues caught in this branch's final code review, share one
root cause — Phase 1's OpenXR call sequence was written from general
knowledge of the API shape rather than checked against
`XR_KHR_vulkan_enable2`'s Valid Usage requirements or the official Khronos
`hello_xr` sample. Monado's simulated HMD is an unusually permissive test
runtime and didn't catch everything a stricter real compositor might reject
— notably, SDL's Vulkan device/instance were never told what extensions the
OpenXR runtime requires (the spec's intended flow for that is
`xrCreateVulkanInstanceKHR`/`xrCreateVulkanDeviceKHR`, which this walking
skeleton deliberately bypasses since it uses an already-existing device
instead). Recommendation for whoever writes the Phase 2 plan: (a) run
against the OpenXR core validation API layer
(`XR_APILAYER_LUNARG_core_validation`) rather than relying on
hardware-testing alone; (b) author the plan directly against the spec's
Valid Usage sections and the `hello_xr` sample rather than from recall; and
(c) resolve `VkQueue` external synchronization between SDL_GPU's own
internal command submission and future VR rendering before Phase 2 submits
real scene content to the same queue — Vulkan requires this to be
externally synchronized, and right now it's only safe because the walking
skeleton and SDL_GPU never submit concurrently.

### Phase 2 — Stereo cockpit camera

Add `build_mvp_vr_eye()`. Render twice per frame (separate draw pass per
eye — no single-pass/multiview optimization yet). Anchor the head pose to a
cockpit-relative position on the car, not the existing chase-cam. Tune
near/far planes for VR. Disable CRT filter and 2D post-process for the VR
eye targets.

### Phase 3 — Comfort & desktop validation

OpenXR governs frame pacing (`xrWaitFrame`/`xrBeginFrame`/`xrEndFrame`) for
the VR path, replacing SDL's present-mode logic (used only by the flat
build). Add a spectator/mirror window on the desktop swapchain. Validate on
real hardware: Index/Vive/Quest-via-Link on Windows, then Linux x86_64
SteamVR. Comfort mitigations (vignetting, FOV reduction during high-G
moments) are a design pass informed by real testing feedback here, not
designed blind up front.

### Phase 4 — Motion controllers

OpenXR action sets/bindings for common controllers (Index, Vive wands, Quest
Touch via Link). Two uses: a 3D laser-pointer interaction layer for the
existing Nuklear-based menu system (`menu_render_gpu.c` assumes a flat 2D
screen today), and secondary in-race functions (recenter view, pause) layered
on top of the primary wheel/gamepad/keyboard driving input.

### Phase 5 — Steam Frame / ARM native (stretch, separate research track)

Explicitly deferred behind a working desktop baseline (Phases 1–4). Open
questions: current public SDK/toolchain availability for Steam Frame,
whether it is "just another OpenXR runtime on Linux ARM64" (reusing patterns
already in `build.zig` from the existing Android NDK ARM64 target) or
something more bespoke, and foveated-rendering/perf-budget needs for a
mobile SoC. Needs its own research spike before any build work; not
estimated as part of this plan.

## Testing strategy

The existing rendering-snapshot harness (`zig build test-snapshots`, diffing
framebuffer captures against Git LFS baselines) doesn't directly extend to
stereo/headset-driven poses, but the windowless GPU renderer path it depends
on is exactly what Phases 1–2 reuse for eye-texture rendering. A VR-equivalent
harness (synthetic OpenXR poses, per-eye snapshot diffing) is realistic to
add later on the same pattern — not designed as part of this initial build,
but not blocked by anything in it either.

## Risk register

| Risk | Notes |
|---|---|
| SDL fork patch maintenance | Ongoing cost of Approach C; rebase against upstream SDL_GPU changes as needed. |
| Steam Frame hardware/SDK uncertainty | Deferred entirely to Phase 5's own spike. |
| VR near/far tuning | Real values needed for cockpit-distance depth precision; a Phase 2 detail. |
| VR comfort (motion sickness) | Known risk category for first-person racing VR; no mitigation designed yet, informed by Phase 3 hardware testing. |
| No GPU in the dev LXC | This container has no `/dev/dri`/GPU passthrough, so `SDL_CreateGPUDevice` fails here even for the existing flat build. All hardware validation (Phases 1, 3+) needs the separate GPU-equipped box. |

## Open items for implementation planning

- Exact `Zizin13/SDL` fork branching strategy for the patch (separate branch
  vs. a maintained patch series).
- Whether the VR build's `-Dvr` option should also gate CI (`ci.yml`,
  `build.yml`) or stay a manual/local-only build path until Phase 3 lands.

## Known limitations blocking upstream

These are accepted, deliberate, **local-only** provisional states for
continued development on the `vr-phase1-impl` branch on this dev LXC — not
something that should be merged toward `master` or exercised in CI as-is.

- `build.zig.zon`'s `.sdl` dependency currently resolves through a
  git-tracked symlink (`external/sdl-vr-fork`) pointing at an absolute host
  path (`/root/sdl-vr-fork`), which only exists on this specific development
  LXC.
- This is **not** gated behind `-Dvr` — Zig's `.zon` manifest has no
  conditionals — so it affects the default, non-VR build too: `zig build`,
  `zig build test-snapshots`, `zig build web`, and both GitHub Actions
  workflows would all fail on any other machine, including CI.
- It also means the SDL dependency currently has no `.hash` integrity pin
  (Zig only hash-verifies URL-based dependencies, not local paths) — no
  reproducibility guarantee for ROLLER's largest third-party dependency right
  now.
- Before this can be merged toward `master` or run in CI, the patched SDL
  fork branch (`vr-gpu-properties`, currently only pushed to nowhere — it's a
  local clone at `/root/sdl-vr-fork`) needs to be pushed to a real git
  remote, and `build.zig.zon` needs to go back to a `git+https://...#<commit-sha>`
  URL with a proper `.hash`.
