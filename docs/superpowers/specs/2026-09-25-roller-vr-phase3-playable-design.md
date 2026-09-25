# ROLLER VR — Phase 3: Playable VR

**Status:** Design and this written spec approved by the user on 2026-09-25. The M1 plan is written (`docs/superpowers/plans/2026-09-25-vr-phase3-m1-windows-steamvr-lifecycle.md`); nothing is implemented yet.
**Date:** 2026-09-25
**Branch:** `roller-vr`
**Depends on:** `docs/superpowers/specs/2026-09-22-roller-vr-design.md` (overall VR design), `docs/superpowers/specs/2026-09-23-roller-vr-phase2-design.md` (Phase 2: stereo attract race; read its "Known gaps" and "Follow-ups")

## Goal

Make `roller-vr` a playable game: on a Windows 11 PC running SteamVR and streaming to a Steam Frame, the user starts the game, navigates the menus, drives a race with sound and a HUD, looks around freely with correct head tracking, and quits cleanly, all without taking the headset off.

Phase 2 delivered a stereo render of the attract-mode race and nothing else (no input, no audio, no menus, no HUD, only partial head tracking, LXC-only). This phase closes those gaps. It pulls forward work the earlier specs had parked as "Phase 3/4" (menus, motion controllers, head-tracked culling); comfort options, frame-pacing tuning and a picture in the mirror window stay deferred.

### Definition of done

On the user's PC with SteamVR and the Steam Frame:

1. `roller-vr.exe` starts and the title screen appears on a floating screen in the headset.
2. The menus can be navigated with keyboard, gamepad or Frame controllers, with music and effects audible.
3. A race can be started and driven with any of the three, with the HUD visible.
4. Head tracking is correct in the race: a full 360-degree head turn keeps the horizon level and nothing vanishes.
5. The game can be quit from inside the headset and from SteamVR's own exit; both shut down cleanly (teardown and save) and the process exits.

### Non-goals

- Comfort options (vignette, snap turning, horizon stabilisation), frame-pacing tuning, spectator/mirror-window picture (the mirror window exists only to hold keyboard focus).
- Two-player split screen (player 1 only), mirror-view and widescreen presentations (Phase 2 non-goals still hold).
- Haptics, controller pointer/laser interaction with the menu, hand or finger tracking.
- An on-device (ARM Linux/Android) build for the Steam Frame; only the PC-VR streaming path.
- The minor Phase 2 follow-ups (fence handoff flag, per-frame swapchain enumeration, dead getters, and so on) unless a milestone touches the code anyway.

## Constraints and current facts

