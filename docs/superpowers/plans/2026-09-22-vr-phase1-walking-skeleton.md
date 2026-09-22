# ROLLER VR Phase 1: Walking Skeleton Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove OpenXR can render a solid color to both eyes through the same Vulkan/D3D12 device SDL_GPU already owns, with zero changes to ROLLER's gameplay/rendering code — the walking-skeleton validation described as Phase 1 of the VR design spec.

**Architecture:** Patch the vendored SDL fork so `SDL_GPUDevice` exposes its internal Vulkan (and D3D12) device handles as queryable properties, mirroring the pattern `SDL_Renderer` already uses. A new, small, standalone executable (gated behind a `-Dvr` build option, entirely separate from `roller.c`'s game loop) creates an `SDL_GPUDevice`, pulls those handles, opens an OpenXR session against them via `XR_KHR_vulkan_enable2`, and clears both eye swapchain images to a solid color using raw Vulkan calls for a fixed number of frames before a clean shutdown.

**Refinement from the spec:** the spec's Phase 1 description mentioned using ROLLER's existing offscreen windowless GPU renderer (`scene_render_gpu_create_windowless`) as the eye-texture target. That renderer only produces `SDL_GPUTexture*` handles, and SDL_GPU has no public API to wrap an OpenXR-provided `VkImage` as an `SDL_GPUTexture` (SDL_GPU only creates its own textures internally) — there's no clean bridge between the two for a plain color clear. Since Phase 1's whole point is proving the plumbing *before* touching game rendering, this plan clears the OpenXR swapchain images directly with raw Vulkan commands, using the same `VkDevice`/`VkQueue` the SDL patch exposes. Wiring the real scene renderer into an eye texture is Phase 2's job (a separate future plan), once Phase 1 has proven the session/swapchain plumbing works at all.

**Tech Stack:** C · Zig 0.15.2 (`build.zig`) · SDL3 (`Zizin13/SDL` fork, patched) · Vulkan 1.4 · OpenXR 1.1 (`libopenxr-dev`/`libopenxr-loader1`, system package) · Monado (`libopenxr1-monado`/`monado-service`, system package) for runtime validation

**Spec:** `docs/superpowers/specs/2026-09-22-roller-vr-design.md`

## Global Constraints

- No changes to `roller.c`, `scene_render_gpu.c`, or any other existing game-rendering file. The new VR skeleton is a fully separate executable target.
- The SDL fork patch must not change behavior for non-VR builds — untouched fields/functions are additive only.
- All new build-time code (OpenXR linking, the new executable target) must be gated behind a new `-Dvr` boolean build option, off by default.
- Target platforms: Linux x86_64 (real end-to-end runtime validation on the `fatal-gpu` host, 192.168.14.92, `ssh fatal-gpu`) and Windows x86_64 via cross-compile (compile-only validation in this plan — no Windows GPU rig available yet; runtime validation with real SteamVR is a manual follow-up for the user).
- SDL fork source lives locally at `/root/.cache/zig/p/sdl-0.3.0+3.2.22-7uIn9Is4fwGyNwHnYNTLnZP1WBhWxa5V2WS34x9X_jm8` (already fetched by a prior `zig build`) — patch it there, then move the patched tree to a real local git clone (Task 1, Step 1) so it can be committed and pointed to from `build.zig.zon`.

---

## File Structure

**Patched SDL fork** (new local git repo, cloned from `Zizin13/SDL`):
- `src/gpu/SDL_sysgpu.h` — add `SDL_PropertiesID props;` field to `struct SDL_GPUDevice`
- `src/gpu/SDL_gpu.c` — add `SDL_GetGPUDeviceProperties()`, add cleanup call in `SDL_DestroyGPUDevice()`
- `include/SDL3/SDL_gpu.h` — declare `SDL_GetGPUDeviceProperties()` + new property name macros
- `src/gpu/vulkan/SDL_gpu_vulkan.c` — populate Vulkan properties in `VULKAN_CreateDevice()`
- `src/gpu/d3d12/SDL_gpu_d3d12.c` — populate D3D12 properties in `D3D12_CreateDevice()`

**ROLLER (this repo, `roller-vr` branch):**
- `build.zig.zon` — point the `sdl` dependency at the patched fork
- `build.zig` — add `-Dvr` option; when set, add OpenXR system library linking and a new `vr_skeleton` executable target
- `PROJECTS/ROLLER/vr_skeleton_main.c` — new file, the walking-skeleton executable's entire implementation

**GPU test host (`fatal-gpu`, not part of this repo):**
- Monado + OpenXR loader packages (already installed and verified during planning — see Task 6)

---

### Task 1: Set up a local patchable clone of the SDL fork

**Files:**
- Create: `/root/sdl-vr-fork/` (new local git clone, outside the ROLLER repo — this becomes its own git history)

**Interfaces:**
- Produces: a local git repo at `/root/sdl-vr-fork` on a new branch `vr-gpu-properties`, ready for Tasks 2–3 to commit patches into.

- [ ] **Step 1: Clone the fork**

```bash
cd /root
git clone https://github.com/Zizin13/SDL.git sdl-vr-fork
cd sdl-vr-fork
git log --oneline -1
```

Expected: clones successfully; note the current HEAD commit hash (this should match the commit ROLLER's `build.zig.zon` currently pins — `c8789580c0f4f0b1bdc268ab5c52082864f3e2a8` — confirm with `git log --oneline | grep c878958`).

- [ ] **Step 2: Create the working branch**

```bash
cd /root/sdl-vr-fork
git checkout -b vr-gpu-properties
```

- [ ] **Step 3: Verify it's the same source already cached by Zig**

```bash
diff -rq /root/sdl-vr-fork/src/gpu/vulkan/SDL_gpu_vulkan.c \
  "/root/.cache/zig/p/sdl-0.3.0+3.2.22-7uIn9Is4fwGyNwHnYNTLnZP1WBhWxa5V2WS34x9X_jm8/src/gpu/vulkan/SDL_gpu_vulkan.c"
```

Expected: no output (files identical). If they differ, the fork has moved since this plan was written — re-read `struct VulkanRenderer` in the fresh clone before proceeding to Task 2, since the field names this plan relies on (`instance`, `physicalDevice`, `logicalDevice`, `unifiedQueue`, `queueFamilyIndex`) may have changed.

---

### Task 2: Patch SDL_GPU to expose device properties (shared struct + public API)

**Files:**
- Modify: `/root/sdl-vr-fork/src/gpu/SDL_sysgpu.h`
- Modify: `/root/sdl-vr-fork/include/SDL3/SDL_gpu.h`
- Modify: `/root/sdl-vr-fork/src/gpu/SDL_gpu.c`

**Interfaces:**
- Produces: `SDL_PropertiesID SDL_GetGPUDeviceProperties(SDL_GPUDevice *device)` — new public function, callable from Task 5's skeleton. Property name macros: `SDL_PROP_GPU_DEVICE_VULKAN_INSTANCE_POINTER`, `SDL_PROP_GPU_DEVICE_VULKAN_PHYSICAL_DEVICE_POINTER`, `SDL_PROP_GPU_DEVICE_VULKAN_DEVICE_POINTER`, `SDL_PROP_GPU_DEVICE_VULKAN_QUEUE_POINTER`, `SDL_PROP_GPU_DEVICE_VULKAN_QUEUE_FAMILY_INDEX_NUMBER`, `SDL_PROP_GPU_DEVICE_D3D12_DEVICE_POINTER`, `SDL_PROP_GPU_DEVICE_D3D12_COMMAND_QUEUE_POINTER`.

- [ ] **Step 1: Add the `props` field to `struct SDL_GPUDevice`**

In `src/gpu/SDL_sysgpu.h`, find `struct SDL_GPUDevice` (starts around line 518, right after `typedef struct SDL_GPURenderer SDL_GPURenderer;`). Add one field anywhere inside the struct body, e.g. right after the opening brace:

```c
struct SDL_GPUDevice
{
    // Properties for interop (e.g. exposing native Vulkan/D3D12 handles)
    SDL_PropertiesID props;

    // Quit

    void (*DestroyDevice)(SDL_GPUDevice *device);
    // ... rest of struct unchanged
```

- [ ] **Step 2: Declare the new public function and property macros**

In `include/SDL3/SDL_gpu.h`, find `SDL_GetGPUDeviceDriver` (around line 2240) and add the new declaration right after it:

```c
extern SDL_DECLSPEC const char * SDLCALL SDL_GetGPUDeviceDriver(SDL_GPUDevice *device);

/**
 * Get the properties associated with a GPU device.
 *
 * On the Vulkan backend, the following properties are set:
 *
 * - `SDL_PROP_GPU_DEVICE_VULKAN_INSTANCE_POINTER`: the VkInstance associated
 *   with the device.
 * - `SDL_PROP_GPU_DEVICE_VULKAN_PHYSICAL_DEVICE_POINTER`: the
 *   VkPhysicalDevice associated with the device.
 * - `SDL_PROP_GPU_DEVICE_VULKAN_DEVICE_POINTER`: the VkDevice associated
 *   with the device.
 * - `SDL_PROP_GPU_DEVICE_VULKAN_QUEUE_POINTER`: the VkQueue used for
 *   submission.
 * - `SDL_PROP_GPU_DEVICE_VULKAN_QUEUE_FAMILY_INDEX_NUMBER`: the queue
 *   family index of that queue.
 *
 * On the D3D12 backend, the following properties are set:
 *
 * - `SDL_PROP_GPU_DEVICE_D3D12_DEVICE_POINTER`: the ID3D12Device associated
 *   with the device.
 * - `SDL_PROP_GPU_DEVICE_D3D12_COMMAND_QUEUE_POINTER`: the
 *   ID3D12CommandQueue used for submission.
 *
 * These exist so an external graphics API consumer (e.g. an OpenXR runtime
 * via XR_KHR_vulkan_enable2 / XR_KHR_D3D12_enable) can bind to the same
 * device SDL_GPU owns, instead of creating a second device.
 *
 * \param device a GPU context to query.
 * \returns a valid property ID on success or 0 on failure; call
 *          SDL_GetError() for more information.
 *
 * \since This function is available since SDL 3.4.0.
 */
extern SDL_DECLSPEC SDL_PropertiesID SDLCALL SDL_GetGPUDeviceProperties(SDL_GPUDevice *device);

#define SDL_PROP_GPU_DEVICE_VULKAN_INSTANCE_POINTER              "SDL.gpu.device.vulkan.instance"
#define SDL_PROP_GPU_DEVICE_VULKAN_PHYSICAL_DEVICE_POINTER       "SDL.gpu.device.vulkan.physical_device"
#define SDL_PROP_GPU_DEVICE_VULKAN_DEVICE_POINTER                "SDL.gpu.device.vulkan.device"
#define SDL_PROP_GPU_DEVICE_VULKAN_QUEUE_POINTER                 "SDL.gpu.device.vulkan.queue"
#define SDL_PROP_GPU_DEVICE_VULKAN_QUEUE_FAMILY_INDEX_NUMBER     "SDL.gpu.device.vulkan.queue_family_index"
#define SDL_PROP_GPU_DEVICE_D3D12_DEVICE_POINTER                 "SDL.gpu.device.d3d12.device"
#define SDL_PROP_GPU_DEVICE_D3D12_COMMAND_QUEUE_POINTER          "SDL.gpu.device.d3d12.command_queue"
```

- [ ] **Step 3: Implement the getter and destroy-time cleanup**

In `src/gpu/SDL_gpu.c`, find `SDL_GetGPUDeviceDriver` (around line 787) and add the new function right after it, mirroring `SDL_GetRendererProperties`'s lazy-create pattern from `src/render/SDL_render.c:1210`:

```c
const char * SDL_GetGPUDeviceDriver(SDL_GPUDevice *device)
{
    CHECK_DEVICE_MAGIC(device, NULL);

    return device->backend;
}

SDL_PropertiesID SDL_GetGPUDeviceProperties(SDL_GPUDevice *device)
{
    CHECK_DEVICE_MAGIC(device, 0);

    if (device->props == 0) {
        device->props = SDL_CreateProperties();
    }
    return device->props;
}
```

Then find `SDL_DestroyGPUDevice` (around line 758) and add cleanup before it calls into the backend's own destroy function (which frees the `SDL_GPUDevice` struct itself):

```c
void SDL_DestroyGPUDevice(SDL_GPUDevice *device)
{
    CHECK_DEVICE_MAGIC(device, );

    if (device->props) {
        SDL_DestroyProperties(device->props);
    }
    device->DestroyDevice(device);
}
```

- [ ] **Step 4: Commit**

```bash
cd /root/sdl-vr-fork
git add src/gpu/SDL_sysgpu.h include/SDL3/SDL_gpu.h src/gpu/SDL_gpu.c
git commit -m "gpu: add SDL_GetGPUDeviceProperties for native device handle interop"
```

---

### Task 3: Populate the properties from the Vulkan and D3D12 backends

**Files:**
- Modify: `/root/sdl-vr-fork/src/gpu/vulkan/SDL_gpu_vulkan.c`
- Modify: `/root/sdl-vr-fork/src/gpu/d3d12/SDL_gpu_d3d12.c`

**Interfaces:**
- Consumes: `struct VulkanRenderer` fields `instance` (VkInstance), `physicalDevice` (VkPhysicalDevice), `logicalDevice` (VkDevice), `unifiedQueue` (VkQueue), `queueFamilyIndex` (Uint32) — all confirmed present in `struct VulkanRenderer` (`SDL_gpu_vulkan.c:1127`). `struct D3D12Renderer` fields `device` (ID3D12Device*), `commandQueue` (ID3D12CommandQueue*) — confirmed present in `struct D3D12Renderer` (`SDL_gpu_d3d12.c:859`).
- Consumes: property macros and `device->props` field from Task 2.
- Produces: a fully working `SDL_GetGPUDeviceProperties()` for both backends — the deliverable Task 4 and Task 5 depend on.

- [ ] **Step 1: Populate Vulkan properties**

In `src/gpu/vulkan/SDL_gpu_vulkan.c`, find the end of `VULKAN_CreateDevice` where the result struct is finalized (around line 11766-11768):

```c
    result = (SDL_GPUDevice *)SDL_malloc(sizeof(SDL_GPUDevice));
    ASSIGN_DRIVER(VULKAN)

    result->driverData = (SDL_GPURenderer *)renderer;
    result->shader_formats = SDL_GPU_SHADERFORMAT_SPIRV;
```

Change it to (two additions: zero `props` since `SDL_malloc` doesn't zero memory, then populate the properties):

```c
    result = (SDL_GPUDevice *)SDL_malloc(sizeof(SDL_GPUDevice));
    ASSIGN_DRIVER(VULKAN)

    result->driverData = (SDL_GPURenderer *)renderer;
    result->shader_formats = SDL_GPU_SHADERFORMAT_SPIRV;
    result->props = 0;

    {
        SDL_PropertiesID vr_props = SDL_GetGPUDeviceProperties(result);
        SDL_SetPointerProperty(vr_props, SDL_PROP_GPU_DEVICE_VULKAN_INSTANCE_POINTER, renderer->instance);
        SDL_SetPointerProperty(vr_props, SDL_PROP_GPU_DEVICE_VULKAN_PHYSICAL_DEVICE_POINTER, renderer->physicalDevice);
        SDL_SetPointerProperty(vr_props, SDL_PROP_GPU_DEVICE_VULKAN_DEVICE_POINTER, renderer->logicalDevice);
        SDL_SetPointerProperty(vr_props, SDL_PROP_GPU_DEVICE_VULKAN_QUEUE_POINTER, renderer->unifiedQueue);
        SDL_SetNumberProperty(vr_props, SDL_PROP_GPU_DEVICE_VULKAN_QUEUE_FAMILY_INDEX_NUMBER, renderer->queueFamilyIndex);
    }
```

- [ ] **Step 2: Populate D3D12 properties**

In `src/gpu/d3d12/SDL_gpu_d3d12.c`, find the end of `D3D12_CreateDevice` (around line 9229-9233):

```c
    ASSIGN_DRIVER(D3D12)
    result->driverData = (SDL_GPURenderer *)renderer;
    result->shader_formats = shaderFormats;
    result->debug_mode = debugMode;
    renderer->sdlGPUDevice = result;

    return result;
```

Change it to (note: this backend already uses `SDL_calloc`, so `result->props` is already 0 — no explicit zeroing needed here, unlike the Vulkan backend):

```c
    ASSIGN_DRIVER(D3D12)
    result->driverData = (SDL_GPURenderer *)renderer;
    result->shader_formats = shaderFormats;
    result->debug_mode = debugMode;
    renderer->sdlGPUDevice = result;

    {
        SDL_PropertiesID vr_props = SDL_GetGPUDeviceProperties(result);
        SDL_SetPointerProperty(vr_props, SDL_PROP_GPU_DEVICE_D3D12_DEVICE_POINTER, renderer->device);
        SDL_SetPointerProperty(vr_props, SDL_PROP_GPU_DEVICE_D3D12_COMMAND_QUEUE_POINTER, renderer->commandQueue);
    }

    return result;
```

- [ ] **Step 3: Commit**

```bash
cd /root/sdl-vr-fork
git add src/gpu/vulkan/SDL_gpu_vulkan.c src/gpu/d3d12/SDL_gpu_d3d12.c
git commit -m "gpu: populate device properties from Vulkan and D3D12 backends"
```

- [ ] **Step 4: Push the branch somewhere ROLLER's build can reach**

This needs a real git remote. Since there's no existing fork-of-the-fork under your control yet, the simplest path: push to a new repo you create (e.g. `git@github.com:<your-account>/SDL.git`), or, if you'd rather not publish it yet, reference it locally first (Task 4 supports both — see its Step 1 note).

```bash
cd /root/sdl-vr-fork
git remote add vr-fork <your-remote-url>   # fill in once you've created it
git push vr-fork vr-gpu-properties
```

If you don't have a remote ready yet, skip this step and use the local-path variant in Task 4 Step 1 instead — you can switch to the pushed remote later without changing anything else in this plan.

---

### Task 4: Point ROLLER's build at the patched fork and add the `-Dvr` option

**Files:**
- Modify: `build.zig.zon`
- Modify: `build.zig`

**Interfaces:**
- Consumes: the patched SDL fork from Task 3 (either local path or pushed branch).
- Produces: a `-Dvr` boolean build option; when true, `build.zig` links `openxr_loader` and builds the new `vr_skeleton` executable (added in Task 5).

- [ ] **Step 1: Point `build.zig.zon` at the patched fork**

Read the current `sdl` entry in `build.zig.zon` first:

```bash
grep -A2 '.sdl = ' /config/projects/fatal/ROLLER/build.zig.zon
```

It currently reads:

```
.sdl = .{
    .url = "git+https://github.com/Zizin13/SDL#c8789580c0f4f0b1bdc268ab5c52082864f3e2a8",
    .hash = "sdl-0.3.0+3.2.22-7uIn9Is4fwGyNwHnYNTLnZP1WBhWxa5V2WS34x9X_jm8",
},
```

If Task 3 Step 4 was completed (branch pushed to a real remote), replace the `.url` with that remote + the new commit hash, and update `.hash` by running `zig build` once and copying the hash Zig reports in its error message (Zig verifies package hashes; it will tell you the correct value the first time it fetches an unrecognized URL). If you skipped pushing, use a local path dependency instead:

```
.sdl = .{
    .path = "/root/sdl-vr-fork",
},
```

(A path dependency has no `.hash` field — Zig doesn't hash local paths.)

- [ ] **Step 2: Add the `-Dvr` option and OpenXR linking**

In `build.zig`, find where `crash_debug` is declared (near the top, alongside the other `b.option` calls — see the existing `crash_debug` block for the pattern) and add a new option right after it:

```zig
    const build_vr = b.option(bool, "vr", "Build the VR walking-skeleton executable (Linux/Windows only, requires OpenXR dev packages)") orelse false;
```

Then, after the main `exe` target is fully configured (after the `switch (target.result.os.tag)` block that handles Windows/Android/other linking, and after `b.installArtifact(exe);` if that's where the main executable install happens — find it by searching for `installArtifact` in the file), add:

```zig
    if (build_vr) {
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
        vr_exe.linkLibrary(sdl.artifact("SDL3"));
        switch (target.result.os.tag) {
            .windows => {
                // OpenXR SDK loader .lib is expected at OPENXR_SDK_PATH\lib\openxr_loader.lib
                // and headers at OPENXR_SDK_PATH\include, matching the official Khronos
                // OpenXR-SDK release layout. This is a manual dependency for now (no Zig
                // package pinned yet) — set the env var before building with -Dvr on Windows.
                const openxr_sdk_path = std.process.getEnvVarOwned(b.allocator, "OPENXR_SDK_PATH") catch @panic("Set OPENXR_SDK_PATH to an extracted OpenXR-SDK release to build with -Dvr on Windows");
                vr_exe_mod.addIncludePath(.{ .cwd_relative = b.pathJoin(&.{ openxr_sdk_path, "include" }) });
                vr_exe_mod.addLibraryPath(.{ .cwd_relative = b.pathJoin(&.{ openxr_sdk_path, "lib" }) });
                vr_exe.linkSystemLibrary("openxr_loader");
            },
            else => {
                // Linux: system package (confirmed present on the fatal-gpu test host as
                // libopenxr-dev / libopenxr-loader1, headers at /usr/include/openxr/).
                vr_exe.linkSystemLibrary("openxr_loader");
            },
        }
        b.installArtifact(vr_exe);
    }
```

- [ ] **Step 3: Verify the non-VR build is unaffected**

```bash
cd /config/projects/fatal/ROLLER
zig build 2>&1 | tail -20
```

Expected: builds exactly as before (this is a regression check — the `-Dvr` option defaults to `false`, so this path must be untouched). Compare against the last known-good build summary (20/20 steps succeeded) from the earlier session work on this project.

- [ ] **Step 4: Commit**

```bash
git add build.zig build.zig.zon
git commit -m "vr: add -Dvr build option, point sdl dependency at patched fork"
```

(Do not push yet — Task 5 adds the file this build depends on; push once both are in and Task 6 has verified the whole thing runs.)

---

### Task 5: Write the walking-skeleton executable

**Files:**
- Create: `PROJECTS/ROLLER/vr_skeleton_main.c`

**Interfaces:**
- Consumes: `SDL_GetGPUDeviceProperties()` and the property macros from Task 2/3. `SDL_CreateGPUDevice`, `SDL_DestroyGPUDevice` (existing public SDL_GPU API, unchanged).
- Produces: a standalone `main()` — no other file calls into this one. Exit code 0 on success (N frames rendered and session torn down cleanly), non-zero with a printed error on any OpenXR/Vulkan failure.

- [ ] **Step 1: Write the skeleton**

```c
/* PROJECTS/ROLLER/vr_skeleton_main.c
 *
 * VR Phase 1 walking skeleton: proves OpenXR can open a session against the
 * same Vulkan device SDL_GPU owns, and render a solid color to both eyes.
 * Deliberately does not touch any ROLLER game/rendering code — see the
 * "Refinement from the spec" note in the implementation plan this came from.
 */
#include <SDL3/SDL.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_COUNT_TO_RUN 120

static void fail(const char *what)
{
    fprintf(stderr, "vr_skeleton: %s\n", what);
    exit(1);
}

static void fail_xr(const char *what, XrResult result)
{
    fprintf(stderr, "vr_skeleton: %s (XrResult=%d)\n", what, (int)result);
    exit(1);
}

static void fail_vk(const char *what, VkResult result)
{
    fprintf(stderr, "vr_skeleton: %s (VkResult=%d)\n", what, (int)result);
    exit(1);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* --- 1. Create the SDL_GPU device (Vulkan) --- */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fail(SDL_GetError());
    }

    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
    if (!gpu) {
        fail(SDL_GetError());
    }
    printf("vr_skeleton: SDL_GPU driver: %s\n", SDL_GetGPUDeviceDriver(gpu));

    SDL_PropertiesID gpu_props = SDL_GetGPUDeviceProperties(gpu);
    if (!gpu_props) {
        fail("SDL_GetGPUDeviceProperties failed");
    }

    VkInstance vk_instance = (VkInstance)SDL_GetPointerProperty(gpu_props, SDL_PROP_GPU_DEVICE_VULKAN_INSTANCE_POINTER, NULL);
    VkPhysicalDevice vk_physical_device = (VkPhysicalDevice)SDL_GetPointerProperty(gpu_props, SDL_PROP_GPU_DEVICE_VULKAN_PHYSICAL_DEVICE_POINTER, NULL);
    VkDevice vk_device = (VkDevice)SDL_GetPointerProperty(gpu_props, SDL_PROP_GPU_DEVICE_VULKAN_DEVICE_POINTER, NULL);
    VkQueue vk_queue = (VkQueue)SDL_GetPointerProperty(gpu_props, SDL_PROP_GPU_DEVICE_VULKAN_QUEUE_POINTER, NULL);
    Uint32 vk_queue_family_index = (Uint32)SDL_GetNumberProperty(gpu_props, SDL_PROP_GPU_DEVICE_VULKAN_QUEUE_FAMILY_INDEX_NUMBER, 0xFFFFFFFF);

    if (!vk_instance || !vk_physical_device || !vk_device || !vk_queue || vk_queue_family_index == 0xFFFFFFFF) {
        fail("SDL_GPU did not expose Vulkan device properties (is this the patched SDL fork? is the active backend Vulkan?)");
    }
    printf("vr_skeleton: got VkInstance=%p VkDevice=%p queueFamilyIndex=%u\n",
           (void *)vk_instance, (void *)vk_device, vk_queue_family_index);

    /* --- 2. Create the OpenXR instance --- */
    const char *enabled_extensions[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };

    XrInstanceCreateInfo instance_create_info = { .type = XR_TYPE_INSTANCE_CREATE_INFO };
    strncpy(instance_create_info.applicationInfo.applicationName, "ROLLER VR Skeleton",
            XR_MAX_APPLICATION_NAME_SIZE - 1);
    instance_create_info.applicationInfo.applicationVersion = 1;
    instance_create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    instance_create_info.enabledExtensionCount = 1;
    instance_create_info.enabledExtensionNames = enabled_extensions;

    XrInstance instance = XR_NULL_HANDLE;
    XrResult xr_result = xrCreateInstance(&instance_create_info, &instance);
    if (XR_FAILED(xr_result)) {
        fail_xr("xrCreateInstance failed", xr_result);
    }

    /* --- 3. Get the system (the "head" device — the Simulated HMD when no real headset is attached) --- */
    XrSystemGetInfo system_get_info = {
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };
    XrSystemId system_id;
    xr_result = xrGetSystem(instance, &system_get_info, &system_id);
    if (XR_FAILED(xr_result)) {
        fail_xr("xrGetSystem failed (no HMD available from the active OpenXR runtime?)", xr_result);
    }

    /* Required by the OpenXR spec before creating a Vulkan-backed session,
     * even though we already have our own device and don't act on the
     * returned version range here. */
    PFN_xrGetVulkanGraphicsRequirements2KHR pfn_get_vk_requirements = NULL;
    xr_result = xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirements2KHR",
                                       (PFN_xrVoidFunction *)&pfn_get_vk_requirements);
    if (XR_FAILED(xr_result) || !pfn_get_vk_requirements) {
        fail_xr("failed to load xrGetVulkanGraphicsRequirements2KHR", xr_result);
    }
    XrGraphicsRequirementsVulkan2KHR vk_requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    xr_result = pfn_get_vk_requirements(instance, system_id, &vk_requirements);
    if (XR_FAILED(xr_result)) {
        fail_xr("xrGetVulkanGraphicsRequirements2KHR failed", xr_result);
    }

    /* --- 4. Create the session against our existing Vulkan device --- */
    XrGraphicsBindingVulkan2KHR vk_binding = {
        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .instance = vk_instance,
        .physicalDevice = vk_physical_device,
        .device = vk_device,
        .queueFamilyIndex = vk_queue_family_index,
        .queueIndex = 0,
    };
    XrSessionCreateInfo session_create_info = {
        .type = XR_TYPE_SESSION_CREATE_INFO,
        .next = &vk_binding,
        .systemId = system_id,
    };
    XrSession session = XR_NULL_HANDLE;
    xr_result = xrCreateSession(instance, &session_create_info, &session);
    if (XR_FAILED(xr_result)) {
        fail_xr("xrCreateSession failed", xr_result);
    }
    printf("vr_skeleton: OpenXR session created\n");

    /* --- 5. Enumerate the view configuration (expect 2 views for stereo) --- */
    Uint32 view_count = 0;
    xr_result = xrEnumerateViewConfigurationViews(instance, system_id,
                                                   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                   0, &view_count, NULL);
    if (XR_FAILED(xr_result) || view_count == 0) {
        fail_xr("xrEnumerateViewConfigurationViews (count) failed", xr_result);
    }
    XrViewConfigurationView *view_configs = calloc(view_count, sizeof(XrViewConfigurationView));
    for (Uint32 i = 0; i < view_count; i++) {
        view_configs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    }
    xr_result = xrEnumerateViewConfigurationViews(instance, system_id,
                                                   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                   view_count, &view_count, view_configs);
    if (XR_FAILED(xr_result)) {
        fail_xr("xrEnumerateViewConfigurationViews failed", xr_result);
    }
    printf("vr_skeleton: view count = %u, view[0] recommended %ux%u\n",
           view_count, view_configs[0].recommendedImageRectWidth,
           view_configs[0].recommendedImageRectHeight);

    /* --- 6. Create one swapchain per eye --- */
    VkFormat swapchain_format = VK_FORMAT_R8G8B8A8_UNORM;
    XrSwapchain *swapchains = calloc(view_count, sizeof(XrSwapchain));
    for (Uint32 i = 0; i < view_count; i++) {
        XrSwapchainCreateInfo swapchain_create_info = {
            .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
            .format = (int64_t)swapchain_format,
            .sampleCount = 1,
            .width = view_configs[i].recommendedImageRectWidth,
            .height = view_configs[i].recommendedImageRectHeight,
            .faceCount = 1,
            .arraySize = 1,
            .mipCount = 1,
        };
        xr_result = xrCreateSwapchain(session, &swapchain_create_info, &swapchains[i]);
        if (XR_FAILED(xr_result)) {
            fail_xr("xrCreateSwapchain failed", xr_result);
        }
    }
    printf("vr_skeleton: created %u swapchains\n", view_count);

    /* --- 7. Vulkan command pool/buffer for the clear commands --- */
    VkCommandPoolCreateInfo pool_create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk_queue_family_index,
    };
    VkCommandPool command_pool;
    VkResult vk_result = vkCreateCommandPool(vk_device, &pool_create_info, NULL, &command_pool);
    if (vk_result != VK_SUCCESS) {
        fail_vk("vkCreateCommandPool failed", vk_result);
    }

    VkCommandBufferAllocateInfo cb_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command_buffer;
    vk_result = vkAllocateCommandBuffers(vk_device, &cb_alloc_info, &command_buffer);
    if (vk_result != VK_SUCCESS) {
        fail_vk("vkAllocateCommandBuffers failed", vk_result);
    }

    /* --- 8. Poll for session state, begin the session once XR_SESSION_STATE_READY --- */
    bool session_running = false;
    bool should_quit = false;
    int frames_rendered = 0;

    while (!should_quit && frames_rendered < FRAME_COUNT_TO_RUN) {
        XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                XrEventDataSessionStateChanged *state_event = (XrEventDataSessionStateChanged *)&event;
                printf("vr_skeleton: session state -> %d\n", (int)state_event->state);
                if (state_event->state == XR_SESSION_STATE_READY && !session_running) {
                    XrSessionBeginInfo begin_info = {
                        .type = XR_TYPE_SESSION_BEGIN_INFO,
                        .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                    };
                    xr_result = xrBeginSession(session, &begin_info);
                    if (XR_FAILED(xr_result)) {
                        fail_xr("xrBeginSession failed", xr_result);
                    }
                    session_running = true;
                    printf("vr_skeleton: session began\n");
                } else if (state_event->state == XR_SESSION_STATE_STOPPING) {
                    should_quit = true;
                } else if (state_event->state == XR_SESSION_STATE_EXITING ||
                           state_event->state == XR_SESSION_STATE_LOSS_PENDING) {
                    should_quit = true;
                }
            }
            event.type = XR_TYPE_EVENT_DATA_BUFFER;
        }

        if (!session_running) {
            SDL_Delay(10);
            continue;
        }

        XrFrameWaitInfo frame_wait_info = { .type = XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState frame_state = { .type = XR_TYPE_FRAME_STATE };
        xr_result = xrWaitFrame(session, &frame_wait_info, &frame_state);
        if (XR_FAILED(xr_result)) {
            fail_xr("xrWaitFrame failed", xr_result);
        }

        XrFrameBeginInfo frame_begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
        xr_result = xrBeginFrame(session, &frame_begin_info);
        if (XR_FAILED(xr_result)) {
            fail_xr("xrBeginFrame failed", xr_result);
        }

        XrCompositionLayerProjectionView projection_views[8];
        memset(projection_views, 0, sizeof(projection_views));

        if (frame_state.shouldRender) {
            for (Uint32 i = 0; i < view_count; i++) {
                Uint32 image_index = 0;
                XrSwapchainImageAcquireInfo acquire_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                xr_result = xrAcquireSwapchainImage(swapchains[i], &acquire_info, &image_index);
                if (XR_FAILED(xr_result)) {
                    fail_xr("xrAcquireSwapchainImage failed", xr_result);
                }

                XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
                xr_result = xrWaitSwapchainImage(swapchains[i], &wait_info);
                if (XR_FAILED(xr_result)) {
                    fail_xr("xrWaitSwapchainImage failed", xr_result);
                }

                Uint32 xr_image_count = 0;
                xrEnumerateSwapchainImages(swapchains[i], 0, &xr_image_count, NULL);
                XrSwapchainImageVulkan2KHR *xr_images = calloc(xr_image_count, sizeof(XrSwapchainImageVulkan2KHR));
                for (Uint32 k = 0; k < xr_image_count; k++) {
                    xr_images[k].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
                }
                xrEnumerateSwapchainImages(swapchains[i], xr_image_count, &xr_image_count,
                                            (XrSwapchainImageBaseHeader *)xr_images);
                VkImage target_image = xr_images[image_index].image;
                free(xr_images);

                /* Left eye red, right eye blue — cheap visual proof the two
                 * swapchains are genuinely independent, not the same image
                 * cleared twice. */
                VkClearColorValue clear_color = (i == 0)
                    ? (VkClearColorValue){ .float32 = { 1.0f, 0.0f, 0.0f, 1.0f } }
                    : (VkClearColorValue){ .float32 = { 0.0f, 0.0f, 1.0f, 1.0f } };

                vkResetCommandBuffer(command_buffer, 0);
                VkCommandBufferBeginInfo cb_begin_info = {
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                };
                vkBeginCommandBuffer(command_buffer, &cb_begin_info);

                VkImageSubresourceRange subresource_range = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                };
                VkImageMemoryBarrier to_transfer_dst = {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = target_image,
                    .subresourceRange = subresource_range,
                };
                vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_transfer_dst);

                vkCmdClearColorImage(command_buffer, target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      &clear_color, 1, &subresource_range);

                VkImageMemoryBarrier to_present = to_transfer_dst;
                to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                to_present.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_present);

                vkEndCommandBuffer(command_buffer);

                VkSubmitInfo submit_info = {
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .commandBufferCount = 1,
                    .pCommandBuffers = &command_buffer,
                };
                vk_result = vkQueueSubmit(vk_queue, 1, &submit_info, VK_NULL_HANDLE);
                if (vk_result != VK_SUCCESS) {
                    fail_vk("vkQueueSubmit failed", vk_result);
                }
                vkQueueWaitIdle(vk_queue); /* simplicity over perf — this is a spike, not the real render loop */

                XrSwapchainImageReleaseInfo release_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xr_result = xrReleaseSwapchainImage(swapchains[i], &release_info);
                if (XR_FAILED(xr_result)) {
                    fail_xr("xrReleaseSwapchainImage failed", xr_result);
                }

                projection_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                projection_views[i].subImage.swapchain = swapchains[i];
                projection_views[i].subImage.imageRect.extent.width = (int32_t)view_configs[i].recommendedImageRectWidth;
                projection_views[i].subImage.imageRect.extent.height = (int32_t)view_configs[i].recommendedImageRectHeight;
                /* Identity pose/fov: proving the swapchain plumbing here, not
                 * head tracking — that's Phase 2. */
                projection_views[i].pose.orientation.w = 1.0f;
                projection_views[i].fov.angleLeft = -0.7f;
                projection_views[i].fov.angleRight = 0.7f;
                projection_views[i].fov.angleUp = 0.7f;
                projection_views[i].fov.angleDown = -0.7f;
            }
            frames_rendered++;
        }

        XrCompositionLayerProjection projection_layer = {
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
            .viewCount = view_count,
            .views = projection_views,
        };
        const XrCompositionLayerBaseHeader *submitted_layers[1] = {
            (const XrCompositionLayerBaseHeader *)&projection_layer,
        };

        XrFrameEndInfo frame_end_info = {
            .type = XR_TYPE_FRAME_END_INFO,
            .displayTime = frame_state.predictedDisplayTime,
            .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
            .layerCount = frame_state.shouldRender ? 1u : 0u,
            .layers = frame_state.shouldRender ? submitted_layers : NULL,
        };
        xr_result = xrEndFrame(session, &frame_end_info);
        if (XR_FAILED(xr_result)) {
            fail_xr("xrEndFrame failed", xr_result);
        }
    }

    printf("vr_skeleton: rendered %d frames, shutting down cleanly\n", frames_rendered);

    /* --- 9. Clean shutdown --- */
    vkDeviceWaitIdle(vk_device);
    vkFreeCommandBuffers(vk_device, command_pool, 1, &command_buffer);
    vkDestroyCommandPool(vk_device, command_pool, NULL);
    for (Uint32 i = 0; i < view_count; i++) {
        xrDestroySwapchain(swapchains[i]);
    }
    free(swapchains);
    free(view_configs);
    if (session_running) {
        xrEndSession(session);
    }
    xrDestroySession(session);
    xrDestroyInstance(instance);
    SDL_DestroyGPUDevice(gpu);
    SDL_Quit();

    if (frames_rendered == 0) {
        fail("exited without rendering a single frame (session never reached READY?)");
    }

    printf("vr_skeleton: PASS\n");
    return 0;
}
```

- [ ] **Step 2: Verify it builds (Linux)**

```bash
cd /config/projects/fatal/ROLLER
zig build -Dvr 2>&1 | tail -40
```

Expected: builds `vr_skeleton` alongside the normal `roller` executable, no errors. If Zig reports it can't find `openxr/openxr.h` or `-lopenxr_loader`, the build host is missing `libopenxr-dev` (`apt-get install libopenxr-dev`) — this plan assumes it's present because Task 6 already confirmed it's installed on `fatal-gpu`; if building on a different Linux box, install it there first.

- [ ] **Step 3: Commit**

```bash
git add PROJECTS/ROLLER/vr_skeleton_main.c
git commit -m "vr: add walking-skeleton executable (Phase 1)"
```

---

### Task 6: Run the walking skeleton end-to-end on `fatal-gpu` against Monado

**Files:** none (deployment/verification only — this is the task that actually proves Phase 1's goal).

**Interfaces:**
- Consumes: the `vr_skeleton` binary built in Task 5.
- Produces: a real, observed pass/fail result — the deliverable this whole plan exists to produce.

- [ ] **Step 1: Confirm prerequisites are still in place on `fatal-gpu`**

These were installed and verified live during planning; confirm they're still there before relying on them:

```bash
ssh fatal-gpu "dpkg -l libopenxr-dev libopenxr1-monado monado-service 2>&1 | grep ^ii"
ssh fatal-gpu "cat /etc/xdg/openxr/1/active_runtime.json"
```

Expected: all three packages listed as installed (`ii`), and the active runtime JSON points at `libopenxr_monado.so`. If missing, reinstall: `ssh fatal-gpu "apt-get install -y libopenxr-dev libopenxr1-monado monado-service monado-cli"`.

- [ ] **Step 2: Copy the built `vr_skeleton` binary to `fatal-gpu`**

```bash
scp -F ~/.ssh/config /config/projects/fatal/ROLLER/zig-out/bin/vr_skeleton fatal-gpu:/root/roller/vr_skeleton
ssh fatal-gpu "chmod +x /root/roller/vr_skeleton"
```

- [ ] **Step 3: Start Monado in the background**

```bash
ssh fatal-gpu "nohup monado-service > /root/monado.log 2>&1 < /dev/null & disown; sleep 3; cat /root/monado.log"
```

Expected output includes the same fallback sequence confirmed during planning: no real HMD found, falls back to the `legacy` builder, selects `Simulated HMD` with `view count: 2`.

- [ ] **Step 4: Run the walking skeleton**

```bash
ssh fatal-gpu "cd /root/roller && XR_RUNTIME_JSON=/etc/xdg/openxr/1/active_runtime.json ./vr_skeleton > /root/vr_skeleton_run.log 2>&1; echo EXIT_CODE=\$?"
ssh fatal-gpu "cat /root/vr_skeleton_run.log"
```

Expected: exit code 0, log shows `SDL_GPU driver: vulkan`, `OpenXR session created`, `view count = 2`, the session state transitioning to `READY` (numeric state value printed — cross-check against `XR_SESSION_STATE_READY = 3` in `openxr.h` if unsure it's the right one), `rendered 120 frames`, and finally `vr_skeleton: PASS`.

- [ ] **Step 5: Clean up**

```bash
ssh fatal-gpu "pkill -9 monado-service 2>&1; pkill -9 vr_skeleton 2>&1; echo done"
```

- [ ] **Step 6: Record the result**

If Step 4 passed: update `docs/superpowers/specs/2026-09-22-roller-vr-design.md`'s Phase 1 description to note it's complete, and update `/config/projects/fatal/CLAUDE.md` and the `fatal_project` memory with the outcome (mirroring how the rest of this session's findings were recorded).

If Step 4 failed: do not treat that as this plan's failure — record the exact failure point (which OpenXR/Vulkan call, what error code) in the same docs, since a Phase 1 walking skeleton's job is to surface exactly this kind of integration problem cheaply, before Phase 2 builds on top of it. Debugging that failure is follow-up work, not a task this plan's tasks were scoped to absorb.

---

## Self-Review Notes

- **Spec coverage:** this plan implements exactly Phase 1 of the design spec (SDL patch, OpenXR session, both-eyes solid color, Linux runtime validation via Monado, Windows compile-only). Phases 2–5 are explicitly out of scope for this plan, as agreed when starting it.
- **Deviation from the spec recorded:** Phase 1 no longer routes through `scene_render_gpu_create_windowless` — explained in the Architecture section above, since SDL_GPU can't wrap an externally-provided `VkImage`. This should be reflected back into the spec once Task 6 confirms the approach works (Task 6, Step 6).
- **Windows validation gap:** Task 5's Windows path is compile-only (requires `OPENXR_SDK_PATH` set to a manually-downloaded OpenXR-SDK release — no Zig package exists for it, and this plan didn't fabricate one). Real SteamVR runtime testing on Windows is explicitly deferred to the user, who has the hardware.
