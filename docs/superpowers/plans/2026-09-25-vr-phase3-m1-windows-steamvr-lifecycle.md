# ROLLER VR Phase 3, M1: Windows/SteamVR Bring-up and Session Lifecycle — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `roller-vr` start and stop correctly under a real OpenXR runtime (Vulkan interop negotiated the way the OpenXR spec requires, an orderly shutdown from either side, diagnostics in a log file) and produce a Windows build the user can test under SteamVR.

**Architecture:** The OpenXR instance is created before the Vulkan device, so the runtime's Vulkan API version and extension lists can be passed into SDL_GPU through a small hook in the SDL fork (`SDL_GPUVulkanOptions`, the same shape as SDL 3.4's). Session decisions live in a pure C state machine; a quit that originates in OpenXR is forwarded to the game's own shutdown sequence instead of just leaving the frame loop. Windows builds are cross-compiled on this LXC against a staged OpenXR loader and a generated Vulkan import library.

**Tech Stack:** C11, Zig 0.15.2 build system, OpenXR 1.1 (`XR_KHR_vulkan_enable`), Vulkan 1.1, the patched SDL 3.2.22 fork, Monado on `fatal-gpu`, SteamVR on Windows 11.

**Spec:** `docs/superpowers/specs/2026-09-25-roller-vr-phase3-playable-design.md`, section "M1: Windows/SteamVR bring-up and session lifecycle". Task 10 corrects one claim in it (the mechanism for enabling Vulkan extensions) and records how the lifecycle fix works.

**Plan scope:** this is the plan for milestone M1 only. M2 (head tracking), M3 (input and audio) and M4 (menus and HUD) get their own plans once their inputs exist: M2's task list depends on an investigation whose results are not known yet, and M3/M4 build on interfaces M1 and M2 produce.

## Global Constraints