- **No headset yet (2026-09-25).** The user does not own the Steam Frame. Acceptance is therefore split, per milestone, into what is verifiable without a headset and a headset-gated remainder. Headset-gated items stay explicit and unchecked until the hardware exists.
- **Target environment:** Windows 11, SteamVR's OpenXR runtime, Steam Frame streaming. Builds are cross-compiled on the dev LXC (`zig build -Dvr -Dtarget=x86_64-windows-gnu`) and served over the existing Samba share, as `roller.exe` already is.
- **Test environments:** this LXC and `fatal-gpu` (Monado simulated HMD, Xorg with DRI3), and the user's PC (SteamVR, optionally with its null virtual-headset driver, real keyboard and gamepad).
- **SDL fork wiring (commit `69886c1`):** the default build uses the original pinned `Zizin13/SDL#c8789580` (`.sdl`); the patched fork is the lazy `.sdl_vr` dependency, selected by `sdlDependency()` in `build.zig` only under `-Dvr`. `.sdl_vr` is still a host-local symlink (`external/sdl-vr-fork` to `/root/sdl-vr-fork`), so `-Dvr` builds work only on the dev LXC. Fork hosting is undecided and not blocked on by this phase, because all builds happen here.
- **Frame controller profile (verified from Valve's public source, [SteamFrameControllerProfile.cs](https://github.com/ValveSoftware/Unity/blob/main/com.valvesoftware.openxr.utils/Runtime/Interactions/SteamFrameControllerProfile.cs), not from summaries):** extension `XR_VALVE_frame_controller_interaction`, profile `/interaction_profiles/valve/frame_controller_valve`. Left hand: `dpad_up|right|down|left/click|touch`, `view/click|touch`. Right hand: `a|b|x|y/click|touch`, `menu/click|touch`. Both hands: `system/click|touch` (the runtime may reserve it), `squeeze/value|click|touch`, `shoulder/click|touch`, `trigger/value|click|touch`, `thumbstick` (vector2), `thumbstick/click|touch`, `grip/pose`, `aim/pose`, `/output/haptic`. The per-hand attribution comes from that package's doc comments and is unverified on hardware. The extension is not in the Khronos registry (`xr.xml`, OpenXR-Docs main, checked 2026-09-25) or the OpenXR headers on this LXC, so the strings are defined locally. Valve's own doc states that without the profile the controllers appear to applications as emulated Oculus Touch controllers.
- **Windows OpenXR loader:** the Khronos OpenXR-SDK `release-1.1.63` ships the Windows loader only as the NuGet package `OpenXR.Loader.1.1.63.nupkg`. Its contents were checked: x64 `openxr_loader.dll`, the import library `openxr_loader.lib`, and the 1.1.63 headers. This LXC's Debian headers are 1.1.47. No prebuilt `vulkan-1.lib` is available; it is generated from a `.def` file with `zig dlltool` (the real `vulkan-1.dll` comes with the GPU driver).
- **Upstream SDL:** the unreleased SDL 3.6 line (`main`) integrates OpenXR into SDL_GPU (`SDL_openxr.h`, `SDL_CreateGPUXRSession`, `SDL_PROP_GPU_DEVICE_CREATE_XR_*`). Releases up to 3.4.16 do not have it. 3.4.x has `SDL_GPUVulkanOptions` (extra instance/device extensions, API version). Only the headers were read; nothing was tried.

## Milestones (risk-first; each ends with a check on the user's PC where possible)

1. **M1: Windows/SteamVR bring-up and session lifecycle.** Attract race visible under SteamVR, correct startup and shutdown.
2. **M2: Head-tracked rendering.** Culling, sky and screen-space passes see the true head camera.
3. **M3: Input and audio.** Keyboard, gamepad and Frame controllers drive the game; sound works. The race is drivable.
4. **M4: Menus and HUD.** The full loop from title screen to finished race without leaving the headset.

The order puts the highest technical uncertainty (real-runtime contact, then visibility) first. M2 is testable on the LXC alone. Each milestone gets its own section in the implementation plan and its own review point.

## Units

New files under `PROJECTS/ROLLER/`, compiled only under `ROLLER_VR_BUILD`, so the default build is untouched:

- **`vr_quad.c` (new):** show a 2D frame as an OpenXR quad layer. Takes an indexed buffer, a palette and an optional transparent index, converts to RGBA on the CPU (frames are 640x400 at most), uploads into the quad's own swapchain, and submits an `XrCompositionLayerQuad` with a pose mode (fixed in front of the play space, or head-locked). Re-submits the last image every XR frame and uploads only when the game produced a new frame. Used twice: menu quad and HUD quad.
- **`vr_input.c` (new):** OpenXR actions and binding suggestions for three profiles, per-frame `xrSyncActions`, and writing the result into an SDL virtual gamepad. The mapping table lives in a separate file with no OpenXR dependency (`vr_input_map.c/.h`) so it can be unit-tested.
- **`vr_camera.c` (new):** composes the car's cockpit pose with the head pose into the game's camera globals, and supplies the VR frustum and the per-eye deltas.
- **`vr_runtime.c` (existing, 1264 lines):** gains the session state machine and the exit path into the game's quit sequence. The stereo render/copy code stays where it is; new functionality goes into new files, not into this one.
- **Game-side hooks** in shared files, all gated on `g_bVRMode`: the `InitSDL()` VR branch (audio, input, a desktop window), the `UpdateSDL()` VR path, the HUD buffer handoff, `InputRefreshDevices()`, and the camera/visibility sites found by the M2 investigation.

## M1: Windows/SteamVR bring-up and session lifecycle

### Windows build

- Stage the Khronos 1.1.63 NuGet contents so the existing `OPENXR_SDK_PATH` layout expectation holds (`include/`, `lib/openxr_loader.lib`); ship `openxr_loader.dll` next to the exe (the loader finds SteamVR through the registry's active runtime).
- Vulkan: headers from this LXC (`libvulkan-dev` 1.4.309) and a generated `vulkan-1.lib`.
- A staging script under `projects/fatal/` (outside the ROLLER repo) does this reproducibly; output goes to `builds/windows-vr/` with `midi/` and `audio/` as for the current Windows build.
- The earlier finding stands: with dummy SDK paths the cross-build compiled cleanly and failed only at link. This milestone replaces the dummies with real files.

### Vulkan instance/device extensions (Phase 2 open item, fixed here)

Phase 2 found that `vr_runtime.c` never enables the Vulkan extensions the OpenXR runtime asks for, because SDL creates the `VkInstance` (API 1.0) and `VkDevice` itself. The spec's valid usage requires them, and SteamVR is the runtime most likely to punish the gap.

- Add a small hook to the SDL fork to accept extra instance and device extensions and a newer API version, shaped like upstream 3.4's `SDL_GPUVulkanOptions` so a later move to 3.4 or 3.6 is cheap.
- `vr_runtime.c` calls `xrGetVulkanInstanceExtensionsKHR` and `xrGetVulkanDeviceExtensionsKHR` and passes the results in.
- Considered and rejected: `xrCreateVulkanInstanceKHR`/`xrCreateVulkanDeviceKHR`, because SDL owns creation.
- Measurable on the LXC: the validation messages `VUID-VkImageMemoryBarrier-None-09120` and `image-09118` (12 raw lines each in the Phase 2 300-frame run) must drop to zero on Monado.

### Session lifecycle (Phase 2 follow-ups 2 and 5)

- Full state machine: READY begins the session; SYNCHRONIZED, VISIBLE and FOCUSED are tracked (M3 gates input on FOCUSED); STOPPING ends the current frame and calls `xrEndSession`; EXITING and LOSS_PENDING request the game's own quit.
- A runtime-initiated end triggers the game's normal quit path (`quit_game`, `frontend_shutdown_finish()`, `ShutdownSDL()`, including config and save), instead of `vr_run_frame()` returning false and the process leaving the loop without teardown.
- A game-initiated quit calls `xrRequestExitSession` and drains to STOPPING before destroying, replacing today's shortcut (see the comment near `vr_shutdown()`).
- `xrWaitFrame`, `xrBeginFrame` and `xrEndFrame` failures take the same path and exit non-zero.
- `vr_init()` failure: message to a log file and, on Windows, a message box; non-zero exit (today `ErrorBoxExit()` exits 0).
- `roller-vr.log` next to the exe: runtime name, enabled extensions, session state transitions, view configuration, swapchain formats, and (from M2) frame times. This is the main diagnostic for test runs on the user's PC.

### Acceptance

Verifiable now:

- On the LXC (Monado, validation layers): no `09118`/`09120`; runtime-initiated end and game-initiated quit both exit cleanly with the save path executed.
- On the user's PC: the Windows build launches under SteamVR and shows the attract race. If the SteamVR null (virtual headset) driver works for OpenXR sessions, as is believed but not yet verified, this needs no headset. Quitting from SteamVR ends the process cleanly; `roller-vr.log` confirms the sequence.

Headset-gated: the same run in the real Steam Frame.

## M2: Head-tracked rendering

### What the code does today

The game's camera is a set of globals (`viewx/y/z`, `vk1..vk9`, `worlddirn`) written by `calculateview()` each frame in `draw_road()` (`3d.c`). Everything upstream of the eye MVPs reads them: track visibility (`CalcVisibleTrack`), cars (`DrawCars`), buildings (`CalcVisibleBuildings`), sky and horizon (`game_render_draw_sky`, `horizon.c`), screen-space quads (`project_to_ndc`) and the GPU quad frustum cull. Phase 2 applies the head pose only at the final per-eye MVP, so all of that still uses the flat car camera. Files that consume the camera globals include `transfrm.c`, `func3.c`, `drawtrk3.c`, `car.c`, `building.c`, `horizon.c` and `tower.c`.

Track visibility is not a frustum test. `CalcVisibleTrack()` draws one contiguous run of chunks, forward or backward from the nearest chunk, using precomputed per-chunk counts (`TrakView[].byForwardMainChunks/byBackwardMainChunks`), and chooses the direction by comparing the view direction with the track direction. A 90-degree sideways head turn needs parts of both runs.

### Design

1. **Cull once from a head camera, render per eye.** The car's cockpit pose composed with the head pose is written into the game camera globals before visibility runs. Culling uses a frustum wide enough to cover both eyes. Each eye renders with its own MVP, which differs from the head camera only by the IPD and eye rotation. Rejected: running the whole camera-to-queue path once per eye (twice the CPU preparation and game-side effects run twice, for no visible gain).
2. **Chunk selection in VR** picks the runs that intersect the head frustum, forward and backward where the view requires it.
3. **Sky/horizon and screen-space quads** use the per-eye view. Each eye's horizon plane comes from that eye's rotation instead of one 2D split in a 640x400 window. The GPU quad frustum cull uses the per-eye MVP or is skipped in VR (GPU clipping covers it).
4. **Building near-plane culling and car visibility** use the VR frustum instead of the 640x400 window extents.
5. **World scale.** The placeholder `VR_METERS_TO_WORLD = 100` is replaced by a value derived from the car model's real dimensions, with a config override. Parallax and cockpit feel depend on it; final tuning needs the headset.

### Investigation first (time-boxed)

The first task of this milestone inventories every consumer of the camera globals and confirms design points 2 to 4 before code changes. If the chunk model cannot be widened at acceptable cost, the fallback is to draw the whole track within draw distance in VR (higher cost, correct by construction). The choice is made on measured frame time.

The same task explains the Phase 2 screenshot anomaly: `screenshots/2026-09-23-vr-phase2-monado-stereo.png` (the final Phase 2 screenshot, after the fix wave) shows a view looking up at the sky with buildings along the bottom, large black polygons and a car floating in mid-air, and no track. The Phase 2 spec describes it as showing sky, track, walls and cars, which does not match. The two earlier screenshots from the same run (`vr-phase2-evidence/task-9-logs/task9_shot1.png`, `task9_shot3.png`) look right. Whether the last one is an unusual moment of the attract replay or a regression from the fix wave is unknown.

### Testing without a headset

- `--vr-fake-head`: a scripted sweep of yaw, pitch, roll and position offsets replaces the runtime head pose.
- Per frame log of how many chunks, cars, buildings and quads were submitted; CPU and GPU frame times in the log.
- Eye images dumped to PNG; the sweep covers yaw 0, 90, 180 and 270 degrees. The images are reviewed by looking at them, not only by numbers.

### Acceptance

Verifiable now: the sweep shows no geometry popping or vanishing, consistent counts, a level horizon in the reviewed images, and the screenshot anomaly explained. On the user's PC (null driver): the same runs, plus frame times from the log.

Headset-gated: a real 360-degree head turn in the attract race with a level horizon and nothing vanishing; frame times against the Frame's refresh rate; world-scale and comfort tuning.

## M3: Input and audio

### Input sources

All feed the game's existing input layer (`rollerinput.c`: 14 logical actions bound to keyboard keys or joystick/gamepad buttons, axes and hats). That file changes only in `InputRefreshDevices()`.

- **Keyboard:** the `InitSDL()` VR branch creates a small visible desktop window that is never claimed by the GPU device, solely so SDL delivers keyboard events. Its title says to click it for keyboard focus. It is raised at start and whenever the session becomes FOCUSED. Limitation: keyboard works only while that window has focus.
- **Physical gamepad or wheel:** unchanged; `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` is set so they keep working while SteamVR has focus.
- **Frame controllers:** `vr_input.c` reads OpenXR actions and writes them into an SDL virtual gamepad with the standard layout. The pinned SDL (3.2.22) has the virtual joystick API (`SDL_VirtualJoystickDesc`, attach, set axis and button). That SDL maps a gamepad-type virtual joystick to the standard gamepad mapping is expected but must be verified at the start of this milestone. Being a standard gamepad, the game's default gamepad bindings, menu navigation and rebinding screen work unchanged.

### The Windows input backend

The default Windows backend is SDL DirectInput, but the config can select WinMM, whose device list never contains SDL virtual devices. In VR mode the SDL joystick subsystem is therefore always initialised and the virtual pad is always opened in addition to whatever the chosen backend finds (a few lines in `InputRefreshDevices()`, gated on the VR build). WinMM wheels keep working next to the Frame controllers. The virtual pad is attached before `InputInit()` so the first device scan includes it.

### Frame controller mapping

Bindings are suggested for the Valve `frame_controller_valve` profile, Oculus Touch and the simple-controller profile together; the runtime picks one. `XR_VALVE_frame_controller_interaction` is enabled only if the runtime advertises it (requesting an unavailable extension fails instance creation).

- Thumbsticks map to the two sticks, triggers to the triggers, shoulder and squeeze to bumpers or stick clicks.
- Right hand: A, B, X, Y map to the face buttons; `menu` maps to Start. Left hand: the d-pad (Valve profile only) maps to the d-pad; `view` maps to Back. `system` is left to the runtime.
- On the Touch profile there is no d-pad; menus use the stick.
- When the session is not FOCUSED (for example the SteamVR dashboard is open) all inputs read zero.
- Safety exit: holding Back and Start for 2 seconds requests a clean quit on any screen. Released before that threshold it recenters the menu (menu states only).
- The mapping table is pure C, unit-tested with simulated action states, and wired into `zig build test`.

### Audio

The VR branch stops skipping audio: `SDL_INIT_AUDIO`, the `ROLLERGetAudioInfo()` probe, and the same WildMIDI, OS MIDI and OPL3 init as the normal path. The MIDI config and patches ship next to the exe, as they do for the current Windows build.

### Testing and acceptance

- **LXC:** `--vr-fake-input` injects a scripted virtual-pad sequence (menu navigation, accelerate, steer, quit); the log must show the car moving and menu states changing. Audio runs on SDL's dummy driver and the init logs are checked. The mapping unit tests pass.
- **User's PC, no headset:** a real keyboard and a real gamepad or wheel drive a race with sound.
- **Headset-gated:** Frame controller feel and the Valve profile check on hardware, behaviour when the dashboard opens, audio routing while streaming.

## M4: Menus and HUD

`vr_quad` serves both. The menu frame and the HUD buffer are both 8-bit indexed buffers with a palette, and the game already converts them to RGBA (`ConvertIndexedToRGBA` in `roller.c`, `indexed_to_rgba` in `scene_render_gpu.c`), so one path covers both.

- **Menu quad:** every non-race frontend state (title, menus, results, the intro sequences, the configure and rebind screens). About 2 m ahead at eye height and about 2.3 m wide to match the 16:10 frame, placed in front of the head direction whenever a menu state begins. With no projection layer submitted, the runtime shows its neutral background.
- **HUD quad:** today the race path returns before the HUD is composited (`vr_present_stereo_frame()` is called from `updatescreen()`). The HUD buffer goes to a head-locked quad, roughly 1.5 m away, low in the view and slightly transparent. Position, size and distance are config values; tuning needs the headset. The first task confirms exactly where the game holds the HUD buffer and which palette index is transparent (`scene_render_gpu_set_hud_buffer()`, `hudSrcBuf`).
- **Rejected:** drawing the menu as a textured quad inside the stereo scene (same look, but through our per-eye renderer, and it gives up the runtime's quad-layer sampling for no benefit).
- **Loading screens** block the game thread, so no XR frames would be submitted and the headset would freeze during track loads. The plan needs a keepalive frame submission during loading; whether the loader has usable hook points is unknown (risk 3 below).

### Acceptance

Verifiable now: menu and HUD quad images dumped and reviewed; menu navigation via `--vr-fake-input` from the title screen into a race and back; on the user's PC (null driver window) the menu and HUD are visible and a full loop title, race, quit works with real input. Headset-gated: legibility, HUD placement and size, comfort.

## Testing strategy summary

| Tier | Where | What |
| --- | --- | --- |
| 1 | LXC / `fatal-gpu`, Monado sim HMD | Validation layers clean, fake-head sweeps, scripted fake input, PNG dumps of eyes and quads reviewed by looking, cull and frame-time logs, unit tests |
| 2 | User's PC, no headset | SteamVR null driver for the Windows path, real keyboard and gamepad, a driven race with sound, `roller-vr.log` |
| 3 | Headset-gated | Feel, comfort, scale tuning, Frame controllers, frame times while streaming |

As in Phases 1 and 2: new OpenXR and Vulkan calls are written against the spec's valid-usage sections and the Khronos `hello_xr` sample, not from memory, and development runs use `XR_APILAYER_LUNARG_core_validation`.

## Risks and open items

1. **Track visibility widening (M2)** may cost more than expected; fallback is the whole-track draw.
2. **First contact with SteamVR (M1)** may expose Vulkan extension or loader problems. Whether the SteamVR null driver works for OpenXR sessions is believed but unverified; it is the first thing to check on the PC.
3. **Loading screens** block the frame loop (see M4). Hook points unknown.
4. **Keyboard focus** works only while the mirror window has focus.
5. **Frame controllers** are implemented to Valve's public spec but unverified until hardware exists. Keyboard and gamepad are the verified input paths.
6. **The unexplained Phase 2 screenshot** (see M2).
7. **SDL fork hosting:** `-Dvr` builds work only where `/root/sdl-vr-fork` exists; fine while all builds happen on the dev LXC, a blocker for anything else. The unreleased SDL 3.6 OpenXR integration would replace the fork and the extension hook; not usable until released.
8. **Seated recentering:** the play-space origin is set at session start. Whether a recenter action is needed for seated racing is a headset-gated question.
9. **Auto pause** when the session leaves FOCUSED during a race (dashboard open): input reads zero; whether to also pause the game is decided with the headset.
10. **Overall design spec:** its phase list (comfort as Phase 3, controllers and menus as Phase 4) is superseded by this spec for menus, controllers and head-tracked culling and should be updated when this phase lands.