- **Baseline:** ROLLER branch `roller-vr` at commit `90d5aef` (clean tree); SDL fork `/root/sdl-vr-fork`, branch `vr-gpu-properties`, at `3fe781d23`. The patches below were generated against exactly these.
- **The default build must not change.** New `vr_*.c` files are listed only in the `-Dvr` block of `build.zig`; edits in shared files sit inside `#if defined(ROLLER_VR_BUILD)`. Task 10 checks that a plain `zig build` still succeeds and its binary contains no VR or fork symbols.
- **OpenXR/Vulkan calls follow the spec's Valid Usage text, not memory** (the Phase 1 and 2 lesson). The relevant sources are the OpenXR spec files `khr_vulkan_enable.adoc` / `khr_vulkan_enable2.adoc` and the Vulkan validation layer's own messages.
- **No `Co-Authored-By` trailer** on any commit (the user's rule). Commit locally, once per task; do not push unless the user asks.
- **Builds run on the dev LXC only** (`.sdl_vr` is a host-local symlink to `/root/sdl-vr-fork`). Hardware checks run on `fatal-gpu` (Debian LXC, Intel iGPU, Monado): never compile there (2 GB RAM), copy binaries over.
- **Kill processes you start by exact PID** (the dev LXC shares a PID namespace with Docker containers). `fatal-gpu` is a separate LXC; the run script leaves Xorg and `monado-service` running there for reuse.
- **`-Dvr` builds:** about 10 minutes from a cold cache, seconds when incremental. All runs below use the default Debug optimization (roughly 5 frames per second on the iGPU is expected).
- **Windows target:** Windows 11, SteamVR's OpenXR runtime. No headset is available; acceptance is split into what is verifiable without one and a headset-gated remainder (see the spec).

## Review Focus

The spec is a vision document; these are the inputs and conditions it implies but no single task test fully covers, most likely to bite first. Each line names the task whose tests pin it.

1. **The runtime lacks `XR_KHR_vulkan_enable`** (SteamVR's advertised set is unknown). Expected: fall back to `XR_KHR_vulkan_enable2` with a logged warning; if neither is offered, exit non-zero with a clear message. Pinned by Task 3 (`test_binding_choice`, all four combinations) and Task 7 (the no-runtime run).
2. **The runtime's extension list is odd** (repeated spaces, duplicates, a very long name, more than 64 entries). Expected: every name kept once, no crash, failure reported. Pinned by Task 3.
3. **A runtime-required extension is unsupported by the GPU/driver.** Expected: device creation fails with a message that names the extension, and that message reaches the user. Pinned by Task 6 (fork test on real hardware) and Task 7.
4. **`roller-vr.log` cannot be created** (read-only folder, a share without write access). Expected: a console message and a normal run without a log, not a crash. Pinned by Task 2 (`test_open_failure_leaves_streams_alone`).
5. **Quit events arrive in odd orders or several times** (STOPPING before the session began, EXITING or LOSS_PENDING without STOPPING, repeated events, the runtime dying mid-frame). Expected: the game always runs its own shutdown, exactly one quit is requested, and the exit status is non-zero only for real OpenXR failures. Pinned by Task 4 (state machine tests) and Task 8 (three hardware runs).

## Findings that shaped this plan

All verified on 2026-09-25; they explain design choices the tasks below would otherwise seem to contradict.

1. **`XR_KHR_vulkan_enable2` is the wrong extension for this application.** Its valid usage for `XrGraphicsBindingVulkan2KHR` requires `instance` to have been created with `xrCreateVulkanInstanceKHR` and `device` with `xrCreateVulkanDeviceKHR` (OpenXR spec, `khr_vulkan_enable2.adoc`); SDL_GPU creates both itself. The application-created model is `XR_KHR_vulkan_enable` (v1): enable a Vulkan API version inside `xrGetVulkanGraphicsRequirementsKHR`'s range plus every extension `xrGetVulkanInstanceExtensionsKHR`/`xrGetVulkanDeviceExtensionsKHR` list, and use the physical device from `xrGetVulkanGraphicsDeviceKHR` (`khr_vulkan_enable.adoc`). The two `xrGetVulkan*ExtensionsKHR` functions do not exist under enable2. `XrGraphicsBindingVulkan2KHR` is an alias of the v1 struct, so session creation is identical for both.
2. **The Phase 2 validation messages have a precise cause.** `VUID-VkImageMemoryBarrier-None-09120` and `-image-09118` (12 raw lines each) come from the swapchain-release barrier using `VK_QUEUE_FAMILY_EXTERNAL`, which Vulkan allows only if `VK_KHR_external_memory` is enabled or the instance API version is at least 1.1. Requesting 1.1 clears both.
3. **A runtime-initiated session end skipped teardown and the save path.** `vr_run_frame()` returned false, the loop ended, and `doexit()` in `main()` only *requests* the frontend shutdown, which needs more loop iterations to act on; `main()` then returned 0 without `vr_shutdown()` or `ShutdownSDL()`. Observed on Monado: exit status 0 and no `vr_runtime: shutdown` line. (A comment in the old `vr_shutdown()` claimed `doexit()` drives the shutdown; that comment was wrong.) The fix forwards the quit to the game the way `--vr-frames` does and keeps running the game's loop, without OpenXR frames, until it has shut itself down.
4. **Two repo checks were already red on `roller-vr`.** `check-roller-core-manifest` and `check-source-set-drift` both failed because Phase 2's `vr_runtime.c` and `vr_mvp_math_test.c` were never classified in `roller-core.srclist`. Task 1 fixes that. `check-source-set-drift` then still fails at `build.zig:172` (`.files must be an inline .{...} list`) because Phase 2's refactor introduced `.files = game_c_sources`; that is pre-existing debt outside this plan and is left alone.
5. **SDL overwrote the specific error.** `VULKAN_CreateDevice` replaces whatever detail an inner function stored (for example which extension is missing) with a generic "Failed to initialize Vulkan!". Task 6 keeps the cause in the message.
6. **A high `apiVersion` cannot be tested by device creation.** Vulkan 1.1+ implementations must not reject an `apiVersion` above what they support, so a "too new" request still succeeds. The API version is verified end to end instead (the validation counts in Task 7).
7. **Facts about the Monado test runtime** (Debian package 25.0.0): it advertises `XR_KHR_vulkan_enable`; `xrGetVulkanGraphicsRequirementsKHR` reports the range 1.0 to an effectively unbounded maximum (clamped to 127.1023 by `vr_xr_to_vk_version`); the instance extensions it needs are `VK_KHR_external_fence_capabilities`, `VK_KHR_external_memory_capabilities`, `VK_KHR_external_semaphore_capabilities`, `VK_KHR_get_physical_device_properties2`.

## File Structure

| File | Task | Responsibility |
| --- | --- | --- |
| `roller-core.srclist` | 1-5 | classifies each new VR translation unit (`EXCLUDE`) so `check-roller-core-manifest` passes |
| `PROJECTS/ROLLER/vr_log.c/.h`, `tests/vr_log_test.c` | 2 | pure-C diagnostics log: redirects stdout/stderr to `roller-vr.log`, timestamped lines |
| `PROJECTS/ROLLER/vr_ext_list.c/.h`, `tests/vr_ext_list_test.c` | 3 | pure-C extension-name list, OpenXR string parsing, `enable` vs `enable2` choice |
| `PROJECTS/ROLLER/vr_session_fsm.c/.h`, `tests/vr_session_fsm_test.c` | 4 | pure-C OpenXR session lifecycle state machine |
| `PROJECTS/ROLLER/vr_diag.c/.h` | 5 | SDL glue: opens the log next to the exe, routes `SDL_Log`, fatal-error exit |
| `tools/vr-monado-run.sh` (in `projects/fatal/`, outside the ROLLER repo) | 5 | runs `roller-vr` on `fatal-gpu` under Monado with validation and reports |
| `/root/sdl-vr-fork` `SDL_gpu.h`, `SDL_gpu_vulkan.c` | 6 | `SDL_GPUVulkanOptions` hook (API version + extra instance/device extensions) |
| `tests/vr_vulkan_options_test.c` | 6 | real-GPU check of that hook |
| `PROJECTS/ROLLER/vr_runtime.c/.h` | 7, 8 | Vulkan negotiation (7); session lifecycle, clean quit, exit status (8) |
| `PROJECTS/ROLLER/roller.c`, `3d.c` | 5, 7, 8 | VR-gated hooks (log init, GPU creation with options, exit status) |
| `build.zig` | 2-8 | `test-vr-pure` step, VR source list, fork options test |
| `tools/stage-vr-windows-sdks.sh`, `tools/package-vr-windows.sh` (in `projects/fatal/`) | 9 | Windows SDK staging and the folder copied to the PC |

Conventions: the ROLLER repo is `/config/projects/fatal/ROLLER`; the fork is `/root/sdl-vr-fork`; helper scripts live in `/config/projects/fatal/tools/` (not under git). Every `git apply` below runs from the ROLLER repo root.

---

### Task 1: Classify the existing VR translation units

**Files:**
- Modify: `roller-core.srclist`

**Interfaces:**
- Consumes: nothing.
- Produces: `check-roller-core-manifest` passes on `roller-vr`; every later task adds one `EXCLUDE|PROJECTS/ROLLER/<file>.c` line for each `.c` file it creates.

- [ ] **Step 1: Confirm the checks are red**

```bash
cd /config/projects/fatal/ROLLER
git status --short          # expect: nothing
python3 tools/check_roller_core_manifest.py; echo "exit=$?"
python3 tools/check_source_set_drift.py; echo "exit=$?"
```

Expected: both print the same failure and exit 1:

```text
roller-core manifest check failed: unclassified translation units:
  PROJECTS/ROLLER/vr_mvp_math_test.c
  PROJECTS/ROLLER/vr_runtime.c
```

- [ ] **Step 2: Classify both files**

Both are compiled only into the VR executable and are not part of `roller-core`, which is what `EXCLUDE` means.

```bash
git apply --whitespace=nowarn - <<'PATCH'
diff --git a/roller-core.srclist b/roller-core.srclist
index fdbea7a..2844f04 100644
--- a/roller-core.srclist
+++ b/roller-core.srclist
@@ -8,6 +8,8 @@
 # and test-only code that is not linked into roller-core.
 EXCLUDE|PROJECTS/ROLLER/roller.c
 EXCLUDE|PROJECTS/ROLLER/roller_web.c
+EXCLUDE|PROJECTS/ROLLER/vr_mvp_math_test.c
+EXCLUDE|PROJECTS/ROLLER/vr_runtime.c
 EXCLUDE|PROJECTS/ROLLER/nuklear_sdl_renderer.c
 EXCLUDE|PROJECTS/ROLLER/rollercomms.c
 EXCLUDE|PROJECTS/ROLLER/sound.c
PATCH
```

- [ ] **Step 3: Verify the manifest check passes**

```bash
python3 tools/check_roller_core_manifest.py; echo "exit=$?"
python3 tools/check_source_set_drift.py; echo "exit=$?"
```

Expected:

```text
roller-core manifest check passed: 90 translation units (EXCLUDE=15, STUB_SWAP=6, KEEP=40, PRESENT_BUT_DORMANT=29)
exit=0
source-set drift check failed: build.zig:172: .files must be an inline .{...} list
exit=1
```

The drift check still fails, for the pre-existing reason in Finding 4 (`.files = game_c_sources`, introduced by Phase 2's `build.zig` refactor). That is out of scope here; do not change the checker or `build.zig` to make it pass.

- [ ] **Step 4: Commit**

```bash
git add roller-core.srclist
git commit -m "build: classify the VR translation units in roller-core.srclist

vr_runtime.c and vr_mvp_math_test.c were never listed, which made
check-roller-core-manifest (and the first half of check-source-set-drift)
fail on this branch. Both are VR-only and not part of roller-core."
```

---

### Task 2: `vr_log` — the diagnostics log (pure C, TDD)

**Files:**
- Create: `PROJECTS/ROLLER/vr_log.h`, `PROJECTS/ROLLER/vr_log.c`, `tests/vr_log_test.c`
- Modify: `build.zig` (the `addPureCTest` helper and the `test-vr-pure` step), `roller-core.srclist`

**Interfaces:**
- Consumes: nothing.
- Produces (used by Tasks 5, 7, 8):
  - `bool vr_log_open(const char *path)` — truncates/creates `path`, redirects stdout and stderr into it (unbuffered); a second call while open is a no-op returning true; on failure stdout/stderr are untouched.
  - `void vr_log_close(void)`, `bool vr_log_is_open(void)`
  - `size_t vr_log_format_line(char *out, size_t out_size, long elapsed_ms, const char *level, const char *message)` — always NUL-terminated and newline-terminated, truncating if needed.
  - `void vr_log_line(const char *level, const char *fmt, ...)` — one line `[<sec>.<ms>s] <LEVEL> <message>` to stderr (the log file while open).
  - Build: `zig build test-vr-pure` (also part of `zig build test`), and the helper `addPureCTest(b, target, optimize, c_flags, name, test_source, module_source)`.

- [ ] **Step 1: Write the failing test**

Create `tests/vr_log_test.c`:

```c
/* tests/vr_log_test.c -- unit tests for PROJECTS/ROLLER/vr_log.c (pure C, no SDL). */
#include "vr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void test_format_basic(void)
{
    char buf[128];
    size_t n = vr_log_format_line(buf, sizeof(buf), 12345, "INFO", "hello");
    CHECK(strcmp(buf, "[12.345s] INFO hello\n") == 0);
    CHECK(n == strlen(buf));

    vr_log_format_line(buf, sizeof(buf), 7, "WARN", "x");
    CHECK(strcmp(buf, "[0.007s] WARN x\n") == 0);

    vr_log_format_line(buf, sizeof(buf), -5, NULL, NULL);
    CHECK(strcmp(buf, "[0.000s] INFO \n") == 0);
}

static void test_format_truncates_but_keeps_newline(void)
{
    char buf[16];
    size_t n = vr_log_format_line(buf, sizeof(buf), 1, "ERROR", "a very long message that cannot fit");
    CHECK(n == strlen(buf));
    CHECK(n == sizeof(buf) - 1);
    CHECK(buf[n - 1] == '\n');

    char tiny[2];
    n = vr_log_format_line(tiny, sizeof(tiny), 1, "INFO", "x");
    CHECK(tiny[0] == '\n' && tiny[1] == '\0' && n == 1);
    CHECK(vr_log_format_line(tiny, 1, 1, "INFO", "x") == 0);
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *data = calloc(1, 4096);
    size_t got = fread(data, 1, 4095, f);
    data[got] = '\0';
    fclose(f);
    return data;
}

static void test_open_captures_stdout_stderr_and_restores(void)
{
    const char *path = "vr_log_test.tmp.log";
    remove(path);

    CHECK(!vr_log_is_open());
    CHECK(vr_log_open(path));
    CHECK(vr_log_is_open());
    CHECK(vr_log_open(path)); /* second open is a no-op */

    vr_log_line("WARN", "x=%d", 7);
    fprintf(stderr, "raw stderr line\n");
    printf("raw stdout line\n");
    vr_log_close();
    CHECK(!vr_log_is_open());
    vr_log_close(); /* closing twice is safe */

    char *text = slurp(path);
    CHECK(text != NULL);
    if (text) {
        CHECK(strstr(text, "WARN x=7\n") != NULL);
        CHECK(strstr(text, "raw stderr line\n") != NULL);
        CHECK(strstr(text, "raw stdout line\n") != NULL);
        free(text);
    }
    remove(path);
}

static void test_open_failure_leaves_streams_alone(void)
{
    CHECK(!vr_log_open("/nonexistent-dir-for-vr-log-test/x.log"));
    CHECK(!vr_log_is_open());
    CHECK(!vr_log_open(NULL));
    printf("stdout still works after a failed open\n");
}

int main(void)
{
    test_format_basic();
    test_format_truncates_but_keeps_newline();
    test_open_captures_stdout_stderr_and_restores();
    test_open_failure_leaves_streams_alone();

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("vr_log_test: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Wire the test into the build and the manifest**

```bash
git apply --whitespace=nowarn - <<'PATCH'
diff --git a/build.zig b/build.zig
index 8803137..e34cfb7 100644
--- a/build.zig
+++ b/build.zig
@@ -901,9 +901,16 @@ fn configureRenderQueue3DTests(
     );
     tick_clock_tests.dependOn(&run_tick_clock.step);
 
+    const vr_pure_tests = b.step(
+        "test-vr-pure",
+        "Run the pure-C VR unit tests (log, extension list, session state machine)",
+    );
+    vr_pure_tests.dependOn(addPureCTest(b, target, optimize, c_flags, "vr_log_test", "tests/vr_log_test.c", "PROJECTS/ROLLER/vr_log.c"));
+
     const test_step = b.step("test", "Run focused unit tests and optional seam checks");
     test_step.dependOn(render_queue_tests);
     test_step.dependOn(tick_clock_tests);
+    test_step.dependOn(vr_pure_tests);
 
     const roller_core_manifest_check = b.addSystemCommand(&.{
         pythonExe(),
@@ -1595,6 +1602,34 @@ fn configureDesktopPlatform(
     }
 }
 
+/// Builds one dependency-free C unit test (the test source plus the single module it
+/// tests) and returns the step that runs it.
+fn addPureCTest(
+    b: *Build,
+    target: ResolvedTarget,
+    optimize: OptimizeMode,
+    c_flags: []const []const u8,
+    name: []const u8,
+    test_source: []const u8,
+    module_source: []const u8,
+) *Build.Step {
+    const mod = b.createModule(.{
+        .target = target,
+        .optimize = optimize,
+        .link_libc = true,
+    });
+    mod.addIncludePath(b.path("PROJECTS/ROLLER"));
+    mod.addCSourceFiles(.{
+        .flags = c_flags,
+        .files = &.{ test_source, module_source },
+    });
+    const exe = b.addExecutable(.{
+        .name = name,
+        .root_module = mod,
+    });
+    return &b.addRunArtifact(exe).step;
+}
+
 /// SDL package the game links against: the pinned Zizin13/SDL (`.sdl`) by default, the
 /// patched VR fork (`.sdl_vr`) under -Dvr. `.sdl_vr` is a lazy dependency so a plain build
 /// never resolves it; null means it still has to be resolved and the build will restart.
diff --git a/roller-core.srclist b/roller-core.srclist
index 2844f04..0e881e4 100644
--- a/roller-core.srclist
+++ b/roller-core.srclist
@@ -8,6 +8,7 @@
 # and test-only code that is not linked into roller-core.
 EXCLUDE|PROJECTS/ROLLER/roller.c
 EXCLUDE|PROJECTS/ROLLER/roller_web.c
+EXCLUDE|PROJECTS/ROLLER/vr_log.c
 EXCLUDE|PROJECTS/ROLLER/vr_mvp_math_test.c
 EXCLUDE|PROJECTS/ROLLER/vr_runtime.c
 EXCLUDE|PROJECTS/ROLLER/nuklear_sdl_renderer.c
PATCH
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
zig build test-vr-pure 2>&1 | grep -E "error" | head -3
```

Expected: a failure because the module does not exist yet, for example
`error: failed to check cache: '.../PROJECTS/ROLLER/vr_log.c' file_hash FileNotFound`.

- [ ] **Step 4: Write the implementation**

Create `PROJECTS/ROLLER/vr_log.h`:

```c
/* PROJECTS/ROLLER/vr_log.h
 *
 * Diagnostics log for roller-vr: one file next to the exe that captures
 * everything written to stdout and stderr (the VR runtime code prints with
 * printf/fprintf(stderr)), plus timestamped lines from vr_log_line(). Pure C
 * (stdio + dup/dup2 only, no SDL) so it can be unit-tested on its own. */
#ifndef VR_LOG_H
#define VR_LOG_H

#include <stdbool.h>
#include <stddef.h>

/* Truncates/creates `path` and redirects stdout and stderr into it (unbuffered,
 * so a crash loses nothing). Returns false, leaving stdout/stderr untouched, if
 * the file cannot be created. A second call while open is a no-op returning true. */
bool vr_log_open(const char *path);

/* Restores the original stdout/stderr and closes the file. Safe to call when not open. */
void vr_log_close(void);

bool vr_log_is_open(void);

/* Formats "[<sec>.<ms>s] <level> <message>\n" into out. The result is always NUL
 * terminated and always ends in '\n', truncating the message if it does not fit.
 * out_size must be at least 2. Returns the length written, excluding the NUL. */
size_t vr_log_format_line(char *out, size_t out_size, long elapsed_ms,
                          const char *level, const char *message);

/* Writes one formatted line to stderr (the log file while open). elapsed time is
 * measured from vr_log_open(); before that it prints 0.000s. */
void vr_log_line(const char *level, const char *fmt, ...);

#endif /* VR_LOG_H */
```

Create `PROJECTS/ROLLER/vr_log.c`:

```c
/* PROJECTS/ROLLER/vr_log.c -- see vr_log.h */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* fileno(), dup(), dup2() under strict -std=c11 */
#endif

#include "vr_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <io.h>
#define VR_DUP   _dup
#define VR_DUP2  _dup2
#define VR_CLOSE _close
#define VR_FILENO _fileno
#else
#include <unistd.h>
#define VR_DUP   dup
#define VR_DUP2  dup2
#define VR_CLOSE close
#define VR_FILENO fileno
#endif

static FILE *s_file = NULL;
static int s_saved_out = -1;
static int s_saved_err = -1;
static struct timespec s_start;

bool vr_log_open(const char *path)
{
    if (s_file)
        return true;
    if (!path)
        return false;

    FILE *f = fopen(path, "wb");
    if (!f)
        return false;

    fflush(stdout);
    fflush(stderr);
    s_saved_out = VR_DUP(1);
    s_saved_err = VR_DUP(2);
    if (VR_DUP2(VR_FILENO(f), 1) < 0 || VR_DUP2(VR_FILENO(f), 2) < 0) {
        if (s_saved_out >= 0) { VR_DUP2(s_saved_out, 1); VR_CLOSE(s_saved_out); }
        if (s_saved_err >= 0) { VR_DUP2(s_saved_err, 2); VR_CLOSE(s_saved_err); }
        s_saved_out = s_saved_err = -1;
        fclose(f);
        return false;
    }
    /* Nothing is pending (flushed above), so switching to unbuffered is safe. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    s_file = f;
    timespec_get(&s_start, TIME_UTC);
    return true;
}

void vr_log_close(void)
{
    if (!s_file)
        return;
    fflush(stdout);
    fflush(stderr);
    if (s_saved_out >= 0) { VR_DUP2(s_saved_out, 1); VR_CLOSE(s_saved_out); }
    if (s_saved_err >= 0) { VR_DUP2(s_saved_err, 2); VR_CLOSE(s_saved_err); }
    s_saved_out = s_saved_err = -1;
    fclose(s_file);
    s_file = NULL;
}

bool vr_log_is_open(void)
{
    return s_file != NULL;
}

size_t vr_log_format_line(char *out, size_t out_size, long elapsed_ms,
                          const char *level, const char *message)
{
    if (!out || out_size < 2)
        return 0;
    if (elapsed_ms < 0)
        elapsed_ms = 0;
    if (!level)
        level = "INFO";
    if (!message)
        message = "";

    int n = snprintf(out, out_size, "[%ld.%03lds] %s %s\n",
                     elapsed_ms / 1000, elapsed_ms % 1000, level, message);
    if (n < 0) {
        out[0] = '\n';
        out[1] = '\0';
        return 1;
    }
    if ((size_t)n >= out_size) {
        /* Truncated: keep the "always ends in newline" guarantee. */
        out[out_size - 2] = '\n';
        out[out_size - 1] = '\0';
        return out_size - 1;
    }
    return (size_t)n;
}

void vr_log_line(const char *level, const char *fmt, ...)
{
    char message[1024];
    char line[1200];

    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt ? fmt : "", args);
    va_end(args);

    long elapsed_ms = 0;
    if (s_file) {
        struct timespec now;
        timespec_get(&now, TIME_UTC);
        elapsed_ms = (long)((now.tv_sec - s_start.tv_sec) * 1000 +
                            (now.tv_nsec - s_start.tv_nsec) / 1000000);
    }
    vr_log_format_line(line, sizeof(line), elapsed_ms, level, message);
    fputs(line, stderr);
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
zig build test-vr-pure
python3 tools/check_roller_core_manifest.py
```

Expected: the test prints `vr_log_test: ALL PASS` (plus `stdout still works after a failed open`) and the manifest check passes (91 translation units).

- [ ] **Step 6: Commit**

```bash
git add PROJECTS/ROLLER/vr_log.c PROJECTS/ROLLER/vr_log.h tests/vr_log_test.c build.zig roller-core.srclist
git commit -m "vr: add vr_log, a diagnostics log that captures stdout and stderr

Pure C (stdio + dup/dup2), unit-tested, with the test-vr-pure build step
that the next VR modules reuse."
```

---

### Task 3: `vr_ext_list` — extension lists and the `enable` vs `enable2` choice (pure C, TDD)

**Files:**
- Create: `PROJECTS/ROLLER/vr_ext_list.h`, `PROJECTS/ROLLER/vr_ext_list.c`, `tests/vr_ext_list_test.c`
- Modify: `build.zig`, `roller-core.srclist`

**Interfaces:**
- Consumes: the `test-vr-pure` step and `addPureCTest` (Task 2).
- Produces (used by Task 7):
  - `VrExtList` (`names[VR_EXT_LIST_MAX][VR_EXT_NAME_MAX]`, `count`), `VR_EXT_LIST_MAX` = 64, `VR_EXT_NAME_MAX` = 128
  - `void vr_ext_list_init(VrExtList *)`, `bool vr_ext_list_contains(const VrExtList *, const char *)`, `bool vr_ext_list_add(VrExtList *, const char *)`, `bool vr_ext_list_add_spaced(VrExtList *, const char *)`, `unsigned int vr_ext_list_ptrs(const VrExtList *, const char **out, unsigned int out_cap)`
  - `VrVkBinding` = `VR_VK_BINDING_NONE | VR_VK_BINDING_ENABLE_V1 | VR_VK_BINDING_ENABLE2_FALLBACK` and `VrVkBinding vr_choose_vk_binding(bool runtime_has_vulkan_enable, bool runtime_has_vulkan_enable2)`

- [ ] **Step 1: Write the failing test**

Create `tests/vr_ext_list_test.c`:

```c
/* tests/vr_ext_list_test.c -- unit tests for PROJECTS/ROLLER/vr_ext_list.c (pure C). */
#include "vr_ext_list.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void test_add_and_dedupe(void)
{
    VrExtList l;
    vr_ext_list_init(&l);
    CHECK(l.count == 0);
    CHECK(vr_ext_list_add(&l, "VK_KHR_external_memory"));
    CHECK(vr_ext_list_add(&l, "VK_KHR_external_memory")); /* duplicate: no-op, still true */
    CHECK(l.count == 1);
    CHECK(vr_ext_list_contains(&l, "VK_KHR_external_memory"));
    CHECK(!vr_ext_list_contains(&l, "VK_KHR_external_semaphore"));
    CHECK(!vr_ext_list_contains(&l, NULL));
}

static void test_add_rejects_bad_names(void)
{
    VrExtList l;
    vr_ext_list_init(&l);
    CHECK(!vr_ext_list_add(&l, NULL));
    CHECK(!vr_ext_list_add(&l, ""));

    char too_long[VR_EXT_NAME_MAX + 1];
    memset(too_long, 'x', VR_EXT_NAME_MAX);
    too_long[VR_EXT_NAME_MAX] = '\0';
    CHECK(!vr_ext_list_add(&l, too_long));

    char longest_ok[VR_EXT_NAME_MAX];
    memset(longest_ok, 'y', VR_EXT_NAME_MAX - 1);
    longest_ok[VR_EXT_NAME_MAX - 1] = '\0';
    CHECK(vr_ext_list_add(&l, longest_ok));
    CHECK(l.count == 1);
}

static void test_add_full_list(void)
{
    VrExtList l;
    vr_ext_list_init(&l);
    char name[32];
    for (int i = 0; i < VR_EXT_LIST_MAX; i++) {
        snprintf(name, sizeof(name), "VK_TEST_ext_%d", i);
        CHECK(vr_ext_list_add(&l, name));
    }
    CHECK(l.count == VR_EXT_LIST_MAX);
    CHECK(!vr_ext_list_add(&l, "VK_TEST_one_too_many"));
    CHECK(l.count == VR_EXT_LIST_MAX);
    CHECK(vr_ext_list_add(&l, "VK_TEST_ext_3")); /* already present: still fine when full */
}

static void test_spaced_parsing(void)
{
    VrExtList l;
    vr_ext_list_init(&l);
    CHECK(vr_ext_list_add_spaced(&l, "VK_KHR_a VK_KHR_b VK_KHR_a"));
    CHECK(l.count == 2);
    CHECK(strcmp(l.names[0], "VK_KHR_a") == 0);
    CHECK(strcmp(l.names[1], "VK_KHR_b") == 0);

    CHECK(vr_ext_list_add_spaced(&l, "   VK_KHR_c    VK_KHR_d  "));
    CHECK(l.count == 4);
    CHECK(vr_ext_list_add_spaced(&l, ""));
    CHECK(vr_ext_list_add_spaced(&l, "    "));
    CHECK(vr_ext_list_add_spaced(&l, NULL));
    CHECK(l.count == 4);
    CHECK(!vr_ext_list_add_spaced(NULL, "VK_KHR_a"));
}

static void test_spaced_reports_failure_but_keeps_earlier_names(void)
{
    VrExtList l;
    vr_ext_list_init(&l);
    char huge[VR_EXT_NAME_MAX + 8];
    memset(huge, 'z', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';

    char input[VR_EXT_NAME_MAX + 64];
    snprintf(input, sizeof(input), "VK_KHR_ok %s VK_KHR_after", huge);
    CHECK(!vr_ext_list_add_spaced(&l, input));
    CHECK(vr_ext_list_contains(&l, "VK_KHR_ok"));
    CHECK(vr_ext_list_contains(&l, "VK_KHR_after")); /* parsing continues past the bad token */
    CHECK(l.count == 2);
}

static void test_ptrs(void)
{
    VrExtList l;
    vr_ext_list_init(&l);
    vr_ext_list_add_spaced(&l, "VK_KHR_a VK_KHR_b VK_KHR_c");

    const char *out[8];
    unsigned int n = vr_ext_list_ptrs(&l, out, 8);
    CHECK(n == 3);
    CHECK(strcmp(out[0], "VK_KHR_a") == 0 && strcmp(out[2], "VK_KHR_c") == 0);

    n = vr_ext_list_ptrs(&l, out, 2); /* capacity smaller than the list */
    CHECK(n == 2);
    CHECK(vr_ext_list_ptrs(NULL, out, 8) == 0);
    CHECK(vr_ext_list_ptrs(&l, NULL, 8) == 0);
}

static void test_binding_choice(void)
{
    CHECK(vr_choose_vk_binding(true, true) == VR_VK_BINDING_ENABLE_V1);
    CHECK(vr_choose_vk_binding(true, false) == VR_VK_BINDING_ENABLE_V1);
    CHECK(vr_choose_vk_binding(false, true) == VR_VK_BINDING_ENABLE2_FALLBACK);
    CHECK(vr_choose_vk_binding(false, false) == VR_VK_BINDING_NONE);
}

int main(void)
{
    test_add_and_dedupe();
    test_add_rejects_bad_names();
    test_add_full_list();
    test_spaced_parsing();
    test_spaced_reports_failure_but_keeps_earlier_names();
    test_ptrs();
    test_binding_choice();

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("vr_ext_list_test: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Wire the test into the build and the manifest**

```bash
git apply --whitespace=nowarn - <<'PATCH'
diff --git a/build.zig b/build.zig
index e34cfb7..d8f8128 100644
--- a/build.zig
+++ b/build.zig
@@ -906,6 +906,7 @@ fn configureRenderQueue3DTests(
         "Run the pure-C VR unit tests (log, extension list, session state machine)",
     );
     vr_pure_tests.dependOn(addPureCTest(b, target, optimize, c_flags, "vr_log_test", "tests/vr_log_test.c", "PROJECTS/ROLLER/vr_log.c"));
+    vr_pure_tests.dependOn(addPureCTest(b, target, optimize, c_flags, "vr_ext_list_test", "tests/vr_ext_list_test.c", "PROJECTS/ROLLER/vr_ext_list.c"));
 
     const test_step = b.step("test", "Run focused unit tests and optional seam checks");
     test_step.dependOn(render_queue_tests);
diff --git a/roller-core.srclist b/roller-core.srclist
index 0e881e4..cf1d3a4 100644
--- a/roller-core.srclist
+++ b/roller-core.srclist
@@ -8,6 +8,7 @@
 # and test-only code that is not linked into roller-core.
 EXCLUDE|PROJECTS/ROLLER/roller.c
 EXCLUDE|PROJECTS/ROLLER/roller_web.c
+EXCLUDE|PROJECTS/ROLLER/vr_ext_list.c
 EXCLUDE|PROJECTS/ROLLER/vr_log.c
 EXCLUDE|PROJECTS/ROLLER/vr_mvp_math_test.c
 EXCLUDE|PROJECTS/ROLLER/vr_runtime.c
PATCH
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
zig build test-vr-pure 2>&1 | grep -E "error" | head -3
```

Expected: `error: failed to check cache: '.../PROJECTS/ROLLER/vr_ext_list.c' file_hash FileNotFound`.

- [ ] **Step 4: Write the implementation**

Create `PROJECTS/ROLLER/vr_ext_list.h`:

```c
/* PROJECTS/ROLLER/vr_ext_list.h
 *
 * Small, dependency-free helpers for negotiating Vulkan extensions between the
 * OpenXR runtime and SDL_GPU: a fixed-capacity de-duplicated name list that can
 * parse OpenXR's space-separated extension strings (the format returned by
 * xrGetVulkanInstanceExtensionsKHR / xrGetVulkanDeviceExtensionsKHR), and the
 * choice of which XR_KHR_vulkan_enable* variant to use. Pure C so it can be
 * unit-tested without OpenXR or Vulkan headers. */
#ifndef VR_EXT_LIST_H
#define VR_EXT_LIST_H

#include <stdbool.h>

#define VR_EXT_LIST_MAX  64
#define VR_EXT_NAME_MAX  128

typedef struct {
    char         names[VR_EXT_LIST_MAX][VR_EXT_NAME_MAX];
    unsigned int count;
} VrExtList;

void vr_ext_list_init(VrExtList *list);
bool vr_ext_list_contains(const VrExtList *list, const char *name);

/* Adds `name` unless already present (then it is a no-op returning true).
 * Returns false, leaving the list unchanged, for a NULL/empty name, a name of
 * VR_EXT_NAME_MAX characters or more, or when the list is full. */
bool vr_ext_list_add(VrExtList *list, const char *name);

/* Adds every name in a space-separated string (repeated spaces and leading or
 * trailing spaces are fine; NULL or "" adds nothing). Returns false if any name
 * could not be added; names before the failing one stay added. */
bool vr_ext_list_add_spaced(VrExtList *list, const char *spaced);

/* Writes up to out_cap pointers to the stored names (pointing into `list`, valid
 * while the list is unchanged and not copied) into out. Returns how many were
 * written. This is the const char ** shape Vulkan and SDL_GPUVulkanOptions want. */
unsigned int vr_ext_list_ptrs(const VrExtList *list, const char **out, unsigned int out_cap);

typedef enum {
    VR_VK_BINDING_NONE = 0,
    /* XR_KHR_vulkan_enable: the application creates the VkInstance/VkDevice and
     * enables the extensions the runtime lists. This is the model SDL_GPU (which
     * owns device creation) fits. */
    VR_VK_BINDING_ENABLE_V1,
    /* XR_KHR_vulkan_enable2 only: its valid usage requires the runtime to create
     * the instance and device (xrCreateVulkanInstanceKHR/DeviceKHR). Used only as
     * a fallback, with a logged warning, when the runtime lacks enable v1. */
    VR_VK_BINDING_ENABLE2_FALLBACK
} VrVkBinding;

VrVkBinding vr_choose_vk_binding(bool runtime_has_vulkan_enable, bool runtime_has_vulkan_enable2);

#endif /* VR_EXT_LIST_H */
```

Create `PROJECTS/ROLLER/vr_ext_list.c`:

```c
/* PROJECTS/ROLLER/vr_ext_list.c -- see vr_ext_list.h */
#include "vr_ext_list.h"

#include <string.h>

void vr_ext_list_init(VrExtList *list)
{
    if (!list)
        return;
    list->count = 0;
}

bool vr_ext_list_contains(const VrExtList *list, const char *name)
{
    if (!list || !name)
        return false;
    for (unsigned int i = 0; i < list->count; i++) {
        if (strcmp(list->names[i], name) == 0)
            return true;
    }
    return false;
}

bool vr_ext_list_add(VrExtList *list, const char *name)
{
    if (!list || !name || name[0] == '\0')
        return false;
    size_t len = strlen(name);
    if (len >= VR_EXT_NAME_MAX)
        return false;
    if (vr_ext_list_contains(list, name))
        return true;
    if (list->count >= VR_EXT_LIST_MAX)
        return false;
    memcpy(list->names[list->count], name, len + 1);
    list->count++;
    return true;
}

bool vr_ext_list_add_spaced(VrExtList *list, const char *spaced)
{
    if (!list)
        return false;
    if (!spaced)
        return true;

    bool ok = true;
    const char *p = spaced;
    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != ' ')
            p++;
        size_t len = (size_t)(p - start);
        if (len >= VR_EXT_NAME_MAX) {
            ok = false;
            continue;
        }
        char token[VR_EXT_NAME_MAX];
        memcpy(token, start, len);
        token[len] = '\0';
        if (!vr_ext_list_add(list, token))
            ok = false;
    }
    return ok;
}

unsigned int vr_ext_list_ptrs(const VrExtList *list, const char **out, unsigned int out_cap)
{
    if (!list || !out)
        return 0;
    unsigned int n = list->count < out_cap ? list->count : out_cap;
    for (unsigned int i = 0; i < n; i++)
        out[i] = list->names[i];
    return n;
}

VrVkBinding vr_choose_vk_binding(bool runtime_has_vulkan_enable, bool runtime_has_vulkan_enable2)
{
    if (runtime_has_vulkan_enable)
        return VR_VK_BINDING_ENABLE_V1;
    if (runtime_has_vulkan_enable2)
        return VR_VK_BINDING_ENABLE2_FALLBACK;
    return VR_VK_BINDING_NONE;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
zig build test-vr-pure
python3 tools/check_roller_core_manifest.py
```

Expected: `vr_ext_list_test: ALL PASS` (and the log test still passes); the manifest check passes.

- [ ] **Step 6: Commit**

```bash
git add PROJECTS/ROLLER/vr_ext_list.c PROJECTS/ROLLER/vr_ext_list.h tests/vr_ext_list_test.c build.zig roller-core.srclist
git commit -m "vr: add vr_ext_list for Vulkan extension negotiation

De-duplicated extension-name list that parses OpenXR's space-separated
strings, and the choice between XR_KHR_vulkan_enable (application-created
Vulkan objects) and the enable2 fallback. Pure C, unit-tested."
```

---

### Task 4: `vr_session_fsm` — the session lifecycle state machine (pure C, TDD)

**Files:**
- Create: `PROJECTS/ROLLER/vr_session_fsm.h`, `PROJECTS/ROLLER/vr_session_fsm.c`, `tests/vr_session_fsm_test.c`
- Modify: `build.zig`, `roller-core.srclist`

**Interfaces:**
- Consumes: the `test-vr-pure` step (Task 2).
- Produces (used by Task 8):
  - `VrSessionState` (`VR_STATE_UNKNOWN` = 0 ... `VR_STATE_EXITING` = 8, numerically equal to `XrSessionState`), `VrSessionFsm` (`state`, `session_running`, `quit_requested`, `exit_requested`)
  - action flags `VR_ACTION_NONE`, `VR_ACTION_BEGIN_SESSION`, `VR_ACTION_END_SESSION`, `VR_ACTION_REQUEST_QUIT` (bit flags)
  - `void vr_fsm_init(VrSessionFsm *)`, `unsigned int vr_fsm_on_state_changed(VrSessionFsm *, VrSessionState)`, `void vr_fsm_session_begun(VrSessionFsm *)`, `void vr_fsm_session_ended(VrSessionFsm *)`, `bool vr_fsm_should_request_exit(VrSessionFsm *)`, `bool vr_fsm_runtime_is_ending(const VrSessionFsm *)`, `bool vr_fsm_frames_allowed(const VrSessionFsm *)`, `bool vr_fsm_is_visible(const VrSessionFsm *)`, `bool vr_fsm_is_focused(const VrSessionFsm *)`

- [ ] **Step 1: Write the failing test**

Create `tests/vr_session_fsm_test.c`:

```c
/* tests/vr_session_fsm_test.c -- unit tests for PROJECTS/ROLLER/vr_session_fsm.c (pure C). */
#include "vr_session_fsm.h"

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* Drives the fsm as vr_runtime.c does: carry out BEGIN/END actions immediately. */
static unsigned int step(VrSessionFsm *f, VrSessionState s)
{
    unsigned int a = vr_fsm_on_state_changed(f, s);
    if (a & VR_ACTION_BEGIN_SESSION)
        vr_fsm_session_begun(f);
    if (a & VR_ACTION_END_SESSION)
        vr_fsm_session_ended(f);
    return a;
}

static void test_normal_startup_sequence(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    CHECK(!vr_fsm_frames_allowed(&f));
    CHECK(step(&f, VR_STATE_IDLE) == VR_ACTION_NONE);
    CHECK(step(&f, VR_STATE_READY) == VR_ACTION_BEGIN_SESSION);
    CHECK(vr_fsm_frames_allowed(&f));
    CHECK(!vr_fsm_is_visible(&f) && !vr_fsm_is_focused(&f));

    CHECK(step(&f, VR_STATE_SYNCHRONIZED) == VR_ACTION_NONE);
    CHECK(!vr_fsm_is_visible(&f));
    CHECK(step(&f, VR_STATE_VISIBLE) == VR_ACTION_NONE);
    CHECK(vr_fsm_is_visible(&f) && !vr_fsm_is_focused(&f));
    CHECK(step(&f, VR_STATE_FOCUSED) == VR_ACTION_NONE);
    CHECK(vr_fsm_is_visible(&f) && vr_fsm_is_focused(&f));

    /* dashboard opens: still visible, no longer focused */
    CHECK(step(&f, VR_STATE_VISIBLE) == VR_ACTION_NONE);
    CHECK(vr_fsm_is_visible(&f) && !vr_fsm_is_focused(&f));
}

static void test_ready_only_begins_once(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    CHECK(step(&f, VR_STATE_READY) == VR_ACTION_BEGIN_SESSION);
    CHECK(step(&f, VR_STATE_READY) == VR_ACTION_NONE); /* already running */
}

static void test_runtime_initiated_stop(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    step(&f, VR_STATE_READY);
    step(&f, VR_STATE_FOCUSED);

    unsigned int a = step(&f, VR_STATE_STOPPING);
    CHECK(a == (VR_ACTION_END_SESSION | VR_ACTION_REQUEST_QUIT));
    CHECK(!vr_fsm_frames_allowed(&f));

    /* the runtime then reports EXITING: quit was already requested, nothing new */
    CHECK(step(&f, VR_STATE_EXITING) == VR_ACTION_NONE);
    CHECK(step(&f, VR_STATE_STOPPING) == VR_ACTION_NONE);
}

static void test_stopping_before_session_began(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    CHECK(step(&f, VR_STATE_STOPPING) == VR_ACTION_REQUEST_QUIT); /* nothing to end */
}

static void test_loss_pending_never_ends_session(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    step(&f, VR_STATE_READY);
    step(&f, VR_STATE_VISIBLE);
    CHECK(step(&f, VR_STATE_LOSS_PENDING) == VR_ACTION_REQUEST_QUIT);
    CHECK(vr_fsm_frames_allowed(&f)); /* xrEndSession is invalid here, so still "running" */
    CHECK(step(&f, VR_STATE_LOSS_PENDING) == VR_ACTION_NONE);
}

static void test_exiting_alone_requests_quit_once(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    CHECK(step(&f, VR_STATE_EXITING) == VR_ACTION_REQUEST_QUIT);
    CHECK(step(&f, VR_STATE_EXITING) == VR_ACTION_NONE);
}

static void test_game_initiated_exit_request(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    CHECK(!vr_fsm_should_request_exit(&f)); /* not running yet */

    step(&f, VR_STATE_READY);
    step(&f, VR_STATE_FOCUSED);
    CHECK(vr_fsm_should_request_exit(&f));
    CHECK(!vr_fsm_should_request_exit(&f)); /* only once */

    /* the runtime answers with STOPPING: end the session, but the game already
     * knows it is quitting, so a quit request is still reported once (it is idempotent) */
    unsigned int a = step(&f, VR_STATE_STOPPING);
    CHECK(a & VR_ACTION_END_SESSION);
}

static void test_no_exit_request_once_runtime_is_ending(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    step(&f, VR_STATE_READY);
    step(&f, VR_STATE_LOSS_PENDING);
    CHECK(!vr_fsm_should_request_exit(&f));

    VrSessionFsm g;
    vr_fsm_init(&g);
    step(&g, VR_STATE_READY);
    vr_fsm_on_state_changed(&g, VR_STATE_STOPPING); /* STOPPING seen, END not yet applied */
    CHECK(!vr_fsm_should_request_exit(&g));
}

static void test_runtime_is_ending(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    CHECK(!vr_fsm_runtime_is_ending(&f));
    step(&f, VR_STATE_READY);
    step(&f, VR_STATE_FOCUSED);
    CHECK(!vr_fsm_runtime_is_ending(&f));
    step(&f, VR_STATE_STOPPING);
    CHECK(vr_fsm_runtime_is_ending(&f));

    VrSessionFsm g;
    vr_fsm_init(&g);
    step(&g, VR_STATE_LOSS_PENDING);
    CHECK(vr_fsm_runtime_is_ending(&g));
    VrSessionFsm h;
    vr_fsm_init(&h);
    step(&h, VR_STATE_EXITING);
    CHECK(vr_fsm_runtime_is_ending(&h));
    CHECK(!vr_fsm_runtime_is_ending(NULL));
}

static void test_null_safety(void)
{
    vr_fsm_init(NULL);
    CHECK(vr_fsm_on_state_changed(NULL, VR_STATE_READY) == VR_ACTION_NONE);
    vr_fsm_session_begun(NULL);
    vr_fsm_session_ended(NULL);
    CHECK(!vr_fsm_should_request_exit(NULL));
    CHECK(!vr_fsm_frames_allowed(NULL));
    CHECK(!vr_fsm_is_visible(NULL));
    CHECK(!vr_fsm_is_focused(NULL));
}

int main(void)
{
    test_normal_startup_sequence();
    test_ready_only_begins_once();
    test_runtime_initiated_stop();
    test_stopping_before_session_began();
    test_loss_pending_never_ends_session();
    test_exiting_alone_requests_quit_once();
    test_game_initiated_exit_request();
    test_no_exit_request_once_runtime_is_ending();
    test_runtime_is_ending();
    test_null_safety();

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("vr_session_fsm_test: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Wire the test into the build and the manifest**

```bash
git apply --whitespace=nowarn - <<'PATCH'
diff --git a/build.zig b/build.zig
index d8f8128..441f1b7 100644
--- a/build.zig
+++ b/build.zig
@@ -907,6 +907,7 @@ fn configureRenderQueue3DTests(
     );
     vr_pure_tests.dependOn(addPureCTest(b, target, optimize, c_flags, "vr_log_test", "tests/vr_log_test.c", "PROJECTS/ROLLER/vr_log.c"));
     vr_pure_tests.dependOn(addPureCTest(b, target, optimize, c_flags, "vr_ext_list_test", "tests/vr_ext_list_test.c", "PROJECTS/ROLLER/vr_ext_list.c"));
+    vr_pure_tests.dependOn(addPureCTest(b, target, optimize, c_flags, "vr_session_fsm_test", "tests/vr_session_fsm_test.c", "PROJECTS/ROLLER/vr_session_fsm.c"));
 
     const test_step = b.step("test", "Run focused unit tests and optional seam checks");
     test_step.dependOn(render_queue_tests);
diff --git a/roller-core.srclist b/roller-core.srclist
index cf1d3a4..ecbc6a9 100644
--- a/roller-core.srclist
+++ b/roller-core.srclist
@@ -8,6 +8,7 @@
 # and test-only code that is not linked into roller-core.
 EXCLUDE|PROJECTS/ROLLER/roller.c
 EXCLUDE|PROJECTS/ROLLER/roller_web.c
+EXCLUDE|PROJECTS/ROLLER/vr_session_fsm.c
 EXCLUDE|PROJECTS/ROLLER/vr_ext_list.c
 EXCLUDE|PROJECTS/ROLLER/vr_log.c
 EXCLUDE|PROJECTS/ROLLER/vr_mvp_math_test.c
PATCH
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
zig build test-vr-pure 2>&1 | grep -E "error" | head -3
```

Expected: `error: failed to check cache: '.../PROJECTS/ROLLER/vr_session_fsm.c' file_hash FileNotFound`.

- [ ] **Step 4: Write the implementation**

Create `PROJECTS/ROLLER/vr_session_fsm.h`:

```c
/* PROJECTS/ROLLER/vr_session_fsm.h
 *
 * OpenXR session lifecycle as a pure state machine (no OpenXR types), so the
 * decisions can be unit-tested. vr_runtime.c feeds it XrSessionState changes and
 * carries out the returned actions (xrBeginSession, xrEndSession, telling the
 * game to quit). VrSessionState uses the same numeric values as XrSessionState;
 * vr_runtime.c asserts that at compile time. */
#ifndef VR_SESSION_FSM_H
#define VR_SESSION_FSM_H

#include <stdbool.h>

typedef enum {
    VR_STATE_UNKNOWN      = 0,
    VR_STATE_IDLE         = 1,
    VR_STATE_READY        = 2,
    VR_STATE_SYNCHRONIZED = 3,
    VR_STATE_VISIBLE      = 4,
    VR_STATE_FOCUSED      = 5,
    VR_STATE_STOPPING     = 6,
    VR_STATE_LOSS_PENDING = 7,
    VR_STATE_EXITING      = 8
} VrSessionState;

/* Bit flags returned by vr_fsm_on_state_changed(); the caller performs each set one. */
enum {
    VR_ACTION_NONE          = 0,
    VR_ACTION_BEGIN_SESSION = 1 << 0, /* call xrBeginSession, then vr_fsm_session_begun() */
    VR_ACTION_END_SESSION   = 1 << 1, /* call xrEndSession, then vr_fsm_session_ended() */
    VR_ACTION_REQUEST_QUIT  = 1 << 2  /* the runtime wants us gone: make the game quit cleanly */
};

typedef struct {
    VrSessionState state;
    bool session_running; /* between a successful xrBeginSession and xrEndSession */
    bool quit_requested;  /* the runtime asked us to quit; REQUEST_QUIT already returned once */
    bool exit_requested;  /* the game asked to quit; xrRequestExitSession already due once */
} VrSessionFsm;

void vr_fsm_init(VrSessionFsm *fsm);

/* Records a new session state and returns the VR_ACTION_* flags to carry out.
 * READY starts the session (once). STOPPING ends a running session and asks the
 * game to quit. EXITING and LOSS_PENDING ask the game to quit but never end the
 * session (xrEndSession is only valid from STOPPING). Each quit is requested
 * once, however many such events arrive. */
unsigned int vr_fsm_on_state_changed(VrSessionFsm *fsm, VrSessionState new_state);

void vr_fsm_session_begun(VrSessionFsm *fsm);
void vr_fsm_session_ended(VrSessionFsm *fsm);

/* Call when the game wants to quit. Returns true exactly once, and only while the
 * session is running and the runtime has not already started ending it, meaning
 * "call xrRequestExitSession now". */
bool vr_fsm_should_request_exit(VrSessionFsm *fsm);

/* True once the runtime has started ending the session (STOPPING, EXITING or
 * LOSS_PENDING): there is nothing left to request or wait for. */
bool vr_fsm_runtime_is_ending(const VrSessionFsm *fsm);

/* Frames may be waited on/begun/ended only while the session is running. */
bool vr_fsm_frames_allowed(const VrSessionFsm *fsm);
/* The app's frames are visible to the user (VISIBLE or FOCUSED). */
bool vr_fsm_is_visible(const VrSessionFsm *fsm);
/* The app has input focus (FOCUSED); input must read zero otherwise. */
bool vr_fsm_is_focused(const VrSessionFsm *fsm);

#endif /* VR_SESSION_FSM_H */
```

Create `PROJECTS/ROLLER/vr_session_fsm.c`:

```c
/* PROJECTS/ROLLER/vr_session_fsm.c -- see vr_session_fsm.h */
#include "vr_session_fsm.h"

void vr_fsm_init(VrSessionFsm *fsm)
{
    if (!fsm)
        return;
    fsm->state = VR_STATE_UNKNOWN;
    fsm->session_running = false;
    fsm->quit_requested = false;
    fsm->exit_requested = false;
}

unsigned int vr_fsm_on_state_changed(VrSessionFsm *fsm, VrSessionState new_state)
{
    unsigned int actions = VR_ACTION_NONE;
    if (!fsm)
        return actions;

    fsm->state = new_state;
    switch (new_state) {
    case VR_STATE_READY:
        if (!fsm->session_running)
            actions |= VR_ACTION_BEGIN_SESSION;
        break;
    case VR_STATE_STOPPING:
        if (fsm->session_running)
            actions |= VR_ACTION_END_SESSION;
        if (!fsm->quit_requested) {
            fsm->quit_requested = true;
            actions |= VR_ACTION_REQUEST_QUIT;
        }
        break;
    case VR_STATE_EXITING:
    case VR_STATE_LOSS_PENDING:
        if (!fsm->quit_requested) {
            fsm->quit_requested = true;
            actions |= VR_ACTION_REQUEST_QUIT;
        }
        break;
    default:
        break;
    }
    return actions;
}

void vr_fsm_session_begun(VrSessionFsm *fsm)
{
    if (fsm)
        fsm->session_running = true;
}

void vr_fsm_session_ended(VrSessionFsm *fsm)
{
    if (fsm)
        fsm->session_running = false;
}

bool vr_fsm_should_request_exit(VrSessionFsm *fsm)
{
    if (!fsm || !fsm->session_running || fsm->exit_requested)
        return false;
    if (vr_fsm_runtime_is_ending(fsm))
        return false;
    fsm->exit_requested = true;
    return true;
}

bool vr_fsm_runtime_is_ending(const VrSessionFsm *fsm)
{
    return fsm && (fsm->state == VR_STATE_STOPPING || fsm->state == VR_STATE_EXITING ||
                   fsm->state == VR_STATE_LOSS_PENDING);
}

bool vr_fsm_frames_allowed(const VrSessionFsm *fsm)
{
    return fsm && fsm->session_running;
}

bool vr_fsm_is_visible(const VrSessionFsm *fsm)
{
    return fsm && fsm->session_running &&
           (fsm->state == VR_STATE_VISIBLE || fsm->state == VR_STATE_FOCUSED);
}

bool vr_fsm_is_focused(const VrSessionFsm *fsm)
{
    return fsm && fsm->session_running && fsm->state == VR_STATE_FOCUSED;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
zig build test-vr-pure
python3 tools/check_roller_core_manifest.py
```

Expected: `vr_session_fsm_test: ALL PASS`, the two earlier suites still pass, and the manifest check passes.

- [ ] **Step 6: Commit**

```bash
git add PROJECTS/ROLLER/vr_session_fsm.c PROJECTS/ROLLER/vr_session_fsm.h tests/vr_session_fsm_test.c build.zig roller-core.srclist
git commit -m "vr: add vr_session_fsm, the OpenXR session lifecycle as a pure state machine

Decides when to begin/end the session and when to ask the game to quit,
including the odd orders (STOPPING before the session began, LOSS_PENDING,
EXITING alone, repeated events). Unit-tested; vr_runtime.c will use it."
```

---

### Task 5: `vr_diag` and `roller-vr.log`, plus the Monado run script

**Files:**
- Create: `PROJECTS/ROLLER/vr_diag.h`, `PROJECTS/ROLLER/vr_diag.c`, `/config/projects/fatal/tools/vr-monado-run.sh` (outside the ROLLER repo)
- Modify: `PROJECTS/ROLLER/roller.c`, `PROJECTS/ROLLER/3d.c`, `build.zig`, `roller-core.srclist`

**Interfaces:**
- Consumes: `vr_log_open/close/line` (Task 2).
- Produces (used by Tasks 7 and 8):
  - `bool vr_diag_init(void)` — opens `roller-vr.log` in the executable's directory, captures stdout, stderr and `SDL_Log` output; returns false (logging stays on the console) if the file cannot be created.
  - `void vr_diag_fatal(const char *fmt, ...)` — logs the message, shows a message box on Windows only, exits with status 1; never returns.
  - `InitSDL()` calls `vr_diag_init()` in VR mode right after `SDL_Init()`; a failed `vr_init()` now ends in `vr_diag_fatal()` instead of `ErrorBoxExit()` (which exited with status 0).
  - `/config/projects/fatal/tools/vr-monado-run.sh <roller-vr binary> [--frames N] [--runtime-exit-after N] [--no-validation]` — copies the binary to `fatal-gpu:/root/vr-test/`, starts Xorg :1 and `monado-service` if they are not running, runs the game under the OpenXR and Vulkan validation layers, fetches `roller-vr.log`, and prints exit status, negotiation lines, lifecycle lines and validation counts. Exit status is the game's.

- [ ] **Step 1: Create the `vr_diag` module**

Create `PROJECTS/ROLLER/vr_diag.h`:

```c
/* PROJECTS/ROLLER/vr_diag.h
 *
 * roller-vr diagnostics that need SDL: opens roller-vr.log next to the
 * executable (see vr_log.h), routes SDL_Log output into it, and provides the
 * fatal-error exit used by the VR startup path. */
#ifndef VR_DIAG_H
#define VR_DIAG_H

#include <stdbool.h>

/* Opens roller-vr.log in the executable's directory and captures stdout, stderr
 * and SDL_Log output in it. Returns false (logging stays on the console) if the
 * file cannot be created. Call once, early, after SDL_Init(). */
bool vr_diag_init(void);

/* Logs the message, shows it in a message box on Windows (where a GUI process
 * has no visible console), then exits the process with status 1. Never returns. */
void vr_diag_fatal(const char *fmt, ...);

#endif /* VR_DIAG_H */
```

Create `PROJECTS/ROLLER/vr_diag.c`:

```c
/* PROJECTS/ROLLER/vr_diag.c -- see vr_diag.h */
#include "vr_diag.h"
#include "vr_log.h"

#include <SDL3/SDL.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static const char *sdl_priority_name(SDL_LogPriority priority)
{
    switch (priority) {
    case SDL_LOG_PRIORITY_VERBOSE:  return "VERBOSE";
    case SDL_LOG_PRIORITY_DEBUG:    return "DEBUG";
    case SDL_LOG_PRIORITY_INFO:     return "INFO";
    case SDL_LOG_PRIORITY_WARN:     return "WARN";
    case SDL_LOG_PRIORITY_ERROR:    return "ERROR";
    case SDL_LOG_PRIORITY_CRITICAL: return "CRITICAL";
    default:                        return "LOG";
    }
}

static void SDLCALL vr_diag_sdl_log(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    (void)userdata;
    vr_log_line(sdl_priority_name(priority), "[sdl:%d] %s", category, message ? message : "");
}

bool vr_diag_init(void)
{
    const char *base = SDL_GetBasePath();
    char path[1024];
    snprintf(path, sizeof(path), "%sroller-vr.log", base ? base : "");

    if (!vr_log_open(path)) {
        fprintf(stderr, "vr_diag: could not create %s; logging to the console instead\n", path);
        return false;
    }
    SDL_SetLogOutputFunction(vr_diag_sdl_log, NULL);
    vr_log_line("INFO", "roller-vr log opened: %s", path);
    return true;
}

void vr_diag_fatal(const char *fmt, ...)
{
    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt ? fmt : "fatal error", args);
    va_end(args);

    vr_log_line("FATAL", "%s", message);
#if defined(_WIN32)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ROLLER VR", message, NULL);
#endif
    vr_log_close();
    exit(1);
}
```

- [ ] **Step 2: Hook it into startup and the VR source list**

```bash
git apply --whitespace=nowarn - <<'PATCH'
diff --git a/PROJECTS/ROLLER/3d.c b/PROJECTS/ROLLER/3d.c
index 87e7cd4..8684798 100644
--- a/PROJECTS/ROLLER/3d.c
+++ b/PROJECTS/ROLLER/3d.c
@@ -33,6 +33,7 @@
 #endif
 #if defined(ROLLER_VR_BUILD)
 #include "vr_runtime.h"
+#include "vr_diag.h"
 #endif
 #include <SDL3/SDL.h>
 #if defined(__EMSCRIPTEN__)
@@ -3209,10 +3210,8 @@ int main(int argc, const char **argv, const char **envp)
    * (see vr_init()'s own comment in vr_runtime.c), so this is the earliest
    * point that device is guaranteed to exist. */
   if (g_bVRMode) {
-    if (!vr_init()) {
-      ErrorBoxExit("Couldn't initialize VR runtime");
-      return 1;
-    }
+    if (!vr_init())
+      vr_diag_fatal("Couldn't initialize the VR runtime; see roller-vr.log next to the executable.");
   }
 #endif
 
diff --git a/PROJECTS/ROLLER/roller.c b/PROJECTS/ROLLER/roller.c
index 0799e24..f8ebe65 100644
--- a/PROJECTS/ROLLER/roller.c
+++ b/PROJECTS/ROLLER/roller.c
@@ -1,4 +1,7 @@
 #include "roller.h"
+#if defined(ROLLER_VR_BUILD)
+#include "vr_diag.h"
+#endif
 #include "scene_render_gpu.h"
 #include "crt_filter.h"
 #include "rollersound.h"
@@ -779,6 +782,10 @@ int InitSDL(char *whiplash_root, const char *midi_root)
     return 1;
   }
   ROLLERInstallPlatformLogSink();
+#if defined(ROLLER_VR_BUILD)
+  if (g_bVRMode)
+    vr_diag_init(); // roller-vr.log next to the exe: stdout, stderr and SDL_Log
+#endif
 
 #if defined(IS_WASM)
   SDL_strlcpy(whiplash_root, "/persist", 260);
diff --git a/build.zig b/build.zig
index 441f1b7..69b74b0 100644
--- a/build.zig
+++ b/build.zig
@@ -285,7 +285,11 @@ pub fn build(b: *std.Build) void {
         });
         vr_exe_mod.addCSourceFiles(.{
             .flags = c_flags,
-            .files = &.{"PROJECTS/ROLLER/vr_runtime.c"},
+            .files = &.{
+                "PROJECTS/ROLLER/vr_runtime.c",
+                "PROJECTS/ROLLER/vr_diag.c",
+                "PROJECTS/ROLLER/vr_log.c",
+            },
         });
 
         const vr_exe = b.addExecutable(.{
diff --git a/roller-core.srclist b/roller-core.srclist
index ecbc6a9..17bd1da 100644
--- a/roller-core.srclist
+++ b/roller-core.srclist
@@ -8,6 +8,7 @@
 # and test-only code that is not linked into roller-core.
 EXCLUDE|PROJECTS/ROLLER/roller.c
 EXCLUDE|PROJECTS/ROLLER/roller_web.c
+EXCLUDE|PROJECTS/ROLLER/vr_diag.c
 EXCLUDE|PROJECTS/ROLLER/vr_session_fsm.c
 EXCLUDE|PROJECTS/ROLLER/vr_ext_list.c
 EXCLUDE|PROJECTS/ROLLER/vr_log.c
PATCH
```

- [ ] **Step 3: Build the VR executable**

```bash
zig build -Dvr -p /tmp/roller-vr-out 2>&1 | tail -20; echo "exit=${PIPESTATUS[0]}"
ls -la /tmp/roller-vr-out/bin
python3 tools/check_roller_core_manifest.py
```

Expected: exit 0 after about 10 minutes (cold cache); `bin/roller` and `bin/roller-vr` exist; the manifest check passes.

- [ ] **Step 4: Create the Monado run script**

```bash
mkdir -p /config/projects/fatal/tools
```

Create `/config/projects/fatal/tools/vr-monado-run.sh` and make it executable (`chmod +x`):

```bash
#!/bin/bash
# Runs roller-vr on the fatal-gpu host against Monado (simulated HMD) with the OpenXR
# and Vulkan validation layers, then prints the lifecycle lines and validation counts.
#
# Usage: vr-monado-run.sh <roller-vr binary> [--frames N] [--runtime-exit-after N] [--no-validation]
#   --frames N               game-initiated quit after N presented frames (default 300)
#   --runtime-exit-after N   ask the runtime to end the session after N frames instead
#   --no-validation          skip the validation layers
# Exit status: the game's exit status. Files: /tmp/vr-monado-run.<pid>.log (the roller-vr.log).
set -euo pipefail

BIN="${1:?usage: vr-monado-run.sh <roller-vr binary> [--frames N] [--runtime-exit-after N] [--no-validation]}"
shift
FRAMES=300
RUNTIME_EXIT=0
VALIDATION=1
while [ $# -gt 0 ]; do
    case "$1" in
        --frames) FRAMES="$2"; shift 2 ;;
        --runtime-exit-after) RUNTIME_EXIT="$2"; shift 2 ;;
        --no-validation) VALIDATION=0; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

HOST=fatal-gpu
REMOTE_DIR=/root/vr-test
LOCAL_LOG="/tmp/vr-monado-run.$$.log"

ssh "$HOST" "mkdir -p $REMOTE_DIR"
scp -q "$BIN" "$HOST:$REMOTE_DIR/roller-vr"

set +e
ssh "$HOST" bash -s -- "$FRAMES" "$RUNTIME_EXIT" "$VALIDATION" <<'REMOTE'
FRAMES="$1"; RUNTIME_EXIT="$2"; VALIDATION="$3"
cd /root/vr-test || exit 90

# Real Xorg with DRI3 (Xvfb has none), started once; left running for reuse.
if [ ! -S /tmp/.X11-unix/X1 ]; then
    setsid nohup Xorg :1 -config /root/xorg.conf -noreset -novtswitch -sharevts >/root/xorg-vr.log 2>&1 &
    for _ in $(seq 1 30); do [ -S /tmp/.X11-unix/X1 ] && break; sleep 1; done
fi
# monado-service needs a non-null stdin; started once and left running.
if ! pgrep -x monado-service >/dev/null; then
    DISPLAY=:1 setsid nohup bash -c 'sleep infinity | monado-service' >/root/monado-vr.log 2>&1 &
    sleep 3
fi

export DISPLAY=:1 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy
if [ "$VALIDATION" = "1" ]; then
    export XR_ENABLE_API_LAYERS=XR_APILAYER_LUNARG_core_validation
    export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
    export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
fi
[ "$RUNTIME_EXIT" != "0" ] && export ROLLER_VR_TEST_RUNTIME_EXIT_AFTER="$RUNTIME_EXIT"

rm -f roller-vr.log
timeout 180 ./roller-vr --vr --vr-frames "$FRAMES" \
    --whiplash-root /root/roller_v2 --track-path /root/roller_v2/fatdata/TRACK1.TRK </dev/null >/dev/null 2>&1
echo "$?" > /root/vr-test/last.exit
REMOTE
set -e

scp -q "$HOST:$REMOTE_DIR/roller-vr.log" "$LOCAL_LOG"
EXIT_CODE="$(ssh "$HOST" cat $REMOTE_DIR/last.exit)"

echo "=== exit status: $EXIT_CODE   (log: $LOCAL_LOG)"
echo "=== runtime / Vulkan negotiation"
grep -E "OpenXR runtime:|using XR_KHR_vulkan|falling back|runtime-required Vulkan|requesting [0-9]|runtime swapchain formats|view count" "$LOCAL_LOG" || true
echo "=== lifecycle"
grep -E "session state|xrBeginSession|xrEndSession|xrRequestExitSession|session ended|did not report STOPPING|test hook|asking the game|vr_runtime: shutdown|ERROR .* failed" "$LOCAL_LOG" || true
echo "=== validation counts (raw grep -c of log lines)"
for pat in "VUID-VkImageMemoryBarrier-None-09120" "VUID-VkImageMemoryBarrier-image-09118" "SYNC-HAZARD" "Validation Error" "XR_ERROR"; do
    printf "%-45s %s\n" "$pat" "$(grep -c -- "$pat" "$LOCAL_LOG" || true)"
done
exit "$EXIT_CODE"
```

- [ ] **Step 5: Run the game once on Monado and check the log**

```bash
/config/projects/fatal/tools/vr-monado-run.sh /tmp/roller-vr-out/bin/roller-vr --frames 100
```

Expected: the script finishes with `=== exit status: 0`. The log it fetched (path printed on that line) starts with the line below and contains SDL lines prefixed `[sdl:`:

```text
[0.000s] INFO roller-vr log opened: /root/vr-test/roller-vr.log
```

The validation counts at this point are the **Phase 2 baseline**: `VUID-VkImageMemoryBarrier-None-09120` and `VUID-VkImageMemoryBarrier-image-09118` are non-zero (Phase 2 recorded 12 raw lines each in a 300-frame run) and there are no negotiation lines yet. Task 7 makes the counts zero.

- [ ] **Step 6: Commit**

```bash
git add PROJECTS/ROLLER/vr_diag.c PROJECTS/ROLLER/vr_diag.h PROJECTS/ROLLER/roller.c PROJECTS/ROLLER/3d.c build.zig roller-core.srclist
git commit -m "vr: write roller-vr.log next to the executable and exit non-zero on VR startup failure

vr_diag captures stdout, stderr and SDL_Log in the log (the Windows exe is
a GUI process with no console) and provides vr_diag_fatal(), which shows a
message box on Windows and exits with status 1. ErrorBoxExit() exited 0."
```

---

### Task 6: `SDL_GPUVulkanOptions` in the SDL fork

**Files:**
- Modify (fork, `/root/sdl-vr-fork`, branch `vr-gpu-properties`): `include/SDL3/SDL_gpu.h`, `src/gpu/vulkan/SDL_gpu_vulkan.c`
- Create (ROLLER): `tests/vr_vulkan_options_test.c`
- Modify (ROLLER): `build.zig`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces (used by Task 7):
  - `SDL_GPUVulkanOptions` with the exact layout of SDL 3.4's struct (`vulkan_api_version`, `feature_list`, `vulkan_10_physical_device_features`, `device_extension_count`, `device_extension_names`, `instance_extension_count`, `instance_extension_names`); this fork honours only the version and the two extension lists and logs a warning if `feature_list` or `vulkan_10_physical_device_features` is set.
  - `SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER` (`"SDL.gpu.device.create.vulkan.options"`), read only during `SDL_CreateGPUDeviceWithProperties()`.
  - An unsupported requested extension makes device creation fail with a message that names it (`Failed to initialize Vulkan: Requested Vulkan instance extension X is not supported`, and the device-extension equivalent).
  - `zig build -Dvr` installs `bin/vr_vulkan_options_test` (exit 0 = pass, 1 = fail, 77 = skipped because no Vulkan GPU).

- [ ] **Step 1: Write the failing test**

Create `tests/vr_vulkan_options_test.c`:

```c
/* tests/vr_vulkan_options_test.c
 *
 * Checks the SDL fork's SDL_GPUVulkanOptions plumbing (SDL_PROP_GPU_DEVICE_CREATE_
 * VULKAN_OPTIONS_POINTER) against a real Vulkan driver, so it must run on a
 * machine with a Vulkan GPU and a display, for example `fatal-gpu` under Xorg :1:
 *
 *   DISPLAY=:1 SDL_VIDEODRIVER=x11 ./vr_vulkan_options_test
 *
 * Exit status: 0 = all checks passed, 1 = a check failed, 77 = skipped (no usable
 * Vulkan device, as on the dev LXC, which has no GPU). Built by `zig build -Dvr`
 * into <prefix>/bin. */
#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define VK_API_1_1 ((1u << 22) | (1u << 12))

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static SDL_GPUDevice *create_device(const SDL_GPUVulkanOptions *options)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, false);
    if (options)
        SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, (void *)options);
    SDL_GPUDevice *device = SDL_CreateGPUDeviceWithProperties(props);
    SDL_DestroyProperties(props);
    return device;
}

int main(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("SKIP: SDL_Init(VIDEO) failed: %s\n", SDL_GetError());
        return 77;
    }

    /* Baseline: without options a device must come up, otherwise nothing below means anything. */
    SDL_GPUDevice *baseline = create_device(NULL);
    if (!baseline) {
        printf("SKIP: no usable Vulkan GPU device: %s\n", SDL_GetError());
        SDL_Quit();
        return 77;
    }
    printf("baseline device driver: %s\n", SDL_GetGPUDeviceDriver(baseline));
    SDL_DestroyGPUDevice(baseline);

    /* Real options: Vulkan 1.1 plus the external-memory extensions OpenXR runtimes commonly list. */
    static const char *instance_names[] = { "VK_KHR_external_memory_capabilities" };
    static const char *device_names[] = { "VK_KHR_external_memory" };
    SDL_GPUVulkanOptions good;
    SDL_zero(good);
    good.vulkan_api_version = VK_API_1_1;
    good.instance_extension_count = 1;
    good.instance_extension_names = instance_names;
    good.device_extension_count = 1;
    good.device_extension_names = device_names;
    SDL_GPUDevice *device = create_device(&good);
    CHECK(device != NULL);
    if (device) {
        SDL_PropertiesID props = SDL_GetGPUDeviceProperties(device);
        CHECK(SDL_GetPointerProperty(props, SDL_PROP_GPU_DEVICE_VULKAN_INSTANCE_POINTER, NULL) != NULL);
        CHECK(SDL_GetPointerProperty(props, SDL_PROP_GPU_DEVICE_VULKAN_DEVICE_POINTER, NULL) != NULL);
        SDL_DestroyGPUDevice(device);
    } else {
        printf("  error: %s\n", SDL_GetError());
    }

    /* An unsupported instance extension must fail device creation and name the extension. */
    static const char *bogus_instance[] = { "VK_FAKE_not_a_real_instance_extension" };
    SDL_GPUVulkanOptions bad_instance;
    SDL_zero(bad_instance);
    bad_instance.instance_extension_count = 1;
    bad_instance.instance_extension_names = bogus_instance;
    device = create_device(&bad_instance);
    CHECK(device == NULL);
    CHECK(strstr(SDL_GetError(), "VK_FAKE_not_a_real_instance_extension") != NULL);
    if (device)
        SDL_DestroyGPUDevice(device);

    /* Same for an unsupported device extension. */
    static const char *bogus_device[] = { "VK_FAKE_not_a_real_device_extension" };
    SDL_GPUVulkanOptions bad_device;
    SDL_zero(bad_device);
    bad_device.device_extension_count = 1;
    bad_device.device_extension_names = bogus_device;
    device = create_device(&bad_device);
    CHECK(device == NULL);
    CHECK(strstr(SDL_GetError(), "VK_FAKE_not_a_real_device_extension") != NULL);
    if (device)
        SDL_DestroyGPUDevice(device);

    /* Not checked here: that vulkan_api_version reaches vkCreateInstance. A Vulkan 1.1+
     * implementation must never reject a too-high apiVersion, so device creation cannot show
     * it. It is verified end to end instead: with API 1.1 requested, the Vulkan validation
     * layer no longer reports VUID-VkImageMemoryBarrier-None-09120 (see the Monado run). */

    /* Options with nothing requested behave like no options. */
    SDL_GPUVulkanOptions empty;
    SDL_zero(empty);
    device = create_device(&empty);
    CHECK(device != NULL);
    if (device)
        SDL_DestroyGPUDevice(device);

    SDL_Quit();
    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("vr_vulkan_options_test: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Build it as part of the VR build**

```bash
git apply --whitespace=nowarn - <<'PATCH'
diff --git a/build.zig b/build.zig
index 69b74b0..cad1aa6 100644
--- a/build.zig
+++ b/build.zig
@@ -331,6 +331,29 @@ pub fn build(b: *std.Build) void {
             },
         }
         b.installArtifact(vr_exe);
+
+        // Check of the SDL fork's SDL_GPUVulkanOptions plumbing. Needs a Vulkan GPU and a
+        // display (fatal-gpu), so it is built into <prefix>/bin to be copied there and is
+        // not part of `zig build test`.
+        if (b.lazyDependency("sdl_vr", .{
+            .target = target,
+            .optimize = optimize,
+            .sanitize_c = .off,
+            .lto = .none,
+        })) |sdl_vr| {
+            const vk_options_mod = b.createModule(.{
+                .target = target,
+                .optimize = optimize,
+                .link_libc = true,
+            });
+            vk_options_mod.addCSourceFile(.{ .file = b.path("tests/vr_vulkan_options_test.c"), .flags = c_flags });
+            vk_options_mod.addIncludePath(sdl_vr.builder.path("include"));
+            vk_options_mod.linkLibrary(sdl_vr.artifact("SDL3"));
+            b.installArtifact(b.addExecutable(.{
+                .name = "vr_vulkan_options_test",
+                .root_module = vk_options_mod,
+            }));
+        }
     }
 
     if (python_checks) {
PATCH
```

- [ ] **Step 3: Verify the test fails against the unpatched fork**

```bash
zig cc -fsyntax-only -I /root/sdl-vr-fork/include tests/vr_vulkan_options_test.c 2>&1 | grep error
```

Expected: `tests/vr_vulkan_options_test.c:29:43: error: unknown type name 'SDL_GPUVulkanOptions'`.

- [ ] **Step 4: Patch the fork**

The patch adds the struct and property to `SDL_gpu.h`; in `SDL_gpu_vulkan.c` it stores the caller's options pointer on the renderer during device creation, raises the instance `apiVersion` and appends the extra instance extensions in `VULKAN_INTERNAL_CreateInstance`, appends the extra device extensions in `VULKAN_INTERNAL_CreateLogicalDevice`, checks each requested extension first so the error can name it, and keeps the specific cause when `VULKAN_CreateDevice` reports failure (Finding 5).

```bash
cd /root/sdl-vr-fork
git status --short          # expect: nothing
git apply --whitespace=nowarn - <<'PATCH'
--- a/src/gpu/vulkan/SDL_gpu_vulkan.c
+++ b/src/gpu/vulkan/SDL_gpu_vulkan.c
@@ -1140,6 +1140,9 @@
     bool preferLowPower;
     Uint32 allowedFramesInFlight;
 
+    // Borrowed from the caller (SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER); only valid during device creation.
+    const SDL_GPUVulkanOptions *vulkanOptions;
+
     VulkanExtensions supports;
     bool supportsDebugUtils;
     bool supportsColorspace;
@@ -11169,6 +11172,89 @@
     return layerFound;
 }
 
+// Adds the generic failure text without discarding the specific reason an inner
+// function already stored with SDL_SetError (for example which requested extension is missing).
+static void VULKAN_INTERNAL_SetErrorKeepingCause(const char *what)
+{
+    char cause[512];
+    SDL_strlcpy(cause, SDL_GetError(), sizeof(cause));
+    if (cause[0]) {
+        SDL_SetError("%s: %s", what, cause);
+    } else {
+        SDL_SetError("%s", what);
+    }
+}
+
+static bool VULKAN_INTERNAL_NameInList(const char *name, const char *const *list, Uint32 count)
+{
+    Uint32 i;
+    for (i = 0; i < count; i += 1) {
+        if (list[i] && SDL_strcmp(name, list[i]) == 0) {
+            return true;
+        }
+    }
+    return false;
+}
+
+// Returns the first name in `names` the Vulkan instance does not offer, or NULL if all are supported.
+static const char *VULKAN_INTERNAL_FindUnsupportedInstanceExtension(const char *const *names, Uint32 count)
+{
+    Uint32 availableCount = 0, i;
+    VkExtensionProperties *available;
+    const char *missing = NULL;
+
+    vkEnumerateInstanceExtensionProperties(NULL, &availableCount, NULL);
+    available = (VkExtensionProperties *)SDL_malloc(availableCount * sizeof(VkExtensionProperties));
+    if (!available) {
+        return NULL; // let vkCreateInstance report it
+    }
+    vkEnumerateInstanceExtensionProperties(NULL, &availableCount, available);
+    for (i = 0; i < count; i += 1) {
+        if (names[i] && !SupportsInstanceExtension(names[i], available, availableCount)) {
+            missing = names[i];
+            break;
+        }
+    }
+    SDL_free(available);
+    return missing;
+}
+
+// Returns the first name in `names` the physical device does not offer, or NULL if all are supported.
+static const char *VULKAN_INTERNAL_FindUnsupportedDeviceExtension(
+    VulkanRenderer *renderer,
+    const char *const *names,
+    Uint32 count)
+{
+    Uint32 availableCount = 0, i, j;
+    VkExtensionProperties *available;
+    const char *missing = NULL;
+
+    renderer->vkEnumerateDeviceExtensionProperties(renderer->physicalDevice, NULL, &availableCount, NULL);
+    available = (VkExtensionProperties *)SDL_malloc(availableCount * sizeof(VkExtensionProperties));
+    if (!available) {
+        return NULL; // let vkCreateDevice report it
+    }
+    renderer->vkEnumerateDeviceExtensionProperties(renderer->physicalDevice, NULL, &availableCount, available);
+    for (i = 0; i < count; i += 1) {
+        bool found = false;
+        if (!names[i]) {
+            continue;
+        }
+        for (j = 0; j < availableCount; j += 1) {
+            if (SDL_strcmp(names[i], available[j].extensionName) == 0) {
+                found = true;
+                break;
+            }
+        }
+        if (!found) {
+            missing = names[i];
+            break;
+        }
+    }
+    SDL_free(available);
+    return missing;
+}
+
 static Uint8 VULKAN_INTERNAL_CreateInstance(VulkanRenderer *renderer)
 {
     VkResult vulkanResult;
@@ -11179,6 +11265,9 @@
     Uint32 instanceExtensionCount;
     VkInstanceCreateInfo createInfo;
     static const char *layerNames[] = { "VK_LAYER_KHRONOS_validation" };
+    const SDL_GPUVulkanOptions *vulkanOptions = renderer->vulkanOptions;
+    Uint32 extraInstanceExtensionCount = 0;
+    Uint32 extraIndex;
 
     appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
     appInfo.pNext = NULL;
@@ -11187,6 +11276,14 @@
     appInfo.pEngineName = "SDLGPU";
     appInfo.engineVersion = SDL_VERSION;
     appInfo.apiVersion = VK_MAKE_VERSION(1, 0, 0);
+    if (vulkanOptions) {
+        if (vulkanOptions->vulkan_api_version > appInfo.apiVersion) {
+            appInfo.apiVersion = vulkanOptions->vulkan_api_version;
+        }
+        if (vulkanOptions->instance_extension_names) {
+            extraInstanceExtensionCount = vulkanOptions->instance_extension_count;
+        }
+    }
 
     createFlags = 0;
 
@@ -11208,7 +11305,7 @@
      */
     instanceExtensionNames = SDL_stack_alloc(
         const char *,
-        instanceExtensionCount + 4);
+        instanceExtensionCount + 4 + extraInstanceExtensionCount);
     SDL_memcpy((void *)instanceExtensionNames, originalInstanceExtensionNames, instanceExtensionCount * sizeof(const char *));
 
     // Core since 1.1
@@ -11221,6 +11318,23 @@
     createFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
 #endif
 
+    // Extensions the application asked for through SDL_GPUVulkanOptions (for example the ones an OpenXR runtime requires)
+    if (extraInstanceExtensionCount > 0) {
+        const char *missing = VULKAN_INTERNAL_FindUnsupportedInstanceExtension(
+            vulkanOptions->instance_extension_names,
+            extraInstanceExtensionCount);
+        if (missing) {
+            SDL_stack_free((char *)instanceExtensionNames);
+            SET_ERROR_AND_RETURN("Requested Vulkan instance extension %s is not supported", missing, false);
+        }
+        for (extraIndex = 0; extraIndex < extraInstanceExtensionCount; extraIndex += 1) {
+            const char *extra = vulkanOptions->instance_extension_names[extraIndex];
+            if (extra && !VULKAN_INTERNAL_NameInList(extra, instanceExtensionNames, instanceExtensionCount)) {
+                instanceExtensionNames[instanceExtensionCount++] = extra;
+            }
+        }
+    }
+
     if (!VULKAN_INTERNAL_CheckInstanceExtensions(
             instanceExtensionNames,
             instanceExtensionCount,
@@ -11543,6 +11657,9 @@
     VkPhysicalDeviceFeatures haveDeviceFeatures;
     VkPhysicalDevicePortabilitySubsetFeaturesKHR portabilityFeatures;
     const char **deviceExtensions;
+    const SDL_GPUVulkanOptions *vulkanOptions = renderer->vulkanOptions;
+    Uint32 extraDeviceExtensionCount = 0;
+    Uint32 extraIndex;
 
     VkDeviceQueueCreateInfo queueCreateInfo;
     float queuePriority = 1.0f;
@@ -11611,12 +11728,31 @@
     deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
     deviceCreateInfo.enabledLayerCount = 0;
     deviceCreateInfo.ppEnabledLayerNames = NULL;
+    if (vulkanOptions && vulkanOptions->device_extension_names) {
+        extraDeviceExtensionCount = vulkanOptions->device_extension_count;
+    }
+    if (extraDeviceExtensionCount > 0) {
+        // Extensions the application asked for through SDL_GPUVulkanOptions (for example the ones an OpenXR runtime requires)
+        const char *missing = VULKAN_INTERNAL_FindUnsupportedDeviceExtension(
+            renderer,
+            vulkanOptions->device_extension_names,
+            extraDeviceExtensionCount);
+        if (missing) {
+            SET_ERROR_AND_RETURN("Requested Vulkan device extension %s is not supported", missing, 0);
+        }
+    }
     deviceCreateInfo.enabledExtensionCount = GetDeviceExtensionCount(
         &renderer->supports);
     deviceExtensions = SDL_stack_alloc(
         const char *,
-        deviceCreateInfo.enabledExtensionCount);
+        deviceCreateInfo.enabledExtensionCount + extraDeviceExtensionCount);
     CreateDeviceExtensionArray(&renderer->supports, deviceExtensions);
+    for (extraIndex = 0; extraIndex < extraDeviceExtensionCount; extraIndex += 1) {
+        const char *extra = vulkanOptions->device_extension_names[extraIndex];
+        if (extra && !VULKAN_INTERNAL_NameInList(extra, deviceExtensions, deviceCreateInfo.enabledExtensionCount)) {
+            deviceExtensions[deviceCreateInfo.enabledExtensionCount++] = extra;
+        }
+    }
     deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions;
     deviceCreateInfo.pEnabledFeatures = &desiredDeviceFeatures;
 
@@ -11751,9 +11887,19 @@
     renderer->debugMode = debugMode;
     renderer->preferLowPower = preferLowPower;
     renderer->allowedFramesInFlight = 2;
+    renderer->vulkanOptions = (const SDL_GPUVulkanOptions *)SDL_GetPointerProperty(
+        props,
+        SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER,
+        NULL);
+    if (renderer->vulkanOptions &&
+        (renderer->vulkanOptions->feature_list || renderer->vulkanOptions->vulkan_10_physical_device_features)) {
+        SDL_LogWarn(
+            SDL_LOG_CATEGORY_GPU,
+            "SDL_GPUVulkanOptions::feature_list and vulkan_10_physical_device_features are not supported by this SDL fork and are ignored");
+    }
 
     if (!VULKAN_INTERNAL_PrepareVulkan(renderer)) {
-        SET_STRING_ERROR("Failed to initialize Vulkan!");
+        VULKAN_INTERNAL_SetErrorKeepingCause("Failed to initialize Vulkan");
         SDL_free(renderer);
         SDL_Vulkan_UnloadLibrary();
         return NULL;
@@ -11782,11 +11928,12 @@
 
     if (!VULKAN_INTERNAL_CreateLogicalDevice(
             renderer)) {
-        SET_STRING_ERROR("Failed to create logical device!");
+        VULKAN_INTERNAL_SetErrorKeepingCause("Failed to create logical device");
         SDL_free(renderer);
         SDL_Vulkan_UnloadLibrary();
         return NULL;
     }
+    renderer->vulkanOptions = NULL; // the caller's structure is only valid during creation
 
     // FIXME: just move this into this function
     result = (SDL_GPUDevice *)SDL_malloc(sizeof(SDL_GPUDevice));
--- a/include/SDL3/SDL_gpu.h
+++ b/include/SDL3/SDL_gpu.h
@@ -2163,6 +2163,14 @@
  * - `SDL_PROP_GPU_DEVICE_CREATE_D3D12_SEMANTIC_NAME_STRING`: the prefix to
  *   use for all vertex semantics, default is "TEXCOORD".
  *
+ * With the Vulkan renderer (ROLLER VR fork, same shape as SDL 3.4):
+ *
+ * - `SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER`: a pointer to an
+ *   SDL_GPUVulkanOptions structure, read only during device creation. Only
+ *   `vulkan_api_version` and the two extension lists are honoured in this
+ *   fork; `feature_list` and `vulkan_10_physical_device_features` are ignored
+ *   with a warning.
+ *
  * \param props the properties to use.
  * \returns a GPU context on success or NULL on failure; call SDL_GetError()
  *          for more information.
@@ -2187,6 +2195,29 @@
 #define SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN        "SDL.gpu.device.create.shaders.msl"
 #define SDL_PROP_GPU_DEVICE_CREATE_SHADERS_METALLIB_BOOLEAN   "SDL.gpu.device.create.shaders.metallib"
 #define SDL_PROP_GPU_DEVICE_CREATE_D3D12_SEMANTIC_NAME_STRING "SDL.gpu.device.create.d3d12.semantic"
+#define SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER     "SDL.gpu.device.create.vulkan.options"
+
+/**
+ * A structure specifying additional Vulkan options for device creation.
+ *
+ * Added by the ROLLER VR fork with the exact layout of the structure of the
+ * same name in SDL 3.4, so moving to 3.4 later needs no caller changes. The
+ * OpenXR runtime dictates which Vulkan API version and which instance/device
+ * extensions the VkInstance/VkDevice must enable (XR_KHR_vulkan_enable), but
+ * SDL_GPU creates them, so the caller passes the runtime's lists in here.
+ * Pass the structure through SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER;
+ * it and the arrays it points to only need to outlive device creation.
+ */
+typedef struct SDL_GPUVulkanOptions
+{
+    Uint32 vulkan_api_version; /**< The Vulkan API version to request for the instance (VK_MAKE_VERSION / VK_MAKE_API_VERSION); 0 or anything below 1.0 means 1.0. */
+    void *feature_list; /**< Ignored by this fork (upstream: chain of Vulkan feature structs). */
+    void *vulkan_10_physical_device_features; /**< Ignored by this fork (upstream: a VkPhysicalDeviceFeatures struct). */
+    Uint32 device_extension_count; /**< Number of additional device extensions to require. */
+    const char **device_extension_names; /**< Additional device extensions to require. */
+    Uint32 instance_extension_count; /**< Number of additional instance extensions to require. */
+    const char **instance_extension_names; /**< Additional instance extensions to require. */
+} SDL_GPUVulkanOptions;
 
 /**
  * Destroys a GPU context previously returned by SDL_CreateGPUDevice.
PATCH
git diff --stat
```

Expected: `2 files changed, 182 insertions(+), 4 deletions(-)` (SDL_gpu.h and SDL_gpu_vulkan.c).

- [ ] **Step 5: Build (this recompiles SDL, about 5 minutes)**

```bash
cd /config/projects/fatal/ROLLER
zig build -Dvr -p /tmp/roller-vr-out 2>&1 | tail -20; echo "exit=${PIPESTATUS[0]}"
ls -la /tmp/roller-vr-out/bin
```

Expected: exit 0; `bin/vr_vulkan_options_test`, `bin/roller-vr` and `bin/roller` exist.

- [ ] **Step 6: Run the test on real hardware**

This LXC has no GPU, so run it on `fatal-gpu` under a real Xorg (started once; Xvfb has no DRI3):

```bash
scp /tmp/roller-vr-out/bin/vr_vulkan_options_test fatal-gpu:/root/vr-test/ 2>/dev/null || (ssh fatal-gpu 'mkdir -p /root/vr-test' && scp /tmp/roller-vr-out/bin/vr_vulkan_options_test fatal-gpu:/root/vr-test/)
ssh fatal-gpu 'bash -s' <<'REMOTE'
cd /root/vr-test
if [ ! -S /tmp/.X11-unix/X1 ]; then
  setsid nohup Xorg :1 -config /root/xorg.conf -noreset -novtswitch -sharevts >/root/xorg-vr.log 2>&1 &
  for i in $(seq 1 30); do [ -S /tmp/.X11-unix/X1 ] && break; sleep 1; done
fi
export DISPLAY=:1 SDL_VIDEODRIVER=x11
timeout 60 ./vr_vulkan_options_test; echo "test exit=$?"
REMOTE
```

Expected:

```text
baseline device driver: vulkan
vr_vulkan_options_test: ALL PASS
test exit=0
```

(Run on the dev LXC instead, it prints `SKIP: no usable Vulkan GPU device` and exits 77.)

- [ ] **Step 7: Commit the fork, then ROLLER**

```bash
cd /root/sdl-vr-fork
git add -A
git commit -m "gpu/vulkan: add SDL_GPUVulkanOptions (API version and extra instance/device extensions)

Same struct and property name as SDL 3.4 so a later move to 3.4 needs no
caller changes. An OpenXR runtime dictates the Vulkan API version and
extensions the VkInstance/VkDevice must enable (XR_KHR_vulkan_enable), but
SDL_GPU creates them. Requested extensions are checked first so the error
names the missing one, and VULKAN_CreateDevice keeps that cause instead of
replacing it with a generic message."
git log --oneline -1

cd /config/projects/fatal/ROLLER
git add tests/vr_vulkan_options_test.c build.zig
git commit -m "vr: add a real-GPU test for the SDL fork's Vulkan options hook

Built into <prefix>/bin by -Dvr and run on fatal-gpu (skips with status 77
where there is no Vulkan device)."
```

Write the fork commit hash down; Task 10 records it.

---

### Task 7: Vulkan interop negotiation in `vr_runtime`

**Files:**
- Modify: `PROJECTS/ROLLER/vr_runtime.c`, `PROJECTS/ROLLER/vr_runtime.h`, `PROJECTS/ROLLER/roller.c`, `build.zig`

**Interfaces:**
- Consumes: `VrExtList`, `vr_choose_vk_binding` (Task 3); `vr_log_line` (Task 2); `vr_diag_fatal` (Task 5); `SDL_GPUVulkanOptions` and the property (Task 6).
- Produces:
  - `bool vr_prepare_vulkan(SDL_GPUVulkanOptions *out_options)` — called by `InitSDL()`'s VR branch **before** the GPU device exists: enumerates the runtime's extensions (all logged), chooses `XR_KHR_vulkan_enable` (falls back to `enable2` with a warning, fails if neither), creates the `XrInstance`, logs the runtime name, finds the system, queries the Vulkan requirements, requests API 1.1 clamped into the runtime's range, and (for v1) fetches the runtime's instance and device extension lists into `out_options`.
  - `bool vr_init(void)` now requires `vr_prepare_vulkan()` to have run; it adopts SDL_GPU's device, asks the runtime for its physical device with the function matching the chosen binding, and creates the session (the binding struct is the same for both variants). It also logs the swapchain formats the runtime offers (as `VkFormat` values) and warns if `VK_FORMAT_R8G8B8A8_UNORM` (37), which the eye swapchains use, is missing.
  - `InitSDL()` creates the GPU device with `SDL_CreateGPUDeviceWithProperties()` and the options pointer.

- [ ] **Step 1: Record the baseline (the "failing test")**

With the binary from Task 6 (it still has the Phase 2 runtime behaviour):

```bash
/config/projects/fatal/tools/vr-monado-run.sh /tmp/roller-vr-out/bin/roller-vr --frames 300 | sed -n '/validation counts/,$p'
```

Expected: `VUID-VkImageMemoryBarrier-None-09120` and `VUID-VkImageMemoryBarrier-image-09118` are non-zero (12 each in Phase 2's 300-frame run), and there is no `using XR_KHR_vulkan_enable` line.

- [ ] **Step 2: Apply the change**

The patch moves instance creation and system lookup out of `vr_init()` into the new `vr_prepare_vulkan()`, replaces the enable2-only code with the binding chosen by `vr_choose_vk_binding()`, and rewrites the GPU-device creation in `InitSDL()`.

```bash
git apply --whitespace=nowarn - <<'PATCH'
diff --git a/PROJECTS/ROLLER/roller.c b/PROJECTS/ROLLER/roller.c
index f8ebe65..5c77522 100644
--- a/PROJECTS/ROLLER/roller.c
+++ b/PROJECTS/ROLLER/roller.c
@@ -1,6 +1,7 @@
 #include "roller.h"
 #if defined(ROLLER_VR_BUILD)
 #include "vr_diag.h"
+#include "vr_runtime.h"
 #endif
 #include "scene_render_gpu.h"
 #include "crt_filter.h"
@@ -834,11 +835,29 @@ int InitSDL(char *whiplash_root, const char *midi_root)
      * walking-skeleton's own SDL_CreateGPUDevice call exactly. s_pGPUDevice
      * only exists on non-WASM targets (see its declaration above), and VR
      * mode is meaningless in a browser build anyway. */
+#if defined(ROLLER_VR_BUILD)
+    /* The OpenXR runtime dictates the Vulkan API version and extensions the
+     * VkInstance/VkDevice must enable, so ask it first and hand the answer to
+     * SDL_GPU (patched SDL fork, SDL_GPUVulkanOptions). */
+    SDL_GPUVulkanOptions vkOptions;
+    if (!vr_prepare_vulkan(&vkOptions))
+      vr_diag_fatal("Couldn't prepare the OpenXR runtime; see roller-vr.log next to the executable.");
+    SDL_PropertiesID gpuProps = SDL_CreateProperties();
+    SDL_SetBooleanProperty(gpuProps, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
+    SDL_SetBooleanProperty(gpuProps, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, false);
+    SDL_SetBooleanProperty(gpuProps, SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN, false);
+    SDL_SetPointerProperty(gpuProps, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, &vkOptions);
+    s_pGPUDevice = SDL_CreateGPUDeviceWithProperties(gpuProps);
+    SDL_DestroyProperties(gpuProps);
+    if (!s_pGPUDevice)
+      vr_diag_fatal("Couldn't create the GPU device: %s", SDL_GetError());
+#else
     s_pGPUDevice = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
     if (!s_pGPUDevice) {
       ErrorBoxExit("Couldn't create GPU device: %s", SDL_GetError());
       return 1;
     }
+#endif
     SDL_Log("GPU driver (VR mode): %s", SDL_GetGPUDeviceDriver(s_pGPUDevice));
     return 0;
   }
diff --git a/PROJECTS/ROLLER/vr_runtime.c b/PROJECTS/ROLLER/vr_runtime.c
index 77691f1..1db7e6d 100644
--- a/PROJECTS/ROLLER/vr_runtime.c
+++ b/PROJECTS/ROLLER/vr_runtime.c
@@ -9,6 +9,8 @@
  */
 #include "vr_runtime.h"
 #include "vr_view_merge.h"
+#include "vr_ext_list.h"
+#include "vr_log.h"
 
 #include <SDL3/SDL.h>
 
@@ -46,6 +48,11 @@ static XrSpace s_local_space;
 static VkCommandPool s_command_pool;
 static VkCommandBuffer s_command_buffer;
 static bool s_session_running;
+static VrVkBinding s_vk_binding = VR_VK_BINDING_NONE;
+static VrExtList s_vk_instance_exts;
+static VrExtList s_vk_device_exts;
+static const char *s_vk_instance_ext_ptrs[VR_EXT_LIST_MAX];
+static const char *s_vk_device_ext_ptrs[VR_EXT_LIST_MAX];
 /* task-7-round2 New Important 1: true from the moment vr_run_frame()'s own
  * xrBeginFrame() succeeds until its own xrEndFrame() call (success or
  * failure) or vr_shutdown()'s frame-close-out below runs. Lets vr_shutdown()
@@ -81,8 +88,188 @@ static bool fail_vk(const char *what, VkResult result)
     return false;
 }
 
+/* --- Vulkan interop negotiation (Phase 3 M1) ---------------------------- */
+
+/* Converts an XrVersion to a Vulkan API version, clamping the fields so an
+ * "unbounded" runtime maximum (0xffff minor, 0xffffffff patch) cannot overflow
+ * VK_MAKE_API_VERSION's bit fields. */
+static Uint32 vr_xr_to_vk_version(XrVersion v)
+{
+    Uint32 major = XR_VERSION_MAJOR(v);
+    Uint32 minor = XR_VERSION_MINOR(v);
+    Uint32 patch = XR_VERSION_PATCH(v);
+    if (major > 127) major = 127;
+    if (minor > 1023) minor = 1023;
+    if (patch > 4095) patch = 4095;
+    return VK_MAKE_API_VERSION(0, major, minor, patch);
+}
+
+/* Vulkan 1.1 makes VK_QUEUE_FAMILY_EXTERNAL usable in barriers without enabling
+ * VK_KHR_external_memory (VUID-VkImageMemoryBarrier-None-09120), which the
+ * swapchain image release in vr_present_stereo_frame() relies on. Ask for it
+ * unless the runtime's supported range says otherwise. */
+static Uint32 vr_pick_vulkan_api_version(const XrGraphicsRequirementsVulkanKHR *req)
+{
+    Uint32 wanted = VK_MAKE_API_VERSION(0, 1, 1, 0);
+    Uint32 min_v = vr_xr_to_vk_version(req->minApiVersionSupported);
+    Uint32 max_v = vr_xr_to_vk_version(req->maxApiVersionSupported);
+    if (wanted < min_v) wanted = min_v;
+    if (wanted > max_v) wanted = max_v;
+    return wanted;
+}
+
+typedef XrResult (XRAPI_PTR *VrGetExtensionStringFn)(XrInstance, XrSystemId, uint32_t, uint32_t *, char *);
+
+/* Two-call idiom for xrGetVulkanInstanceExtensionsKHR / xrGetVulkanDeviceExtensionsKHR:
+ * fetch the runtime's space-separated extension string and add the names to `out`. */
+static bool vr_query_extension_string(VrGetExtensionStringFn fn, const char *what, VrExtList *out)
+{
+    uint32_t needed = 0;
+    XrResult xr_result = fn(s_instance, s_system_id, 0, &needed, NULL);
+    if (XR_FAILED(xr_result))
+        return fail_xr(what, xr_result);
+    char *buffer = calloc(needed ? needed : 1, 1);
+    if (!buffer)
+        return fail("out of memory reading the runtime's Vulkan extension list");
+    xr_result = fn(s_instance, s_system_id, needed, &needed, buffer);
+    if (XR_FAILED(xr_result)) {
+        free(buffer);
+        return fail_xr(what, xr_result);
+    }
+    vr_log_line("INFO", "%s: %s", what, buffer[0] ? buffer : "(none)");
+    bool stored = vr_ext_list_add_spaced(out, buffer);
+    free(buffer);
+    if (!stored)
+        return fail("could not store every Vulkan extension name the runtime listed");
+    return true;
+}
+
+bool vr_prepare_vulkan(SDL_GPUVulkanOptions *out_options)
+{
+    if (!out_options)
+        return fail("vr_prepare_vulkan: out_options is NULL");
+    SDL_zerop(out_options);
+    vr_ext_list_init(&s_vk_instance_exts);
+    vr_ext_list_init(&s_vk_device_exts);
+
+    /* --- 1. Which Vulkan interop extension does the runtime offer? --- */
+    uint32_t ext_count = 0;
+    XrResult xr_result = xrEnumerateInstanceExtensionProperties(NULL, 0, &ext_count, NULL);
+    if (XR_FAILED(xr_result))
+        return fail_xr("xrEnumerateInstanceExtensionProperties (count) failed -- is an OpenXR runtime installed and active?", xr_result);
+    XrExtensionProperties *ext_props = calloc(ext_count ? ext_count : 1, sizeof(XrExtensionProperties));
+    if (!ext_props)
+        return fail("out of memory enumerating OpenXR extensions");
+    for (uint32_t i = 0; i < ext_count; i++)
+        ext_props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
+    xr_result = xrEnumerateInstanceExtensionProperties(NULL, ext_count, &ext_count, ext_props);
+    if (XR_FAILED(xr_result)) {
+        free(ext_props);
+        return fail_xr("xrEnumerateInstanceExtensionProperties failed", xr_result);
+    }
+    bool has_enable_v1 = false, has_enable2 = false;
+    vr_log_line("INFO", "OpenXR runtime advertises %u instance extensions:", (unsigned)ext_count);
+    for (uint32_t i = 0; i < ext_count; i++) {
+        vr_log_line("INFO", "  %s (v%u)", ext_props[i].extensionName, (unsigned)ext_props[i].extensionVersion);
+        if (strcmp(ext_props[i].extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0)
+            has_enable_v1 = true;
+        if (strcmp(ext_props[i].extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0)
+            has_enable2 = true;
+    }
+    free(ext_props);
+
+    s_vk_binding = vr_choose_vk_binding(has_enable_v1, has_enable2);
+    if (s_vk_binding == VR_VK_BINDING_NONE)
+        return fail("the OpenXR runtime offers neither XR_KHR_vulkan_enable nor XR_KHR_vulkan_enable2");
+    if (s_vk_binding == VR_VK_BINDING_ENABLE2_FALLBACK)
+        vr_log_line("WARN", "runtime lacks XR_KHR_vulkan_enable; falling back to XR_KHR_vulkan_enable2, whose "
+                            "valid usage expects the runtime to create the Vulkan instance/device (SDL_GPU creates them here)");
+    else
+        vr_log_line("INFO", "using XR_KHR_vulkan_enable (the application creates the Vulkan instance/device)");
+
+    /* --- 2. Create the OpenXR instance and find the headset --- */
+    const char *enabled_extensions[] = {
+        s_vk_binding == VR_VK_BINDING_ENABLE_V1 ? XR_KHR_VULKAN_ENABLE_EXTENSION_NAME
+                                                : XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME
+    };
+    XrInstanceCreateInfo instance_create_info = { .type = XR_TYPE_INSTANCE_CREATE_INFO };
+    strncpy(instance_create_info.applicationInfo.applicationName, "ROLLER VR",
+            XR_MAX_APPLICATION_NAME_SIZE - 1);
+    instance_create_info.applicationInfo.applicationVersion = 1;
+    instance_create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
+    instance_create_info.enabledExtensionCount = 1;
+    instance_create_info.enabledExtensionNames = enabled_extensions;
+
+    s_instance = XR_NULL_HANDLE;
+    xr_result = xrCreateInstance(&instance_create_info, &s_instance);
+    if (XR_FAILED(xr_result))
+        return fail_xr("xrCreateInstance failed", xr_result);
+
+    XrInstanceProperties instance_props = { .type = XR_TYPE_INSTANCE_PROPERTIES };
+    if (XR_SUCCEEDED(xrGetInstanceProperties(s_instance, &instance_props)))
+        vr_log_line("INFO", "OpenXR runtime: %s (%u.%u.%u)", instance_props.runtimeName,
+                    (unsigned)XR_VERSION_MAJOR(instance_props.runtimeVersion),
+                    (unsigned)XR_VERSION_MINOR(instance_props.runtimeVersion),
+                    (unsigned)XR_VERSION_PATCH(instance_props.runtimeVersion));
+
+    XrSystemGetInfo system_get_info = {
+        .type = XR_TYPE_SYSTEM_GET_INFO,
+        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
+    };
+    xr_result = xrGetSystem(s_instance, &system_get_info, &s_system_id);
+    if (XR_FAILED(xr_result))
+        return fail_xr("xrGetSystem failed (no HMD available from the active OpenXR runtime?)", xr_result);
+
+    /* --- 3. What Vulkan API version and extensions does the runtime need? --- */
+    const char *requirements_fn_name = s_vk_binding == VR_VK_BINDING_ENABLE_V1
+        ? "xrGetVulkanGraphicsRequirementsKHR" : "xrGetVulkanGraphicsRequirements2KHR";
+    PFN_xrGetVulkanGraphicsRequirementsKHR pfn_get_requirements = NULL;
+    xr_result = xrGetInstanceProcAddr(s_instance, requirements_fn_name, (PFN_xrVoidFunction *)&pfn_get_requirements);
+    if (XR_FAILED(xr_result) || !pfn_get_requirements)
+        return fail_xr("failed to load the runtime's Vulkan graphics requirements function", xr_result);
+    XrGraphicsRequirementsVulkanKHR requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
+    xr_result = pfn_get_requirements(s_instance, s_system_id, &requirements);
+    if (XR_FAILED(xr_result))
+        return fail_xr("the runtime's Vulkan graphics requirements query failed", xr_result);
+    Uint32 api_version = vr_pick_vulkan_api_version(&requirements);
+    vr_log_line("INFO", "runtime supports Vulkan API %u.%u..%u.%u; requesting %u.%u.%u",
+                (unsigned)VK_API_VERSION_MAJOR(vr_xr_to_vk_version(requirements.minApiVersionSupported)),
+                (unsigned)VK_API_VERSION_MINOR(vr_xr_to_vk_version(requirements.minApiVersionSupported)),
+                (unsigned)VK_API_VERSION_MAJOR(vr_xr_to_vk_version(requirements.maxApiVersionSupported)),
+                (unsigned)VK_API_VERSION_MINOR(vr_xr_to_vk_version(requirements.maxApiVersionSupported)),
+                (unsigned)VK_API_VERSION_MAJOR(api_version), (unsigned)VK_API_VERSION_MINOR(api_version),
+                (unsigned)VK_API_VERSION_PATCH(api_version));
+
+    if (s_vk_binding == VR_VK_BINDING_ENABLE_V1) {
+        PFN_xrGetVulkanInstanceExtensionsKHR pfn_instance_exts = NULL;
+        PFN_xrGetVulkanDeviceExtensionsKHR pfn_device_exts = NULL;
+        xr_result = xrGetInstanceProcAddr(s_instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction *)&pfn_instance_exts);
+        if (XR_FAILED(xr_result) || !pfn_instance_exts)
+            return fail_xr("failed to load xrGetVulkanInstanceExtensionsKHR", xr_result);
+        xr_result = xrGetInstanceProcAddr(s_instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction *)&pfn_device_exts);
+        if (XR_FAILED(xr_result) || !pfn_device_exts)
+            return fail_xr("failed to load xrGetVulkanDeviceExtensionsKHR", xr_result);
+        if (!vr_query_extension_string(pfn_instance_exts, "runtime-required Vulkan instance extensions", &s_vk_instance_exts))
+            return false;
+        if (!vr_query_extension_string(pfn_device_exts, "runtime-required Vulkan device extensions", &s_vk_device_exts))
+            return false;
+    }
+
+    out_options->vulkan_api_version = api_version;
+    out_options->instance_extension_count = vr_ext_list_ptrs(&s_vk_instance_exts, s_vk_instance_ext_ptrs, VR_EXT_LIST_MAX);
+    out_options->instance_extension_names = s_vk_instance_ext_ptrs;
+    out_options->device_extension_count = vr_ext_list_ptrs(&s_vk_device_exts, s_vk_device_ext_ptrs, VR_EXT_LIST_MAX);
+    out_options->device_extension_names = s_vk_device_ext_ptrs;
+    return true;
+}
+
 bool vr_init(void)
 {
+    if (s_instance == XR_NULL_HANDLE) {
+        fail("vr_init() called before vr_prepare_vulkan() created the OpenXR instance");
+        return false;
+    }
+
     /* --- 1. Adopt the game's existing SDL_GPU device (Vulkan) --- */
     /* task-7-review Finding 1 (Critical): this used to call its own
      * SDL_CreateGPUDevice() here, creating a SECOND, independent VkDevice
@@ -132,87 +319,55 @@ bool vr_init(void)
     printf("vr_runtime: got VkInstance=%p VkDevice=%p queueFamilyIndex=%u\n",
            (void *)s_vk_instance, (void *)s_vk_device, s_vk_queue_family_index);
 
-    /* --- 2. Create the OpenXR instance --- */
-    const char *enabled_extensions[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
-
-    XrInstanceCreateInfo instance_create_info = { .type = XR_TYPE_INSTANCE_CREATE_INFO };
-    strncpy(instance_create_info.applicationInfo.applicationName, "ROLLER VR",
-            XR_MAX_APPLICATION_NAME_SIZE - 1);
-    instance_create_info.applicationInfo.applicationVersion = 1;
-    instance_create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
-    instance_create_info.enabledExtensionCount = 1;
-    instance_create_info.enabledExtensionNames = enabled_extensions;
-
-    s_instance = XR_NULL_HANDLE;
-    XrResult xr_result = xrCreateInstance(&instance_create_info, &s_instance);
-    if (XR_FAILED(xr_result)) {
-        fail_xr("xrCreateInstance failed", xr_result);
-        return false;
-    }
-
-    /* --- 3. Get the system (the "head" device — the Simulated HMD when no real headset is attached) --- */
-    XrSystemGetInfo system_get_info = {
-        .type = XR_TYPE_SYSTEM_GET_INFO,
-        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
-    };
-    xr_result = xrGetSystem(s_instance, &system_get_info, &s_system_id);
-    if (XR_FAILED(xr_result)) {
-        fail_xr("xrGetSystem failed (no HMD available from the active OpenXR runtime?)", xr_result);
-        return false;
-    }
-
-    /* Required by the OpenXR spec before creating a Vulkan-backed session,
-     * even though we already have our own device and don't act on the
-     * returned version range here. */
-    PFN_xrGetVulkanGraphicsRequirements2KHR pfn_get_vk_requirements = NULL;
-    xr_result = xrGetInstanceProcAddr(s_instance, "xrGetVulkanGraphicsRequirements2KHR",
-                                       (PFN_xrVoidFunction *)&pfn_get_vk_requirements);
-    if (XR_FAILED(xr_result) || !pfn_get_vk_requirements) {
-        fail_xr("failed to load xrGetVulkanGraphicsRequirements2KHR", xr_result);
-        return false;
-    }
-    XrGraphicsRequirementsVulkan2KHR vk_requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
-    xr_result = pfn_get_vk_requirements(s_instance, s_system_id, &vk_requirements);
-    if (XR_FAILED(xr_result)) {
-        fail_xr("xrGetVulkanGraphicsRequirements2KHR failed", xr_result);
-        return false;
-    }
-
-    /* Also required by the spec (XR_KHR_vulkan_enable2) when the app supplies
-     * its own already-created Vulkan device, as we do here via SDL_GPU: the
-     * runtime must be told which physical device we're using before
-     * xrCreateSession, even though we already know it ourselves. Skipping
-     * this call makes xrCreateSession fail with XR_ERROR_VALIDATION_FAILURE
-     * ("Has not called xrGetVulkanGraphicsDevice2KHR") on at least Monado. */
-    PFN_xrGetVulkanGraphicsDevice2KHR pfn_get_vk_device = NULL;
-    xr_result = xrGetInstanceProcAddr(s_instance, "xrGetVulkanGraphicsDevice2KHR",
-                                       (PFN_xrVoidFunction *)&pfn_get_vk_device);
-    if (XR_FAILED(xr_result) || !pfn_get_vk_device) {
-        fail_xr("failed to load xrGetVulkanGraphicsDevice2KHR", xr_result);
-        return false;
-    }
-    XrVulkanGraphicsDeviceGetInfoKHR vk_device_get_info = {
-        .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
-        .systemId = s_system_id,
-        .vulkanInstance = s_vk_instance,
-    };
+    /* --- 2. Tell the runtime which physical device we use --- */
+    /* Required by the spec before creating a Vulkan-backed session even though
+     * SDL_GPU already chose the device: skipping it makes xrCreateSession fail
+     * with XR_ERROR_VALIDATION_FAILURE on at least Monado. The v1 and v2 entry
+     * points differ; the graphics binding struct is the same for both. */
+    XrResult xr_result;
     VkPhysicalDevice xr_required_physical_device = VK_NULL_HANDLE;
-    xr_result = pfn_get_vk_device(s_instance, &vk_device_get_info, &xr_required_physical_device);
-    if (XR_FAILED(xr_result)) {
-        fail_xr("xrGetVulkanGraphicsDevice2KHR failed", xr_result);
-        return false;
+    if (s_vk_binding == VR_VK_BINDING_ENABLE_V1) {
+        PFN_xrGetVulkanGraphicsDeviceKHR pfn_get_vk_device = NULL;
+        xr_result = xrGetInstanceProcAddr(s_instance, "xrGetVulkanGraphicsDeviceKHR",
+                                           (PFN_xrVoidFunction *)&pfn_get_vk_device);
+        if (XR_FAILED(xr_result) || !pfn_get_vk_device) {
+            fail_xr("failed to load xrGetVulkanGraphicsDeviceKHR", xr_result);
+            return false;
+        }
+        xr_result = pfn_get_vk_device(s_instance, s_system_id, s_vk_instance, &xr_required_physical_device);
+        if (XR_FAILED(xr_result)) {
+            fail_xr("xrGetVulkanGraphicsDeviceKHR failed", xr_result);
+            return false;
+        }
+    } else {
+        PFN_xrGetVulkanGraphicsDevice2KHR pfn_get_vk_device = NULL;
+        xr_result = xrGetInstanceProcAddr(s_instance, "xrGetVulkanGraphicsDevice2KHR",
+                                           (PFN_xrVoidFunction *)&pfn_get_vk_device);
+        if (XR_FAILED(xr_result) || !pfn_get_vk_device) {
+            fail_xr("failed to load xrGetVulkanGraphicsDevice2KHR", xr_result);
+            return false;
+        }
+        XrVulkanGraphicsDeviceGetInfoKHR vk_device_get_info = {
+            .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
+            .systemId = s_system_id,
+            .vulkanInstance = s_vk_instance,
+        };
+        xr_result = pfn_get_vk_device(s_instance, &vk_device_get_info, &xr_required_physical_device);
+        if (XR_FAILED(xr_result)) {
+            fail_xr("xrGetVulkanGraphicsDevice2KHR failed", xr_result);
+            return false;
+        }
     }
     if (xr_required_physical_device != s_vk_physical_device) {
         fail("SDL_GPU's Vulkan physical device does not match the physical device the OpenXR "
-             "runtime requires (xrGetVulkanGraphicsDevice2KHR) -- this can happen on multi-GPU "
-             "systems; creating the session against a mismatched device/physicalDevice pair is "
-             "undefined behavior per the Vulkan spec");
+             "runtime requires -- this can happen on multi-GPU systems; creating the session "
+             "against a mismatched device/physicalDevice pair is undefined behavior per the Vulkan spec");
         return false;
     }
 
-    /* --- 4. Create the session against our existing Vulkan device --- */
-    XrGraphicsBindingVulkan2KHR vk_binding = {
-        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
+    /* --- 3. Create the session against our existing Vulkan device --- */
+    XrGraphicsBindingVulkanKHR vk_binding = {
+        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, /* XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR is the same value */
         .instance = s_vk_instance,
         .physicalDevice = xr_required_physical_device,
         .device = s_vk_device,
@@ -232,6 +387,31 @@ bool vr_init(void)
     }
     printf("vr_runtime: OpenXR session created\n");
 
+    /* Log what the runtime offers: the eye swapchains below are created as
+     * VK_FORMAT_R8G8B8A8_UNORM, which a runtime may not list (SteamVR prefers sRGB). */
+    uint32_t format_count = 0;
+    if (XR_SUCCEEDED(xrEnumerateSwapchainFormats(s_session, 0, &format_count, NULL)) && format_count > 0) {
+        int64_t *formats = calloc(format_count, sizeof(int64_t));
+        if (formats && XR_SUCCEEDED(xrEnumerateSwapchainFormats(s_session, format_count, &format_count, formats))) {
+            char list[512] = "";
+            bool has_unorm = false;
+            for (uint32_t i = 0; i < format_count; i++) {
+                char one[24];
+                snprintf(one, sizeof(one), "%s%lld", i ? " " : "", (long long)formats[i]);
+                if (strlen(list) + strlen(one) < sizeof(list))
+                    strcat(list, one);
+                if (formats[i] == (int64_t)VK_FORMAT_R8G8B8A8_UNORM)
+                    has_unorm = true;
+            }
+            vr_log_line("INFO", "runtime swapchain formats (VkFormat values): %s", list);
+            if (!has_unorm)
+                vr_log_line("WARN", "the runtime does not list VK_FORMAT_R8G8B8A8_UNORM (%d), which the eye "
+                                    "swapchains use; xrCreateSwapchain is likely to fail",
+                            (int)VK_FORMAT_R8G8B8A8_UNORM);
+        }
+        free(formats);
+    }
+
     /* --- 5. Enumerate the view configuration (expect 2 views for stereo) --- */
     s_view_count = 0;
     xr_result = xrEnumerateViewConfigurationViews(s_instance, s_system_id,
@@ -330,6 +510,7 @@ bool vr_init(void)
     return true;
 }
 
+
 /* Number of ticks that actually rendered and submitted a stereo layer, for
  * the --vr-frames N bounded-run option (g_iVRMaxFrames) and progress logging. */
 static int s_vr_frames_presented = 0;
diff --git a/PROJECTS/ROLLER/vr_runtime.h b/PROJECTS/ROLLER/vr_runtime.h
index a15d3ff..a57dfa8 100644
--- a/PROJECTS/ROLLER/vr_runtime.h
+++ b/PROJECTS/ROLLER/vr_runtime.h
@@ -2,9 +2,22 @@
 #define VR_RUNTIME_H
 
 #include <stdbool.h>
+#include <SDL3/SDL_gpu.h>
 #include <openxr/openxr.h>
 #include "scene_render_types.h"
 
+/* Step 1 of VR startup, called by InitSDL()'s VR branch BEFORE the GPU device is
+ * created: creates the OpenXR instance, finds the headset, and asks the runtime
+ * which Vulkan API version and instance/device extensions its Vulkan interop
+ * needs. Fills *out_options (arrays owned by this module, valid until
+ * vr_shutdown()) so SDL_GPU can create a compatible VkInstance/VkDevice via
+ * SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER. Uses XR_KHR_vulkan_enable
+ * (application-created Vulkan objects) and falls back to XR_KHR_vulkan_enable2
+ * with a logged warning. Returns false on failure (details in the log). */
+bool vr_prepare_vulkan(SDL_GPUVulkanOptions *out_options);
+
+/* Step 2, called from main() after InitSDL(): adopts SDL_GPU's device, creates
+ * the session, swapchains and reference space. Requires vr_prepare_vulkan(). */
 bool vr_init(void);
 void vr_shutdown(void);
 
diff --git a/build.zig b/build.zig
index cad1aa6..2b90bc2 100644
--- a/build.zig
+++ b/build.zig
@@ -289,6 +289,7 @@ pub fn build(b: *std.Build) void {
                 "PROJECTS/ROLLER/vr_runtime.c",
                 "PROJECTS/ROLLER/vr_diag.c",
                 "PROJECTS/ROLLER/vr_log.c",
+                "PROJECTS/ROLLER/vr_ext_list.c",
             },
         });
 
PATCH
```

- [ ] **Step 3: Build**

```bash
zig build -Dvr -p /tmp/roller-vr-out 2>&1 | tail -20; echo "exit=${PIPESTATUS[0]}"
python3 tools/check_roller_core_manifest.py
```

Expected: exit 0 (incremental, under a minute); the manifest check passes.

- [ ] **Step 4: Run on Monado and verify the negotiation and the validation counts**

```bash
/config/projects/fatal/tools/vr-monado-run.sh /tmp/roller-vr-out/bin/roller-vr --frames 300
```

Expected (timestamps, the exact format list and the number of `session state` lines vary; the frame count of 300 takes about a minute under Debug):

```text
=== exit status: 0   (log: /tmp/vr-monado-run.<pid>.log)
=== runtime / Vulkan negotiation
[0.003s] INFO using XR_KHR_vulkan_enable (the application creates the Vulkan instance/device)
[0.048s] INFO OpenXR runtime: Monado(XRT) by Collabora et al '25.0.0+dfsg-2' (25.0.0)
[0.048s] INFO runtime supports Vulkan API 1.0..127.1023; requesting 1.1.0
[0.049s] INFO runtime-required Vulkan instance extensions: VK_KHR_external_fence_capabilities VK_KHR_external_memory_capabilities VK_KHR_external_semaphore_capabilities VK_KHR_get_physical_device_properties2
[0.049s] INFO runtime-required Vulkan device extensions: VK_KHR_dedicated_allocation VK_KHR_external_fence VK_KHR_external_memory VK_KHR_external_semaphore VK_KHR_get_memory_requirements2 VK_KHR_image_format_list VK_KHR_external_memory_fd VK_KHR_external_semaphore_fd VK_KHR_external_fence_fd
[0.180s] INFO runtime swapchain formats (VkFormat values): 91 97 90 43 50 37 44 4 100 126 124 125 129 130 127
vr_runtime: view count = 2, view[0] recommended 896x1007
=== validation counts (raw grep -c of log lines)
VUID-VkImageMemoryBarrier-None-09120          0
VUID-VkImageMemoryBarrier-image-09118         0
SYNC-HAZARD                                   0
Validation Error                              0
XR_ERROR                                      0
```

The two `VUID-VkImageMemoryBarrier` counts must be **0** (Finding 2). If they are not, do not continue: read the validation message in the log; it states which extension or API version it still needs.

- [ ] **Step 5: Verify the no-runtime failure path**

Point the OpenXR loader at a runtime that does not exist. The game must exit with status 1 and say why, without a hang or a modal box:

```bash
scp /tmp/roller-vr-out/bin/roller-vr fatal-gpu:/root/vr-test/roller-vr
ssh fatal-gpu 'bash -s' <<'REMOTE'
cd /root/vr-test
export DISPLAY=:1 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy
rm -f roller-vr.log
XR_RUNTIME_JSON=/nonexistent/runtime.json timeout 60 ./roller-vr --vr --vr-frames 10 --whiplash-root /root/roller_v2 --track-path /root/roller_v2/fatdata/TRACK1.TRK </dev/null >/dev/null 2>&1
echo "exit status: $?"
grep -v "sdl:" roller-vr.log | tail -2
REMOTE
```

Expected:

```text
exit status: 1
vr_runtime: xrEnumerateInstanceExtensionProperties (count) failed -- is an OpenXR runtime installed and active? (XrResult=-51)
[0.000s] FATAL Couldn't prepare the OpenXR runtime; see roller-vr.log next to the executable.
```

- [ ] **Step 6: Commit**

```bash
git add PROJECTS/ROLLER/vr_runtime.c PROJECTS/ROLLER/vr_runtime.h PROJECTS/ROLLER/roller.c build.zig
git commit -m "vr: negotiate Vulkan interop per XR_KHR_vulkan_enable and pass it to SDL_GPU

Under XR_KHR_vulkan_enable2 the runtime must create the VkInstance and
VkDevice, but SDL_GPU creates them. Use XR_KHR_vulkan_enable, the
application-created model: create the XrInstance first, ask the runtime
for its Vulkan API range and instance/device extensions, request API 1.1
(which also clears VUID-VkImageMemoryBarrier-None-09120 / image-09118 on
the swapchain release barrier) and hand it all to SDL through
SDL_GPUVulkanOptions. enable2 remains as a logged fallback. The runtime's
whole extension list and name are logged for diagnosing other runtimes."
```

---

### Task 8: Session lifecycle in `vr_runtime`

**Files:**
- Modify: `PROJECTS/ROLLER/vr_runtime.c`, `PROJECTS/ROLLER/vr_runtime.h`, `PROJECTS/ROLLER/3d.c`, `build.zig`

**Interfaces:**
- Consumes: `VrSessionFsm` and its functions (Task 4); `vr_log_line` (Task 2).
- Produces:
  - `int vr_exit_status(void)` — 0 for an orderly end, 1 if an OpenXR call failed; `main()` (and `doexit()`) return it in VR mode.
  - `vr_run_frame()` semantics: a quit that originates in OpenXR (STOPPING, EXITING, LOSS_PENDING, instance loss, a failing `xrPollEvent`/`xrWaitFrame`/`xrBeginFrame`/`xrEndFrame`/`xrBeginSession`) is forwarded to the game (`quit_game = 1; racing = 0`, as `--vr-frames` does); later calls run the game's loop without OpenXR frames until `frontend_shutdown_finish()` has called `vr_shutdown()`; it then returns false.
  - `vr_shutdown()` ends a still-running session the spec-clean way (`xrRequestExitSession`, drain until STOPPING, bounded at 2 s, then `xrEndSession`), skips that when the runtime is already ending it or the request itself fails, then destroys session and instance.
  - Test hook: environment variable `ROLLER_VR_TEST_RUNTIME_EXIT_AFTER=N` calls `xrRequestExitSession` after N presented frames without going through the game's quit path, so the runtime reports STOPPING as it does when the user quits from SteamVR.
  - Log lines (all via `vr_log_line`): `session state -> <n> (actions=0x<mask>)`, `xrEndSession -> XrResult=<n>`, `xrRequestExitSession -> XrResult=<n>`, `asking the game to shut down (OpenXR is ending the session)`, `<call> failed (XrResult=<n>); shutting down`, `session ended after <n> ms`.

- [ ] **Step 1: Apply the change**

The patch adds the state machine to `vr_runtime.c`, `vr_poll_events()`, `vr_forward_quit_to_game()`, `vr_end_session_cleanly()`, the reworked `vr_shutdown()` and `vr_run_frame()`, the test hook, and the exit status.

```bash
git apply --whitespace=nowarn - <<'PATCH'
diff --git a/PROJECTS/ROLLER/3d.c b/PROJECTS/ROLLER/3d.c
index 8684798..77c40e4 100644
--- a/PROJECTS/ROLLER/3d.c
+++ b/PROJECTS/ROLLER/3d.c
@@ -2269,8 +2269,13 @@ void doexit()
     frontend_shutdown_update();
 
 #ifndef __EMSCRIPTEN__
-  if (frontend_shutdown_complete())
+  if (frontend_shutdown_complete()) {
+#if defined(ROLLER_VR_BUILD)
+    if (g_bVRMode)
+      exit(vr_exit_status()); // non-zero when an OpenXR frame call failed
+#endif
     exit(0);
+  }
 #endif
 #endif
 }
@@ -3380,6 +3385,10 @@ int main(int argc, const char **argv, const char **envp)
   //__asm { int     10h; Reset video mode and exit game }// Reset video mode and exit game
   if (!frontend_shutdown_complete())
     doexit();
+#if defined(ROLLER_VR_BUILD)
+  if (g_bVRMode)
+    return vr_exit_status(); // non-zero when an OpenXR frame call failed
+#endif
   return 0;
 }
 #endif
diff --git a/PROJECTS/ROLLER/vr_runtime.c b/PROJECTS/ROLLER/vr_runtime.c
index 1db7e6d..e2c929f 100644
--- a/PROJECTS/ROLLER/vr_runtime.c
+++ b/PROJECTS/ROLLER/vr_runtime.c
@@ -11,6 +11,7 @@
 #include "vr_view_merge.h"
 #include "vr_ext_list.h"
 #include "vr_log.h"
+#include "vr_session_fsm.h"
 
 #include <SDL3/SDL.h>
 
@@ -47,12 +48,27 @@ static XrSwapchain *s_swapchains;
 static XrSpace s_local_space;
 static VkCommandPool s_command_pool;
 static VkCommandBuffer s_command_buffer;
-static bool s_session_running;
+static VrSessionFsm s_fsm; /* session lifecycle decisions, see vr_session_fsm.h */
 static VrVkBinding s_vk_binding = VR_VK_BINDING_NONE;
 static VrExtList s_vk_instance_exts;
 static VrExtList s_vk_device_exts;
 static const char *s_vk_instance_ext_ptrs[VR_EXT_LIST_MAX];
 static const char *s_vk_device_ext_ptrs[VR_EXT_LIST_MAX];
+static int s_exit_status = 0;             /* process exit status, see vr_exit_status() */
+static bool s_quit_forwarded = false;     /* OpenXR asked to quit and the game has been told, see vr_forward_quit_to_game() */
+static int s_test_runtime_exit_after = 0; /* ROLLER_VR_TEST_RUNTIME_EXIT_AFTER=N, see vr_run_frame() */
+
+/* VrSessionState mirrors XrSessionState numerically so the state machine can be
+ * fed the runtime's values directly. */
+_Static_assert((int)XR_SESSION_STATE_IDLE == (int)VR_STATE_IDLE &&
+               (int)XR_SESSION_STATE_READY == (int)VR_STATE_READY &&
+               (int)XR_SESSION_STATE_SYNCHRONIZED == (int)VR_STATE_SYNCHRONIZED &&
+               (int)XR_SESSION_STATE_VISIBLE == (int)VR_STATE_VISIBLE &&
+               (int)XR_SESSION_STATE_FOCUSED == (int)VR_STATE_FOCUSED &&
+               (int)XR_SESSION_STATE_STOPPING == (int)VR_STATE_STOPPING &&
+               (int)XR_SESSION_STATE_LOSS_PENDING == (int)VR_STATE_LOSS_PENDING &&
+               (int)XR_SESSION_STATE_EXITING == (int)VR_STATE_EXITING,
+               "VrSessionState must match XrSessionState");
 /* task-7-round2 New Important 1: true from the moment vr_run_frame()'s own
  * xrBeginFrame() succeeds until its own xrEndFrame() call (success or
  * failure) or vr_shutdown()'s frame-close-out below runs. Lets vr_shutdown()
@@ -149,8 +165,13 @@ bool vr_prepare_vulkan(SDL_GPUVulkanOptions *out_options)
     if (!out_options)
         return fail("vr_prepare_vulkan: out_options is NULL");
     SDL_zerop(out_options);
+    vr_fsm_init(&s_fsm);
     vr_ext_list_init(&s_vk_instance_exts);
     vr_ext_list_init(&s_vk_device_exts);
+    s_exit_status = 0;
+
+    const char *test_exit = getenv("ROLLER_VR_TEST_RUNTIME_EXIT_AFTER");
+    s_test_runtime_exit_after = test_exit ? atoi(test_exit) : 0;
 
     /* --- 1. Which Vulkan interop extension does the runtime offer? --- */
     uint32_t ext_count = 0;
@@ -505,12 +526,118 @@ bool vr_init(void)
         return false;
     }
 
-    s_session_running = false;
-
     return true;
 }
 
 
+/* --- Session lifecycle (Phase 3 M1) -------------------------------------- */
+
+#define VR_EXIT_DRAIN_TIMEOUT_MS 2000
+
+/* The game only shuts down by running its frontend state machine (RACING -> ... ->
+ * SHUTDOWN -> QUIT inside main_loop_iteration()). Merely leaving the frame loop skips
+ * frontend_shutdown_finish(), so vr_shutdown(), ShutdownSDL() and the save path never
+ * run and main() exits 0 -- observed on Monado. So a quit that originates in OpenXR
+ * (the runtime ending the session, or an OpenXR failure) is forwarded to the game the
+ * way --vr-frames does it, and vr_run_frame() then keeps running the game's loop,
+ * without OpenXR frames, until the game has shut itself down. */
+static void vr_forward_quit_to_game(void)
+{
+    if (s_quit_forwarded)
+        return;
+    s_quit_forwarded = true;
+    vr_log_line("INFO", "asking the game to shut down (OpenXR is ending the session)");
+    quit_game = 1;
+    racing = 0;
+}
+
+static void vr_frame_call_failed(const char *what, XrResult result)
+{
+    vr_log_line("ERROR", "%s failed (XrResult=%d); shutting down", what, (int)result);
+    s_exit_status = 1;
+    vr_forward_quit_to_game();
+}
+
+/* Polls every pending OpenXR event, feeds session state changes to the state
+ * machine and carries out the actions it returns. Returns true when the runtime
+ * wants the application to quit. */
+static bool vr_poll_events(void)
+{
+    bool quit = false;
+    XrResult poll_result;
+    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
+    while ((poll_result = xrPollEvent(s_instance, &event)) == XR_SUCCESS) {
+        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
+            const XrEventDataSessionStateChanged *state_event = (const XrEventDataSessionStateChanged *)&event;
+            unsigned int actions = vr_fsm_on_state_changed(&s_fsm, (VrSessionState)state_event->state);
+            vr_log_line("INFO", "session state -> %d (actions=0x%x)", (int)state_event->state, actions);
+            if (actions & VR_ACTION_BEGIN_SESSION) {
+                XrSessionBeginInfo begin_info = {
+                    .type = XR_TYPE_SESSION_BEGIN_INFO,
+                    .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
+                };
+                XrResult begin_result = xrBeginSession(s_session, &begin_info);
+                if (XR_FAILED(begin_result)) {
+                    vr_frame_call_failed("xrBeginSession", begin_result);
+                    quit = true;
+                } else {
+                    vr_fsm_session_begun(&s_fsm);
+                }
+            }
+            if (actions & VR_ACTION_END_SESSION) {
+                XrResult end_result = xrEndSession(s_session);
+                vr_log_line(XR_FAILED(end_result) ? "WARN" : "INFO", "xrEndSession -> XrResult=%d", (int)end_result);
+                vr_fsm_session_ended(&s_fsm);
+            }
+            if (actions & VR_ACTION_REQUEST_QUIT)
+                quit = true;
+        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
+            vr_log_line("WARN", "OpenXR instance loss pending; shutting down");
+            quit = true;
+        }
+        event.type = XR_TYPE_EVENT_DATA_BUFFER;
+    }
+    /* XR_EVENT_UNAVAILABLE (a success code) just means the queue is empty; a failure
+     * such as XR_ERROR_INSTANCE_LOST means the runtime is gone. */
+    if (XR_FAILED(poll_result)) {
+        vr_frame_call_failed("xrPollEvent", poll_result);
+        quit = true;
+    }
+    return quit;
+}
+
+/* Ends a running session the spec-clean way when the GAME is quitting:
+ * xrRequestExitSession, then drain events until the runtime reports STOPPING
+ * (vr_poll_events() calls xrEndSession). Bounded, because the runtime answers
+ * asynchronously; on timeout the still-running session is simply destroyed. When
+ * the runtime is the one ending the session there is nothing to request or wait for. */
+static void vr_end_session_cleanly(void)
+{
+    if (!vr_fsm_frames_allowed(&s_fsm) || vr_fsm_runtime_is_ending(&s_fsm))
+        return;
+
+    if (vr_fsm_should_request_exit(&s_fsm)) {
+        XrResult exit_result = xrRequestExitSession(s_session);
+        vr_log_line(XR_FAILED(exit_result) ? "WARN" : "INFO", "xrRequestExitSession -> XrResult=%d", (int)exit_result);
+        if (XR_FAILED(exit_result))
+            return; /* no STOPPING will follow: the runtime is gone or the session is not running */
+    }
+
+    Uint64 started = SDL_GetTicks();
+    while (vr_fsm_frames_allowed(&s_fsm) && s_exit_status == 0 &&
+           SDL_GetTicks() - started < VR_EXIT_DRAIN_TIMEOUT_MS) {
+        vr_poll_events();
+        if (!vr_fsm_frames_allowed(&s_fsm))
+            break;
+        SDL_Delay(10);
+    }
+    if (vr_fsm_frames_allowed(&s_fsm))
+        vr_log_line("WARN", "the runtime did not report STOPPING within %d ms; destroying the session directly",
+                    VR_EXIT_DRAIN_TIMEOUT_MS);
+    else
+        vr_log_line("INFO", "session ended after %llu ms", (unsigned long long)(SDL_GetTicks() - started));
+}
+
 /* Number of ticks that actually rendered and submitted a stereo layer, for
  * the --vr-frames N bounded-run option (g_iVRMaxFrames) and progress logging. */
 static int s_vr_frames_presented = 0;
@@ -548,73 +675,39 @@ void vr_shutdown(void)
     xrDestroySpace(s_local_space);
     s_local_space = XR_NULL_HANDLE;
 
-    /* task-7-round2 New Important 1: frontend_shutdown_finish() (3d.c) --
-     * this function's only caller -- runs from inside main_loop_iteration(),
-     * which itself can run from inside vr_run_frame() (between its own
-     * xrBeginFrame() and xrEndFrame() calls) on the game-initiated quit
-     * path: main_loop_iteration() reaches frontend's SHUTDOWN state and
-     * calls frontend_shutdown_finish() synchronously, still nested inside
-     * that same open vr_run_frame() frame. s_frame_open (set by
-     * vr_run_frame() around its own xrBeginFrame()/xrEndFrame() pair, see
-     * there) tells that case apart from the other path that reaches here:
-     * the runtime-initiated one, where vr_run_frame()'s event-poll loop
-     * already saw XR_SESSION_STATE_STOPPING and returned false with no
-     * frame outstanding (frontend_run_game_loop()'s `while (vr_run_frame())`
-     * loop then exits, and doexit() in main() drives the frontend shutdown
-     * state machine the rest of the way from the top level) -- s_session_
-     * running is still true on both paths (nothing else ever clears it), so
-     * it alone can't distinguish them.
-     *
-     * On the frame-open path: xrEndSession() requires XR_SESSION_STATE_
-     * STOPPING (OpenXR spec), but the session here is still just RUNNING --
-     * the runtime never asked us to stop, the game did. Reaching STOPPING
-     * the spec-clean way means xrRequestExitSession() + draining events
-     * until the runtime actually delivers that transition, which is
-     * unbounded async runtime-driven latency with no good way to bound it
-     * from inside a synchronous one-shot teardown call -- out of scope here
-     * (left as future work; the process is exiting regardless). Instead we
-     * just close out the still-open frame with an empty-layer xrEndFrame()
-     * (required regardless of which xrEndSession() strategy is used --
-     * destroying a session with a frame began-but-not-ended outstanding is
-     * itself invalid per the spec) and skip xrEndSession() entirely,
-     * proceeding straight to xrDestroySession(). vr_run_frame() checks
-     * s_session == XR_NULL_HANDLE (set at the bottom of this function)
-     * immediately after main_loop_iteration() returns and bails out before
-     * touching xrEndFrame() itself, so this is the only xrEndFrame() call
-     * for this tick -- see vr_run_frame() below.
-     *
-     * On the no-frame-open path, this is exactly the pre-existing, already-
-     * correct behavior: the runtime already drove the session to STOPPING,
-     * so xrEndSession() is exactly the call the spec expects here. */
-    if (s_session_running) {
-        if (s_frame_open) {
-            XrFrameEndInfo empty_frame_end_info = {
-                .type = XR_TYPE_FRAME_END_INFO,
-                .displayTime = s_frame_state.predictedDisplayTime,
-                .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
-                .layerCount = 0,
-                .layers = NULL,
-            };
-            XrResult end_result = xrEndFrame(s_session, &empty_frame_end_info);
-            if (XR_FAILED(end_result)) {
-                fprintf(stderr, "vr_runtime: vr_shutdown(): closing out the still-open frame failed "
-                                 "(XrResult=%d) -- continuing shutdown anyway\n", (int)end_result);
-            }
-            s_frame_open = false;
-            fprintf(stderr, "vr_runtime: vr_shutdown() reached mid-frame (game-initiated quit) -- "
-                             "session is still RUNNING, not STOPPING, so xrEndSession() would be invalid "
-                             "here per the OpenXR spec; skipping it and destroying the session directly "
-                             "instead. A fully spec-clean xrRequestExitSession()+drain-to-STOPPING sequence "
-                             "is left as future work.\n");
-        } else {
-            xrEndSession(s_session);
-        }
+    /* frontend_shutdown_finish() (3d.c) -- this function's only caller -- is
+     * reached two ways. (1) Game-initiated quit: from inside main_loop_iteration(),
+     * which itself runs between xrBeginFrame() and xrEndFrame() in vr_run_frame(),
+     * so s_frame_open is true and the frame must be closed with an empty layer
+     * before the session can end (vr_run_frame() notices s_session == NULL right
+     * after main_loop_iteration() returns and bails without touching OpenXR).
+     * (2) A quit forwarded from OpenXR (vr_forward_quit_to_game()): vr_run_frame()
+     * then runs the game's shutdown without OpenXR frames, so no frame is open and
+     * the runtime has usually ended the session already. */
+    if (s_frame_open) {
+        XrFrameEndInfo empty_frame_end_info = {
+            .type = XR_TYPE_FRAME_END_INFO,
+            .displayTime = s_frame_state.predictedDisplayTime,
+            .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
+            .layerCount = 0,
+            .layers = NULL,
+        };
+        XrResult end_result = xrEndFrame(s_session, &empty_frame_end_info);
+        if (XR_FAILED(end_result))
+            vr_log_line("WARN", "vr_shutdown(): closing the still-open frame failed (XrResult=%d); continuing", (int)end_result);
+        s_frame_open = false;
     }
+    vr_end_session_cleanly();
     xrDestroySession(s_session);
     s_session = XR_NULL_HANDLE;
     xrDestroyInstance(s_instance);
     s_instance = XR_NULL_HANDLE;
-    s_session_running = false;
+    vr_fsm_init(&s_fsm);
+}
+
+int vr_exit_status(void)
+{
+    return s_exit_status;
 }
 
 /* --- Live head tracking + per-eye MVP construction (Phase 2) ------------ */
@@ -942,39 +1035,39 @@ static Uint32 view_configs_height(Uint32 i)
 
 bool vr_run_frame(void)
 {
-    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
-    bool should_quit = false;
-    while (xrPollEvent(s_instance, &event) == XR_SUCCESS) {
-        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
-            XrEventDataSessionStateChanged *state_event = (XrEventDataSessionStateChanged *)&event;
-            if (state_event->state == XR_SESSION_STATE_READY && !s_session_running) {
-                XrSessionBeginInfo begin_info = {
-                    .type = XR_TYPE_SESSION_BEGIN_INFO,
-                    .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
-                };
-                if (XR_FAILED(xrBeginSession(s_session, &begin_info))) return false;
-                s_session_running = true;
-            } else if (state_event->state == XR_SESSION_STATE_STOPPING ||
-                       state_event->state == XR_SESSION_STATE_EXITING ||
-                       state_event->state == XR_SESSION_STATE_LOSS_PENDING) {
-                should_quit = true;
-            }
-        }
-        event.type = XR_TYPE_EVENT_DATA_BUFFER;
+    if (s_instance != XR_NULL_HANDLE && vr_poll_events())
+        vr_forward_quit_to_game();
+
+    if (s_quit_forwarded) {
+        /* No more OpenXR frames. Run the game's own shutdown; when its state machine
+         * reaches frontend_shutdown_finish() it calls vr_shutdown(), which ends the
+         * session (if the runtime has not already) and destroys the instance. */
+        s_should_render_this_tick = false;
+        s_frame_view_count_for_layer = 0;
+        bool keep_running = main_loop_iteration() != 0;
+        return keep_running && s_instance != XR_NULL_HANDLE;
     }
-    if (should_quit) return false;
 
-    if (!s_session_running) {
+    if (!vr_fsm_frames_allowed(&s_fsm)) {
         SDL_Delay(10);
         return true; /* keep looping, waiting for READY */
     }
 
+    XrResult frame_result;
     XrFrameWaitInfo frame_wait_info = { .type = XR_TYPE_FRAME_WAIT_INFO };
     s_frame_state = (XrFrameState){ .type = XR_TYPE_FRAME_STATE };
-    if (XR_FAILED(xrWaitFrame(s_session, &frame_wait_info, &s_frame_state))) return false;
+    frame_result = xrWaitFrame(s_session, &frame_wait_info, &s_frame_state);
+    if (XR_FAILED(frame_result)) {
+        vr_frame_call_failed("xrWaitFrame", frame_result);
+        return true; /* the next call runs the game's shutdown */
+    }
 
     XrFrameBeginInfo frame_begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
-    if (XR_FAILED(xrBeginFrame(s_session, &frame_begin_info))) return false;
+    frame_result = xrBeginFrame(s_session, &frame_begin_info);
+    if (XR_FAILED(frame_result)) {
+        vr_frame_call_failed("xrBeginFrame", frame_result);
+        return true;
+    }
     /* task-7-round2 New Important 1: marks this frame as open for
      * vr_shutdown() -- see its own comment and s_frame_open's declaration
      * above. Cleared below once this function's own xrEndFrame() runs (or
@@ -1036,7 +1129,10 @@ bool vr_run_frame(void)
     };
     XrResult end_result = xrEndFrame(s_session, &frame_end_info);
     s_frame_open = false;
-    if (XR_FAILED(end_result)) return false;
+    if (XR_FAILED(end_result)) {
+        vr_frame_call_failed("xrEndFrame", end_result);
+        return true; /* the next call runs the game's shutdown */
+    }
 
     if (s_frame_view_count_for_layer > 0) {
         s_vr_frames_presented++;
@@ -1057,6 +1153,15 @@ bool vr_run_frame(void)
             quit_game = 1;
             racing = 0;
         }
+        /* Test hook (ROLLER_VR_TEST_RUNTIME_EXIT_AFTER=N): ask the RUNTIME to end the
+         * session after N presented frames, without going through the game's quit
+         * path. The runtime then reports STOPPING exactly as it does when the user
+         * quits from SteamVR, exercising the runtime-initiated shutdown path. */
+        if (s_test_runtime_exit_after > 0 && s_vr_frames_presented == s_test_runtime_exit_after) {
+            XrResult exit_result = xrRequestExitSession(s_session);
+            vr_log_line("INFO", "test hook: xrRequestExitSession after %d frames -> XrResult=%d",
+                        s_vr_frames_presented, (int)exit_result);
+        }
     } else {
         s_vr_empty_frames++;
         if (s_vr_empty_frames <= 10)
diff --git a/PROJECTS/ROLLER/vr_runtime.h b/PROJECTS/ROLLER/vr_runtime.h
index a57dfa8..4241d61 100644
--- a/PROJECTS/ROLLER/vr_runtime.h
+++ b/PROJECTS/ROLLER/vr_runtime.h
@@ -21,6 +21,10 @@ bool vr_prepare_vulkan(SDL_GPUVulkanOptions *out_options);
 bool vr_init(void);
 void vr_shutdown(void);
 
+/* Process exit status for a VR run: 0 for an orderly end (game quit, or the
+ * runtime ending the session), 1 if an OpenXR frame call failed. */
+int vr_exit_status(void);
+
 /* Locates both eyes' head-tracked poses for the given predicted display
  * time. The FOV is always the runtime's. Orientation and position are merged
  * independently (vr_merge_view(), vr_view_merge.h): each comes from the located
@@ -55,10 +59,12 @@ void build_mvp_vr_eye(float mvp_out[16],
  * xrWaitFrame/xrBeginFrame and xrEndFrame. Call in a tight loop from
  * frontend_run_game_loop()'s VR branch (3d.c) in place of calling
  * main_loop_iteration() directly -- vr_run_frame() calls it internally.
- * Returns false when the game itself wants to quit (main_loop_iteration()
- * returned 0) or the OpenXR session is exiting/lost; true otherwise
- * (including the "not session_running yet" wait state, where the caller
- * should just keep looping).
+ * Returns false once the game has finished its own shutdown. A quit that
+ * originates in OpenXR (the runtime ending the session, an OpenXR failure) does
+ * not return early: it is forwarded to the game, and later calls run the game's
+ * loop without OpenXR frames until frontend_shutdown_finish() has run. Returns
+ * true otherwise (including the "not session_running yet" wait state, where the
+ * caller should just keep looping).
  *
  * While main_loop_iteration() runs, if it reaches race_draw()->
  * updatescreen(), updatescreen()'s g_bVRMode branch calls
diff --git a/build.zig b/build.zig
index 2b90bc2..5e1f23c 100644
--- a/build.zig
+++ b/build.zig
@@ -290,6 +290,7 @@ pub fn build(b: *std.Build) void {
                 "PROJECTS/ROLLER/vr_diag.c",
                 "PROJECTS/ROLLER/vr_log.c",
                 "PROJECTS/ROLLER/vr_ext_list.c",
+                "PROJECTS/ROLLER/vr_session_fsm.c",
             },
         });
 
PATCH
```

- [ ] **Step 2: Build**

```bash
zig build -Dvr -p /tmp/roller-vr-out 2>&1 | tail -20; echo "exit=${PIPESTATUS[0]}"
zig build test-vr-pure
python3 tools/check_roller_core_manifest.py
```

Expected: exit 0; all three pure test suites pass; the manifest check passes.

- [ ] **Step 3: Game-initiated quit** (the game asks to quit after 300 frames)

```bash
/config/projects/fatal/tools/vr-monado-run.sh /tmp/roller-vr-out/bin/roller-vr --frames 300
```

Expected (timestamps vary): exit status 0, and the runtime is asked to end the session and answers with STOPPING, in this order:

```text
[..] INFO xrRequestExitSession -> XrResult=0
[..] INFO session state -> 4 (actions=0x0)
[..] INFO session state -> 3 (actions=0x0)
[..] INFO session state -> 6 (actions=0x6)
[..] INFO xrEndSession -> XrResult=0
[..] INFO session state -> 1 (actions=0x0)
[..] INFO session state -> 8 (actions=0x0)
[..] INFO session ended after 1 ms
```

`actions=0x6` is `END_SESSION | REQUEST_QUIT`. The validation counts stay at 0.

- [ ] **Step 4: Runtime-initiated end**

Ask the runtime to end the session after 120 frames without going through the game's quit path:

```bash
/config/projects/fatal/tools/vr-monado-run.sh /tmp/roller-vr-out/bin/roller-vr --frames 100000 --runtime-exit-after 120
```

Expected: exit status 0 and, after the state changes 4, 3, 6 (`actions=0x6`), 1, 8, the game shutting itself down:

```text
[..] INFO test hook: xrRequestExitSession after 120 frames -> XrResult=0
[..] INFO asking the game to shut down (OpenXR is ending the session)
vr_runtime: shutdown (frames with layer=120, empty frames=1)
```

The `vr_runtime: shutdown` line is the proof that `frontend_shutdown_finish()` ran (teardown and the save path). Before this change that line was missing and the process exited 0 without it (Finding 3).

- [ ] **Step 5: The runtime dies mid-run**

Kill Monado by exact PID after 14 seconds. The game must shut down and exit with status 1, without waiting for a STOPPING that cannot come:

```bash
ssh fatal-gpu 'bash -s' <<'REMOTE'
cd /root/vr-test
export DISPLAY=:1 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy
rm -f roller-vr.log kill.exit
( timeout 100 ./roller-vr --vr --vr-frames 100000 --whiplash-root /root/roller_v2 --track-path /root/roller_v2/fatdata/TRACK1.TRK </dev/null >/dev/null 2>&1; echo $? > /root/vr-test/kill.exit ) &
WAITER=$!
sleep 14
MPID=$(pgrep -x monado-service | head -1)
echo "killing monado-service pid=$MPID"
kill "$MPID"
wait "$WAITER"
echo "roller-vr exit status: $(cat /root/vr-test/kill.exit)"
grep -E "^\[[0-9.]+s\] (INFO|WARN|ERROR) |vr_runtime: shutdown" roller-vr.log | grep -v "sdl:" | tail -6
REMOTE
```

Expected (the failing call and the order of the first two lines can differ):

```text
roller-vr exit status: 1
[..] ERROR xrEndFrame failed (XrResult=-17); shutting down
[..] INFO asking the game to shut down (OpenXR is ending the session)
[..] ERROR xrPollEvent failed (XrResult=-13); shutting down
vr_runtime: shutdown (frames with layer=<n>, empty frames=1)
[..] WARN xrRequestExitSession -> XrResult=-17
```

There must be **no** `did not report STOPPING within 2000 ms` line. The next run of `vr-monado-run.sh` restarts `monado-service` automatically.

- [ ] **Step 6: Commit**

```bash
git add PROJECTS/ROLLER/vr_runtime.c PROJECTS/ROLLER/vr_runtime.h PROJECTS/ROLLER/3d.c build.zig
git commit -m "vr: end the OpenXR session cleanly from either side and exit non-zero on failure

A runtime-initiated end (STOPPING/EXITING/LOSS_PENDING) used to make
vr_run_frame() return false; main() then called doexit(), which only
requests the frontend shutdown, so vr_shutdown(), ShutdownSDL() and the
save path never ran and the process exited 0. Forward such quits to the
game the way --vr-frames does and keep running the game's own loop,
without OpenXR frames, until it has shut itself down.

The session state machine (vr_session_fsm) now drives xrBeginSession and
xrEndSession; a game-initiated quit calls xrRequestExitSession and drains
to STOPPING (bounded) before destroying. OpenXR call failures and instance
loss take the same shutdown path and make the process exit 1.
ROLLER_VR_TEST_RUNTIME_EXIT_AFTER=N exercises the runtime-initiated path."
```

---

### Task 9: Windows cross-build and the PC test package

**Files:**
- Create: `/config/projects/fatal/tools/stage-vr-windows-sdks.sh`, `/config/projects/fatal/tools/package-vr-windows.sh` (both outside the ROLLER repo, so this task makes no ROLLER commit)
- Output: `/opt/vr-windows-sdk/` (staged SDKs), `/tmp/roller-vr-win/bin/roller-vr.exe`, `/config/projects/fatal/builds/windows-vr/` (gitignored build output, served over the Samba share)

**Interfaces:**
- Consumes: the Task 8 tree (`zig build -Dvr` for `x86_64-windows-gnu`); `builds/linux/fatdata` (platform-independent game data).
- Produces:
  - `stage-vr-windows-sdks.sh [DEST]` — downloads the Khronos OpenXR loader package `OpenXR.Loader.1.1.63.nupkg` (SHA-256 pinned in the script), lays out `DEST/openxr/{include,lib,bin}`, copies the Vulkan headers from `/usr/include/vulkan` (apt `libvulkan-dev`) to `DEST/vulkan/Include`, and generates `DEST/vulkan/Lib/vulkan-1.lib` from the 240 Vulkan 1.0 to 1.4 core commands with `zig dlltool`. The real `vulkan-1.dll` ships with the GPU driver on the PC; only the import library is needed to link.
  - `package-vr-windows.sh <prefix> [SDK dir]` — builds `builds/windows-vr/` with `roller-vr.exe`, `openxr_loader.dll`, `fatdata/`, `RUN-VR.bat` and `README-VR-TEST.txt`. Milestone 1 has no audio, so `midi/` and `audio/` (about 560 MB) are left out.

- [ ] **Step 1: Create the staging script**

Create `/config/projects/fatal/tools/stage-vr-windows-sdks.sh` and `chmod +x` it:

```bash
#!/bin/bash
# Stages what a cross-build of roller-vr.exe needs on this Linux host:
#   $DEST/openxr/{include,lib,bin}   Khronos OpenXR loader for Windows (headers, import lib, DLL)
#   $DEST/vulkan/{Include,Lib}       Vulkan headers and a generated vulkan-1.lib import library
# then prints the environment to build with. The real vulkan-1.dll ships with the
# GPU driver on the target PC; only the import library is needed to link.
#
# Usage: stage-vr-windows-sdks.sh [DEST]     (default /opt/vr-windows-sdk)
set -euo pipefail

DEST="${1:-/opt/vr-windows-sdk}"
OPENXR_VERSION="1.1.63"
NUPKG_URL="https://github.com/KhronosGroup/OpenXR-SDK/releases/download/release-${OPENXR_VERSION}/OpenXR.Loader.${OPENXR_VERSION}.nupkg"
NUPKG_SHA256="4e5a50a8807ef66f25180ff224e7d8150b594aa8ee4b07590f9ade55a8e98703"
VULKAN_HEADERS="/usr/include/vulkan"   # apt: libvulkan-dev

command -v zig >/dev/null || { echo "zig not found (needed for 'zig dlltool')" >&2; exit 1; }
command -v unzip >/dev/null || { echo "unzip not found" >&2; exit 1; }
[ -f "$VULKAN_HEADERS/vulkan_core.h" ] || { echo "Vulkan headers not found in $VULKAN_HEADERS (apt install libvulkan-dev)" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# --- OpenXR loader (pinned by SHA-256) ---------------------------------------
curl -fsSL -o "$WORK/loader.nupkg" "$NUPKG_URL"
echo "$NUPKG_SHA256  $WORK/loader.nupkg" | sha256sum -c - >/dev/null \
    || { echo "OpenXR loader package failed its SHA-256 check" >&2; exit 1; }
unzip -q -o "$WORK/loader.nupkg" -d "$WORK/nupkg"

rm -rf "$DEST/openxr"
mkdir -p "$DEST/openxr/include" "$DEST/openxr/lib" "$DEST/openxr/bin"
cp -r "$WORK/nupkg/include/openxr" "$DEST/openxr/include/"
cp "$WORK/nupkg/native/x64/release/lib/openxr_loader.lib" "$DEST/openxr/lib/"
cp "$WORK/nupkg/native/x64/release/bin/openxr_loader.dll" "$DEST/openxr/bin/"

# --- Vulkan headers + generated import library -------------------------------
rm -rf "$DEST/vulkan"
mkdir -p "$DEST/vulkan/Include" "$DEST/vulkan/Lib"
cp -r "$VULKAN_HEADERS" "$DEST/vulkan/Include/"
[ -d /usr/include/vk_video ] && cp -r /usr/include/vk_video "$DEST/vulkan/Include/"

# vulkan-1.dll exports the core commands (Vulkan 1.0-1.4) that vulkan_core.h
# declares before its first extension section.
python3 - "$VULKAN_HEADERS/vulkan_core.h" "$WORK/vulkan-1.def" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
core = text[: text.index("#define VK_KHR_")]
names = sorted(set(re.findall(r"typedef\s+[^;]*?\(VKAPI_PTR \*PFN_(vk\w+)\)", core)))
assert "vkGetInstanceProcAddr" in names and "vkCmdCopyImage" in names, "unexpected header layout"
open(sys.argv[2], "w").write("LIBRARY vulkan-1.dll\nEXPORTS\n" + "\n".join(names) + "\n")
print(f"vulkan-1.def: {len(names)} core commands")
PY
zig dlltool -m i386:x86-64 -D vulkan-1.dll -d "$WORK/vulkan-1.def" -l "$DEST/vulkan/Lib/vulkan-1.lib"

echo
echo "Staged in $DEST. Build with:"
echo "  OPENXR_SDK_PATH=$DEST/openxr VULKAN_SDK=$DEST/vulkan \\"
echo "    zig build -Dvr -Dtarget=x86_64-windows-gnu -p <prefix>"
echo "and ship $DEST/openxr/bin/openxr_loader.dll next to roller-vr.exe."
```

- [ ] **Step 2: Stage the SDKs**

```bash
/config/projects/fatal/tools/stage-vr-windows-sdks.sh /opt/vr-windows-sdk
ls -la /opt/vr-windows-sdk/openxr/lib /opt/vr-windows-sdk/openxr/bin /opt/vr-windows-sdk/vulkan/Lib
```

Expected: `vulkan-1.def: 240 core commands`, then `Staged in /opt/vr-windows-sdk.` and the build hint; `openxr_loader.lib` (about 15 KB), `openxr_loader.dll` (2,671,104 bytes) and `vulkan-1.lib` (about 58 KB) exist.

- [ ] **Step 3: Cross-build for Windows**

```bash
cd /config/projects/fatal/ROLLER
OPENXR_SDK_PATH=/opt/vr-windows-sdk/openxr VULKAN_SDK=/opt/vr-windows-sdk/vulkan \
  zig build -Dvr -Dtarget=x86_64-windows-gnu -p /tmp/roller-vr-win 2>&1 | tail -20; echo "exit=${PIPESTATUS[0]}"
file /tmp/roller-vr-win/bin/roller-vr.exe | cut -c1-120
strings -a /tmp/roller-vr-win/bin/roller-vr.exe | grep -i -E "^(openxr_loader|vulkan-1)\.dll$" | sort -u
```

Expected: exit 0 after about 13 minutes (SDL is compiled for Windows too); `PE32+ executable for MS Windows ... (GUI) x86-64`; both `openxr_loader.dll` and `vulkan-1.dll` listed. The executable is a GUI-subsystem process, which is why `roller-vr.log` and the Windows message box exist (there is no console).

- [ ] **Step 4: Create the packaging script**

Create `/config/projects/fatal/tools/package-vr-windows.sh` and `chmod +x` it:

```bash
#!/bin/bash
# Assembles builds/windows-vr/ (what gets copied to the Windows PC) from a roller-vr.exe cross-build.
#
# Usage: package-vr-windows.sh <build prefix containing bin/roller-vr.exe> [SDK dir]
#   SDK dir defaults to /opt/vr-windows-sdk (see stage-vr-windows-sdks.sh)
#
# Milestone 1 has no audio, so only the game data (fatdata/, platform independent, taken
# from builds/linux) and the OpenXR loader DLL ship next to the exe; midi/ and audio/
# come with the input/audio milestone.
set -euo pipefail

PREFIX="${1:?usage: package-vr-windows.sh <prefix containing bin/roller-vr.exe> [sdk-dir]}"
SDK="${2:-/opt/vr-windows-sdk}"
FATAL="$(cd "$(dirname "$0")/.." && pwd)"   # projects/fatal
OUT="$FATAL/builds/windows-vr"

[ -f "$PREFIX/bin/roller-vr.exe" ] || { echo "missing $PREFIX/bin/roller-vr.exe" >&2; exit 1; }
[ -f "$SDK/openxr/bin/openxr_loader.dll" ] || { echo "missing $SDK/openxr/bin/openxr_loader.dll (run stage-vr-windows-sdks.sh)" >&2; exit 1; }
[ -d "$FATAL/builds/linux/fatdata" ] || { echo "missing $FATAL/builds/linux/fatdata" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"
cp "$PREFIX/bin/roller-vr.exe" "$OUT/"
cp "$SDK/openxr/bin/openxr_loader.dll" "$OUT/"
cp -r "$FATAL/builds/linux/fatdata" "$OUT/fatdata"

# %~dp0 ends in a backslash, which would escape the closing quote; "%~dp0." avoids that.
cat > "$OUT/RUN-VR.bat" <<'BAT'
@echo off
cd /d "%~dp0"
roller-vr.exe --vr --whiplash-root "%~dp0." --track-path "%~dp0fatdata\TRACK1.TRK"
echo roller-vr exited with status %ERRORLEVEL%
pause
BAT

cat > "$OUT/README-VR-TEST.txt" <<'TXT'
ROLLER VR - milestone 1 test (Windows 11, SteamVR)

What this checks: the Windows build starts under SteamVR's OpenXR runtime, shows the
game's attract-mode race, and shuts down cleanly. No headset is needed if SteamVR's
null (virtual) headset driver works for this - that is believed but not verified.

1. Make SteamVR the active OpenXR runtime (SteamVR > Settings > Developer >
   "Set SteamVR as OpenXR runtime"), if it is not already.

2. Optional, to run without a headset: enable SteamVR's null driver. The settings live in
   <Steam>\config\steamvr.vrsettings. Do NOT copy keys from this file blindly: open
   <SteamVR install>\drivers\null\resources\settings\default.vrsettings, and use the
   "driver_null" block from there plus, under "steamvr", the keys "requireHmd": false,
   "forcedDriver": "null" and "activateMultipleDrivers": true (from memory - check that
   these names match your SteamVR version). Restart SteamVR; a simulated headset window
   should appear.

3. Start SteamVR, then double-click RUN-VR.bat in this folder.

4. Expected: a window with the attract-mode race (both eyes) in SteamVR's headset view.
   roller-vr.log appears next to roller-vr.exe.

5. Quit through SteamVR (its status window menu > Exit SteamVR, or close the VR view).
   The game window should close by itself. RUN-VR.bat then prints
   "roller-vr exited with status 0".

6. Send back roller-vr.log (or leave it in place if you ran this from the share). Also
   note anything on screen: a black window, an error box, a crash.
TXT

echo "packaged $OUT:"
ls -la "$OUT"
```

- [ ] **Step 5: Package**

```bash
/config/projects/fatal/tools/package-vr-windows.sh /tmp/roller-vr-win /opt/vr-windows-sdk
```

Expected: `packaged /config/projects/fatal/builds/windows-vr:` and a listing with `fatdata/` (about 32 MB), `openxr_loader.dll`, `README-VR-TEST.txt`, `roller-vr.exe` (about 19 MB) and `RUN-VR.bat`. On Windows the folder is `\\192.168.14.95\config\projects\fatal\builds\windows-vr`.

- [ ] **Step 6: Hand over to the user** (no commit; nothing in the ROLLER repo changed)

Tell the user the package is ready and ask them to follow `README-VR-TEST.txt` on the Windows 11 PC: SteamVR as the OpenXR runtime, optionally its null (virtual headset) driver, then `RUN-VR.bat`. Ask for `roller-vr.log` back (or leave it in place if they run from the share). What "pass" means for the no-headset part of M1: the log shows the runtime's name and extension list, the negotiation lines, `session state -> 5` (FOCUSED), frames being submitted, and, after quitting through SteamVR, a clean shutdown with `RUN-VR.bat` reporting status 0. If `roller-vr.log` is empty or missing, or the window stays black, that is itself the finding: the log path is `SDL_GetBasePath()` plus `roller-vr.log`, and a GUI process's stdout/stderr redirection is the one part of `vr_log_open()` that cannot be tested on Linux.

---

### Task 10: Final verification and documentation

**Files:**
- Modify: `docs/superpowers/specs/2026-09-25-roller-vr-phase3-playable-design.md`, `docs/superpowers/specs/2026-09-23-roller-vr-phase2-design.md` (ROLLER repo); `/config/projects/fatal/CLAUDE.md`, `/config/CLAUDE.md`, `/root/.claude/projects/-config/memory/fatal_project.md`, `/root/.claude/projects/-config/memory/MEMORY.md`
- Create: `/tmp/task10_docs.py` (the edit script below; it is not kept)

**Interfaces:**
- Consumes: the fork commit hash from Task 6 and the working Windows package from Task 9.
- Produces: docs that state what M1 actually delivered, including the one spec correction (Finding 1), and a clean tree.

- [ ] **Step 1: Verify the default build is untouched**

```bash
cd /config/projects/fatal/ROLLER
zig build -p /tmp/roller-plain-out 2>&1 | tail -5; echo "exit=${PIPESTATUS[0]}"
nm /tmp/roller-plain-out/bin/roller | grep -c -E " [TtDdBb] (vr_(init|shutdown|run_frame|prepare_vulkan|log_|diag_|fsm_|ext_list_|exit_status|choose_vk_binding)|SDL_GetGPUDeviceProperties|SDL_GetGPUTextureProperties)"
strings /tmp/roller-plain-out/bin/roller | grep -c -E "roller-vr.log|XR_KHR_vulkan_enable|SDL.gpu.device.create.vulkan.options"
```

Expected: exit 0 (about 5 minutes; it builds against the original pinned SDL, not the fork); both counts are `0`.

- [ ] **Step 2: Run every check once more**

```bash
zig build test-vr-pure
python3 tools/check_roller_core_manifest.py
python3 tools/check_source_set_drift.py; echo "exit=$?"
/config/projects/fatal/tools/vr-monado-run.sh /tmp/roller-vr-out/bin/roller-vr --frames 300 | sed -n '1p;/validation counts/,$p'
git status --short; git -C /root/sdl-vr-fork status --short
```

Expected: the three unit-test suites pass; the manifest check passes; the drift check exits 1 with only the pre-existing `build.zig:172: .files must be an inline .{...} list` (Finding 4); the Monado run reports `exit status: 0` and all five validation counts `0`; both repos are clean (everything committed in Tasks 1 to 8).

- [ ] **Step 3: Update the docs**

Create `/tmp/task10_docs.py`:

```python
#!/usr/bin/env python3
"""Task 10: bring the docs in line with what milestone M1 delivered.

Usage: FORK_COMMIT=<hash> TODAY=<YYYY-MM-DD> python3 task10_docs.py [ROOT]
ROOT defaults to /config (the tree that holds projects/fatal and CLAUDE.md); the memory
directory can be overridden with MEMORY_DIR. Every edit asserts that its anchor exists
exactly once, so a doc that has drifted fails loudly instead of being half-edited."""
import os, sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "/config"
MEM = os.environ.get("MEMORY_DIR", "/root/.claude/projects/-config/memory")
FORK = os.environ["FORK_COMMIT"]
TODAY = os.environ["TODAY"]

def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, f"{path}: expected exactly 1 of {old[:80]!r}, found {s.count(old)}"
    open(path, "w").write(s.replace(old, new))

def replace_between(path, start_marker, end_marker, new):
    s = open(path).read()
    a = s.index(start_marker)
    b = s.index(end_marker, a)
    open(path, "w").write(s[:a] + new + s[b:])

R = ROOT + "/projects/fatal/ROLLER"
SPEC3 = R + "/docs/superpowers/specs/2026-09-25-roller-vr-phase3-playable-design.md"
SPEC2 = R + "/docs/superpowers/specs/2026-09-23-roller-vr-phase2-design.md"
FATAL = ROOT + "/projects/fatal/CLAUDE.md"
ROOTMD = ROOT + "/CLAUDE.md"

# ---- Phase 3 spec: correct the extension mechanism, record how the lifecycle fix works ----
replace_between(SPEC3, "### Vulkan instance/device extensions (Phase 2 open item, fixed here)", "### Acceptance", """### Vulkan instance/device extensions (Phase 2 open item, fixed here)

Phase 2 found that `vr_runtime.c` never enables what the OpenXR runtime asks for, because SDL creates the `VkInstance` (API 1.0) and `VkDevice` itself. Checked against the OpenXR spec source (`khr_vulkan_enable.adoc` and `khr_vulkan_enable2.adoc`, 2026-09-25), the fix is not just to pass extensions in:

- **The code used the wrong extension.** Under `XR_KHR_vulkan_enable2` the graphics binding requires the instance and device to have been created with `xrCreateVulkanInstanceKHR`/`xrCreateVulkanDeviceKHR`, which SDL_GPU's ownership of creation rules out (Monado tolerates it; another runtime may not). The application-created model is `XR_KHR_vulkan_enable` (v1): enable an API version inside the range from `xrGetVulkanGraphicsRequirementsKHR` and every extension listed by `xrGetVulkanInstanceExtensionsKHR`/`xrGetVulkanDeviceExtensionsKHR` (these two functions do not exist under enable2), and use the physical device from `xrGetVulkanGraphicsDeviceKHR`. `roller-vr` now uses v1 when the runtime advertises it and falls back to enable2 with a logged warning. Whether SteamVR advertises v1 is unverified; the log lists every extension the runtime offers.
- The `XrInstance` is created before the Vulkan device, because its answers are needed to create the device: `vr_prepare_vulkan()` runs in `InitSDL()`'s VR branch before the GPU device exists.
- The SDL fork gets a hook shaped like upstream 3.4's `SDL_GPUVulkanOptions` (API version and extra instance/device extensions), so a later move to 3.4 or 3.6 is cheap. An unsupported requested extension fails device creation with a message that names it.
- Requesting Vulkan 1.1 (inside the runtime's range) is what clears the Phase 2 validation messages: `VUID-VkImageMemoryBarrier-None-09120` and `-image-09118` (12 raw lines each in the Phase 2 300-frame run) come from the swapchain-release barrier using `VK_QUEUE_FAMILY_EXTERNAL`, which Vulkan allows only with `VK_KHR_external_memory` enabled or an instance API version of at least 1.1. Verified on Monado: both counts are 0.
- Considered and rejected: `xrCreateVulkanInstanceKHR`/`xrCreateVulkanDeviceKHR`, because SDL owns creation.

### Session lifecycle (Phase 2 follow-ups 2 and 5)

- Full state machine (`vr_session_fsm`, pure C, unit-tested): READY begins the session; SYNCHRONIZED, VISIBLE and FOCUSED are tracked (M3 gates input on FOCUSED); STOPPING ends the session with `xrEndSession`; EXITING and LOSS_PENDING request the game's quit but never end the session (only STOPPING allows `xrEndSession`).
- **A runtime-initiated end used to skip teardown and the save path**, and the M1 investigation confirmed exactly why: `vr_run_frame()` returned false, `main()` called `doexit()`, which only *requests* the frontend shutdown, so `vr_shutdown()`, `ShutdownSDL()` and the save path never ran and the process exited 0 (observed on Monado). Fix: a quit that originates in OpenXR is forwarded to the game the way `--vr-frames` does (`quit_game = 1; racing = 0`), and `vr_run_frame()` keeps running the game's own loop, without OpenXR frames, until the game has shut itself down.
- A game-initiated quit calls `xrRequestExitSession` and drains events until the runtime reports STOPPING (bounded at 2 s), then `xrEndSession`, replacing the old shortcut that skipped `xrEndSession`.
- Failures of `xrPollEvent`, `xrWaitFrame`, `xrBeginFrame`, `xrEndFrame` and `xrBeginSession`, and instance loss, take the same shutdown path and make the process exit 1 (`main()` and `doexit()` return `vr_exit_status()`).
- Startup failure: message to the log and, on Windows, a message box; exit status 1 (`ErrorBoxExit()` exited 0).
- `roller-vr.log` next to the exe: runtime name, every extension the runtime advertises, the negotiated Vulkan version and extensions, session state transitions, and (from M2) frame times. This is the main diagnostic for test runs on the user's PC.
- Test hook: `ROLLER_VR_TEST_RUNTIME_EXIT_AFTER=N` makes the runtime end the session after N presented frames, exercising the runtime-initiated path; `projects/fatal/tools/vr-monado-run.sh` runs it on `fatal-gpu`.

""")
edit(SPEC3,
     "output goes to `builds/windows-vr/` with `midi/` and `audio/` as for the current Windows build.",
     "output goes to `builds/windows-vr/` with `roller-vr.exe`, `openxr_loader.dll`, `fatdata/`, a `RUN-VR.bat` and a `README-VR-TEST.txt`. `midi/` and `audio/` (about 560 MB) are left out until the input/audio milestone, since M1 has no audio.")
edit(SPEC3,
     "- On the LXC (Monado, validation layers): no `09118`/`09120`; runtime-initiated end and game-initiated quit both exit cleanly with the save path executed.",
     "- On the LXC (Monado, validation layers): no `09118`/`09120`; runtime-initiated end and game-initiated quit both exit 0 through `vr_shutdown()` (the save path); killing the runtime mid-run shuts down the same way and exits 1.")
edit(SPEC3, "The M1 plan is written (`docs/superpowers/plans/2026-09-25-vr-phase3-m1-windows-steamvr-lifecycle.md`); nothing is implemented yet.",
     f"M1 implemented {TODAY} (plan `docs/superpowers/plans/2026-09-25-vr-phase3-m1-windows-steamvr-lifecycle.md`), the Windows/SteamVR check on the user's PC still pending; M2 to M4 not started.")

# ---- Phase 2 spec: point the resolved items at Phase 3 ----
edit(SPEC2, "## Follow-ups (priority order; Phase 3 and later)\n",
     "## Follow-ups (priority order; Phase 3 and later)\n\n> Items 2, 4 and 5 were resolved by Phase 3 milestone M1 (`2026-09-25-roller-vr-phase3-playable-design.md`, section M1). Note that item 4 turned out to need `XR_KHR_vulkan_enable` (v1), not just extension passing; the details are in that spec.\n")

# ---- projects/fatal/CLAUDE.md ----
edit(FATAL, "(written 2026-09-25, awaiting the user's review; the M2 to M4 plans are not written yet)",
     f"(executed {TODAY}); the M2 to M4 plans are not written yet")
edit(FATAL, "(139 added lines), letting OpenXR bind to the same device SDL_GPU owns (`XR_KHR_vulkan_enable2`).",
     f"and, since Phase 3 M1, an `SDL_GPUVulkanOptions` hook (API version and extra instance/device extensions; fork commit `{FORK}`), letting OpenXR bind to the same device SDL_GPU owns (`XR_KHR_vulkan_enable`, with enable2 only as a logged fallback: enable2 would need runtime-created Vulkan objects).")
edit(FATAL, "plus `vr_runtime.c`, `-DROLLER_VR_BUILD=1`). `roller-vr --vr",
     "plus `vr_runtime.c`, `vr_diag.c`, `vr_log.c`, `vr_ext_list.c`, `vr_session_fsm.c`, `-DROLLER_VR_BUILD=1`). `roller-vr --vr")
edit(FATAL, "Windows VR cross-build (`-Dtarget=x86_64-windows-gnu`) panics without `OPENXR_SDK_PATH`/`VULKAN_SDK`; with dummy paths it compiles and fails only at link. Phase 3 M1 stages the real SDKs.",
     "`roller-vr.log` is written next to the exe (stdout, stderr and SDL_Log); a startup failure exits 1. Pure-C unit tests: `zig build test-vr-pure`. Windows VR cross-build: `tools/stage-vr-windows-sdks.sh` stages the OpenXR loader (Khronos 1.1.63 NuGet package, SHA-256 pinned) and a generated `vulkan-1.lib` in `/opt/vr-windows-sdk`, then `OPENXR_SDK_PATH=/opt/vr-windows-sdk/openxr VULKAN_SDK=/opt/vr-windows-sdk/vulkan zig build -Dvr -Dtarget=x86_64-windows-gnu`; `tools/package-vr-windows.sh` assembles `builds/windows-vr/` (exe, `openxr_loader.dll`, `fatdata/`, `RUN-VR.bat`, `README-VR-TEST.txt`). Monado runs on `fatal-gpu`: `tools/vr-monado-run.sh <roller-vr> [--frames N] [--runtime-exit-after N]` (the env var `ROLLER_VR_TEST_RUNTIME_EXIT_AFTER=N` makes the runtime end the session after N frames).")
edit(FATAL, "no in-headset exit; runtime-initiated session end skips teardown/save. (2)", "no in-headset exit. (2)")
edit(FATAL, " **Open item:** `vr_runtime.c` never enables the Vulkan instance/device extensions the runtime asks for (SDL creates the `VkInstance` at API 1.0), visible as `VUID-VkImageMemoryBarrier-None-09120`/`image-09118` in validation; likely to matter on SteamVR. Windows was only checked for C-compile. Phase 3 addresses all of these.",
     " Phase 3 addresses the rest (M2 to M4).")
edit(FATAL, "acceptance split into headset-free and headset-gated. See the spec.",
     f"acceptance split into headset-free and headset-gated. See the spec. **M1 implemented {TODAY}:** Vulkan interop negotiated per `XR_KHR_vulkan_enable` and passed to SDL_GPU (Monado: `VUID-VkImageMemoryBarrier-None-09120`/`image-09118` went from 12 to 0), an orderly session end from either side (a runtime-initiated end used to skip `vr_shutdown()`/save and exit 0; verified fixed on Monado, and killing the runtime exits 1), `roller-vr.log`, and a Windows build packaged in `builds/windows-vr/`. **Pending:** the user's run under SteamVR on the Windows PC (README-VR-TEST.txt), whose log shows which Vulkan interop extension SteamVR offers.")
edit(FATAL, "Phase 3 (playable VR) spec approved; the M1 plan is written and awaiting the user's review.",
     f"Phase 3 (playable VR) spec approved; M1 (Windows/SteamVR bring-up + session lifecycle) implemented {TODAY}, its SteamVR check on the user's PC pending; M2 (head tracking) is next.")

# ---- /config/CLAUDE.md ----
edit(ROOTMD, "Phase 3 (playable VR for Windows/SteamVR + Steam Frame) spec approved 2026-09-25; the M1 plan (Windows/SteamVR bring-up + session lifecycle) is written and awaiting review.",
     f"Phase 3 (playable VR for Windows/SteamVR + Steam Frame) spec approved 2026-09-25; M1 (Windows/SteamVR bring-up + session lifecycle) implemented {TODAY}, SteamVR check on the user's PC pending.")

# ---- memory ----
edit(MEM + "/fatal_project.md",
     "awaiting the user's review and choice of execution method (subagent-driven or native); M2 to M4 plans come later. Hard gate: no implementation before the user approves the plan.",
     f"approved by the user; M1 plan written (`plans/2026-09-25-vr-phase3-m1-windows-steamvr-lifecycle.md`) and executed {TODAY} (fork commit `{FORK}`): Vulkan interop via XR_KHR_vulkan_enable + SDL_GPUVulkanOptions (validation errors 12 to 0 on Monado), clean session end from either side (a runtime-initiated end used to skip vr_shutdown and exit 0), roller-vr.log, Windows build in builds/windows-vr/. Pending: the user's SteamVR run on the PC. Next: write the M2 (head tracking) plan; its first task is the cull-site investigation.")
edit(MEM + "/MEMORY.md",
     "spec approved 2026-09-25, M1 plan (Windows/SteamVR bring-up + session lifecycle) written and awaiting review",
     f"spec approved 2026-09-25; M1 (Windows/SteamVR bring-up + session lifecycle) implemented {TODAY}, SteamVR check on the user's PC pending, M2 (head tracking) plan next")
print("docs updated")
```

Run it, then check the size of the project doc:

```bash
FORK_COMMIT=$(git -C /root/sdl-vr-fork log --format=%h -1) TODAY=$(date +%F) python3 /tmp/task10_docs.py
git diff --stat
wc -c /config/projects/fatal/CLAUDE.md
```

Expected: `docs updated`; the diff touches the two spec files in the ROLLER repo. `projects/fatal/CLAUDE.md` should come out near 15.7 KB; the rules ask for a compaction pass above roughly 15 to 20 KB, so if it is over 16 KB, trim wording in the VR section (keep every load-bearing fact) instead of adding text.

- [ ] **Step 4: Commit the spec updates**

```bash
git add docs/superpowers/specs/2026-09-25-roller-vr-phase3-playable-design.md docs/superpowers/specs/2026-09-23-roller-vr-phase2-design.md
git commit -m "docs(vr): correct the M1 Vulkan mechanism and record what M1 delivered

The Phase 3 spec said to pass xrGetVulkan*ExtensionsKHR results in while
keeping XR_KHR_vulkan_enable2, but those functions belong to
XR_KHR_vulkan_enable, and enable2 requires runtime-created Vulkan objects.
Records the corrected mechanism, why Vulkan 1.1 clears the Phase 2
validation errors, how the runtime-initiated shutdown fix works, and the
M1 status. The Phase 2 follow-ups now point at Phase 3."
git log --oneline -12
```

Do not push unless the user asks.

- [ ] **Step 5: Report to the user**

Summarize in a few lines: what M1 verified without a headset (Monado: interop negotiated, validation errors 12 to 0, clean session end from either side, a runtime that dies exits 1, no-runtime exits 1 with a clear message; the Windows build links and packages), what is still open (the SteamVR run on the PC; keyboard focus and everything headset-gated), and that the M2 plan is next. Offer to push the ROLLER commits and, as before, ask before creating a remote for the fork.
