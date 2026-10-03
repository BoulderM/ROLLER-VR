/* PROJECTS/ROLLER/vr_runtime.c
 *
 * Opens the OpenXR session against SDL_GPU's Vulkan device and drives the
 * real game loop in stereo. vr_init() sets up the device, instance, system,
 * session, swapchains, reference space and command pool. Each frame is
 * handled by vr_run_frame() and vr_present_stereo_frame(), and vr_shutdown()
 * tears the session down.
 */
#include "vr_runtime.h"
#include "vr_view_merge.h"
#include "vr_ext_list.h"
#include "vr_log.h"
#include "vr_session_fsm.h"
#include "vr_camera_compose.h"

#include <SDL3/SDL.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* draw_road()/main_loop_iteration()/g_pGameRenderer declarations (3d.h
 * already pulls in game_render.h for the game_render_* wrapper calls used
 * below). */
#include "3d.h"
#include "roller.h"  /* ROLLERGetGPUDevice() -- see vr_init() below */
#include "view.h"    /* calculateview() -- see vr_present_stereo_frame() below */
#include "frontend.h"  /* quit_game, racing -- --vr-frames bounded-run exit */
#include "car.h"       /* Car[], CarBox, numcars -- vr_resolve_meters_to_world(), periodic diag log */
#include "drawtrk3.h"  /* TrackSize -- periodic diag log (chunk count) */
#include "building.h"  /* NumVisibleBuildings -- periodic diag log (building count) */
#include "loadtrak.h"   /* TRAK_LEN -- far-plane diag (track chunks beyond the eye far plane) */

static SDL_GPUDevice *s_gpu;
static VkInstance s_vk_instance;
static VkPhysicalDevice s_vk_physical_device;
static VkDevice s_vk_device;
static VkQueue s_vk_queue;
static Uint32 s_vk_queue_family_index;
static XrInstance s_instance;
static XrSystemId s_system_id;
static XrSession s_session;
static Uint32 s_view_count;
static XrViewConfigurationView *s_view_configs;
static XrSwapchain *s_swapchains;
static XrSpace s_local_space;
static VkCommandPool s_command_pool;
static VkCommandBuffer s_command_buffer;
static VrSessionFsm s_fsm; /* session lifecycle decisions, see vr_session_fsm.h */
static VrVkBinding s_vk_binding = VR_VK_BINDING_NONE;
static VrExtList s_vk_instance_exts;
static VrExtList s_vk_device_exts;
static const char *s_vk_instance_ext_ptrs[VR_EXT_LIST_MAX];
static const char *s_vk_device_ext_ptrs[VR_EXT_LIST_MAX];
static int s_exit_status = 0;             /* process exit status, see vr_exit_status() */
static bool s_quit_forwarded = false;     /* OpenXR asked to quit and the game has been told, see vr_forward_quit_to_game() */
static int s_test_runtime_exit_after = 0; /* ROLLER_VR_TEST_RUNTIME_EXIT_AFTER=N, see vr_run_frame() */

/* VrSessionState mirrors XrSessionState numerically so the state machine can be
 * fed the runtime's values directly. */
_Static_assert((int)XR_SESSION_STATE_IDLE == (int)VR_STATE_IDLE &&
               (int)XR_SESSION_STATE_READY == (int)VR_STATE_READY &&
               (int)XR_SESSION_STATE_SYNCHRONIZED == (int)VR_STATE_SYNCHRONIZED &&
               (int)XR_SESSION_STATE_VISIBLE == (int)VR_STATE_VISIBLE &&
               (int)XR_SESSION_STATE_FOCUSED == (int)VR_STATE_FOCUSED &&
               (int)XR_SESSION_STATE_STOPPING == (int)VR_STATE_STOPPING &&
               (int)XR_SESSION_STATE_LOSS_PENDING == (int)VR_STATE_LOSS_PENDING &&
               (int)XR_SESSION_STATE_EXITING == (int)VR_STATE_EXITING,
               "VrSessionState must match XrSessionState");
/* True from the moment vr_run_frame()'s own
 * xrBeginFrame() succeeds until its own xrEndFrame() call (success or
 * failure) or vr_shutdown()'s frame-close-out below runs. Lets vr_shutdown()
 * tell apart the two shutdown paths that reach it (see vr_shutdown()'s own
 * comment): called mid-frame, from inside vr_run_frame() via
 * main_loop_iteration()->frontend_shutdown_finish() (game-initiated quit),
 * vs. called at the top level after vr_run_frame()'s own event loop already
 * saw XR_SESSION_STATE_STOPPING and returned (runtime-initiated shutdown,
 * no frame outstanding). */
static bool s_frame_open = false;
/* Declared here, above the per-frame section, so vr_shutdown() (defined before
 * that section) can read s_frame_state.predictedDisplayTime when closing out a
 * still-open frame on the game-initiated quit path (see vr_shutdown()'s own
 * comment). Still set fresh by vr_run_frame() every tick via xrWaitFrame(). */
static XrFrameState s_frame_state;

/* These write through vr_log_line() (not raw fprintf(stderr, ...)) because
 * roller-vr.exe is a GUI-subsystem Windows build with no console: stderr
 * has no OS handle to write to, so anything not routed through the log file
 * is invisible on Windows. (An earlier version of vr_log.c redirected the
 * process's own stdout/stderr onto the log file so plain fprintf(stderr, ...)
 * calls like these would still land in it; that redirection was removed
 * 2026-10-02 for being unreliable on real Windows hardware -- see vr_log.c's
 * header comment -- which is also why these specific calls needed updating:
 * without the redirection they went nowhere, reproduced 2026-10-02 as a real
 * SteamVR run whose log went straight from "using XR_KHR_vulkan_enable" to
 * the generic top-level "Couldn't prepare the OpenXR runtime" with the actual
 * reason lost.) */
static bool fail(const char *what)
{
    vr_log_line("ERROR", "%s", what);
    return false;
}

static bool fail_xr(const char *what, XrResult result)
{
    vr_log_line("ERROR", "%s (XrResult=%d)", what, (int)result);
    return false;
}

static bool fail_vk(const char *what, VkResult result)
{
    vr_log_line("ERROR", "%s (VkResult=%d)", what, (int)result);
    return false;
}

/* --- Vulkan interop negotiation ----------------------------------------- */

/* Converts an XrVersion to a Vulkan API version, clamping the fields so an
 * "unbounded" runtime maximum (0xffff minor, 0xffffffff patch) cannot overflow
 * VK_MAKE_API_VERSION's bit fields. */
static Uint32 vr_xr_to_vk_version(XrVersion v)
{
    Uint32 major = XR_VERSION_MAJOR(v);
    Uint32 minor = XR_VERSION_MINOR(v);
    Uint32 patch = XR_VERSION_PATCH(v);
    if (major > 127) major = 127;
    if (minor > 1023) minor = 1023;
    if (patch > 4095) patch = 4095;
    return VK_MAKE_API_VERSION(0, major, minor, patch);
}

/* Vulkan 1.1 makes VK_QUEUE_FAMILY_EXTERNAL usable in barriers without enabling
 * VK_KHR_external_memory (VUID-VkImageMemoryBarrier-None-09120), which the
 * swapchain image release in vr_present_stereo_frame() relies on. Ask for it
 * unless the runtime's supported range says otherwise. */
static Uint32 vr_pick_vulkan_api_version(const XrGraphicsRequirementsVulkanKHR *req)
{
    Uint32 wanted = VK_MAKE_API_VERSION(0, 1, 1, 0);
    Uint32 min_v = vr_xr_to_vk_version(req->minApiVersionSupported);
    Uint32 max_v = vr_xr_to_vk_version(req->maxApiVersionSupported);
    if (wanted < min_v) wanted = min_v;
    if (wanted > max_v) wanted = max_v;
    return wanted;
}

typedef XrResult (XRAPI_PTR *VrGetExtensionStringFn)(XrInstance, XrSystemId, uint32_t, uint32_t *, char *);

/* Two-call idiom for xrGetVulkanInstanceExtensionsKHR / xrGetVulkanDeviceExtensionsKHR:
 * fetch the runtime's space-separated extension string and add the names to `out`. */
static bool vr_query_extension_string(VrGetExtensionStringFn fn, const char *what, VrExtList *out)
{
    uint32_t needed = 0;
    XrResult xr_result = fn(s_instance, s_system_id, 0, &needed, NULL);
    if (XR_FAILED(xr_result))
        return fail_xr(what, xr_result);
    char *buffer = calloc(needed ? needed : 1, 1);
    if (!buffer)
        return fail("out of memory reading the runtime's Vulkan extension list");
    xr_result = fn(s_instance, s_system_id, needed, &needed, buffer);
    if (XR_FAILED(xr_result)) {
        free(buffer);
        return fail_xr(what, xr_result);
    }
    vr_log_line("INFO", "%s: %s", what, buffer[0] ? buffer : "(none)");
    bool stored = vr_ext_list_add_spaced(out, buffer);
    free(buffer);
    if (!stored)
        return fail("could not store every Vulkan extension name the runtime listed");
    return true;
}

bool vr_prepare_vulkan(SDL_GPUVulkanOptions *out_options)
{
    if (!out_options)
        return fail("vr_prepare_vulkan: out_options is NULL");
    SDL_zerop(out_options);
    vr_fsm_init(&s_fsm);
    vr_ext_list_init(&s_vk_instance_exts);
    vr_ext_list_init(&s_vk_device_exts);
    s_exit_status = 0;

    const char *test_exit = getenv("ROLLER_VR_TEST_RUNTIME_EXIT_AFTER");
    s_test_runtime_exit_after = test_exit ? atoi(test_exit) : 0;

    /* --- 1. Which Vulkan interop extension does the runtime offer? --- */
    uint32_t ext_count = 0;
    XrResult xr_result = xrEnumerateInstanceExtensionProperties(NULL, 0, &ext_count, NULL);
    if (XR_FAILED(xr_result))
        return fail_xr("xrEnumerateInstanceExtensionProperties (count) failed -- is an OpenXR runtime installed and active?", xr_result);
    XrExtensionProperties *ext_props = calloc(ext_count ? ext_count : 1, sizeof(XrExtensionProperties));
    if (!ext_props)
        return fail("out of memory enumerating OpenXR extensions");
    for (uint32_t i = 0; i < ext_count; i++)
        ext_props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    xr_result = xrEnumerateInstanceExtensionProperties(NULL, ext_count, &ext_count, ext_props);
    if (XR_FAILED(xr_result)) {
        free(ext_props);
        return fail_xr("xrEnumerateInstanceExtensionProperties failed", xr_result);
    }
    bool has_enable_v1 = false, has_enable2 = false;
    vr_log_line("INFO", "OpenXR runtime advertises %u instance extensions:", (unsigned)ext_count);
    for (uint32_t i = 0; i < ext_count; i++) {
        vr_log_line("INFO", "  %s (v%u)", ext_props[i].extensionName, (unsigned)ext_props[i].extensionVersion);
        if (strcmp(ext_props[i].extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0)
            has_enable_v1 = true;
        if (strcmp(ext_props[i].extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0)
            has_enable2 = true;
    }
    free(ext_props);

    s_vk_binding = vr_choose_vk_binding(has_enable_v1, has_enable2);
    if (s_vk_binding == VR_VK_BINDING_NONE)
        return fail("the OpenXR runtime offers neither XR_KHR_vulkan_enable nor XR_KHR_vulkan_enable2");
    if (s_vk_binding == VR_VK_BINDING_ENABLE2_FALLBACK)
        vr_log_line("WARN", "runtime lacks XR_KHR_vulkan_enable; falling back to XR_KHR_vulkan_enable2, whose "
                            "valid usage expects the runtime to create the Vulkan instance/device (SDL_GPU creates them here)");
    else
        vr_log_line("INFO", "using XR_KHR_vulkan_enable (the application creates the Vulkan instance/device)");

    /* --- 2. Create the OpenXR instance and find the headset --- */
    const char *enabled_extensions[] = {
        s_vk_binding == VR_VK_BINDING_ENABLE_V1 ? XR_KHR_VULKAN_ENABLE_EXTENSION_NAME
                                                : XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME
    };
    XrInstanceCreateInfo instance_create_info = { .type = XR_TYPE_INSTANCE_CREATE_INFO };
    strncpy(instance_create_info.applicationInfo.applicationName, "ROLLER VR",
            XR_MAX_APPLICATION_NAME_SIZE - 1);
    instance_create_info.applicationInfo.applicationVersion = 1;
    /* XR_CURRENT_API_VERSION from our 1.1.63 SDK headers requests core OpenXR
     * 1.1; SteamVR's runtime only implements 1.0 and rejects that with
     * XR_ERROR_API_VERSION_UNSUPPORTED (confirmed on the user's real hardware,
     * 2026-10-02 -- Monado, which does implement 1.1, never showed this).
     * Nothing here uses any 1.1-only feature, so request 1.0 instead. */
    instance_create_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    instance_create_info.enabledExtensionCount = 1;
    instance_create_info.enabledExtensionNames = enabled_extensions;

    s_instance = XR_NULL_HANDLE;
    xr_result = xrCreateInstance(&instance_create_info, &s_instance);
    if (XR_FAILED(xr_result))
        return fail_xr("xrCreateInstance failed", xr_result);

    XrInstanceProperties instance_props = { .type = XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties(s_instance, &instance_props)))
        vr_log_line("INFO", "OpenXR runtime: %s (%u.%u.%u)", instance_props.runtimeName,
                    (unsigned)XR_VERSION_MAJOR(instance_props.runtimeVersion),
                    (unsigned)XR_VERSION_MINOR(instance_props.runtimeVersion),
                    (unsigned)XR_VERSION_PATCH(instance_props.runtimeVersion));

    XrSystemGetInfo system_get_info = {
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };
    xr_result = xrGetSystem(s_instance, &system_get_info, &s_system_id);
    if (XR_FAILED(xr_result))
        return fail_xr("xrGetSystem failed (no HMD available from the active OpenXR runtime?)", xr_result);

    /* --- 3. What Vulkan API version and extensions does the runtime need? --- */
    const char *requirements_fn_name = s_vk_binding == VR_VK_BINDING_ENABLE_V1
        ? "xrGetVulkanGraphicsRequirementsKHR" : "xrGetVulkanGraphicsRequirements2KHR";
    PFN_xrGetVulkanGraphicsRequirementsKHR pfn_get_requirements = NULL;
    xr_result = xrGetInstanceProcAddr(s_instance, requirements_fn_name, (PFN_xrVoidFunction *)&pfn_get_requirements);
    if (XR_FAILED(xr_result) || !pfn_get_requirements)
        return fail_xr("failed to load the runtime's Vulkan graphics requirements function", xr_result);
    XrGraphicsRequirementsVulkanKHR requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
    xr_result = pfn_get_requirements(s_instance, s_system_id, &requirements);
    if (XR_FAILED(xr_result))
        return fail_xr("the runtime's Vulkan graphics requirements query failed", xr_result);
    Uint32 api_version = vr_pick_vulkan_api_version(&requirements);
    vr_log_line("INFO", "runtime supports Vulkan API %u.%u..%u.%u; requesting %u.%u.%u",
                (unsigned)VK_API_VERSION_MAJOR(vr_xr_to_vk_version(requirements.minApiVersionSupported)),
                (unsigned)VK_API_VERSION_MINOR(vr_xr_to_vk_version(requirements.minApiVersionSupported)),
                (unsigned)VK_API_VERSION_MAJOR(vr_xr_to_vk_version(requirements.maxApiVersionSupported)),
                (unsigned)VK_API_VERSION_MINOR(vr_xr_to_vk_version(requirements.maxApiVersionSupported)),
                (unsigned)VK_API_VERSION_MAJOR(api_version), (unsigned)VK_API_VERSION_MINOR(api_version),
                (unsigned)VK_API_VERSION_PATCH(api_version));

    if (s_vk_binding == VR_VK_BINDING_ENABLE_V1) {
        PFN_xrGetVulkanInstanceExtensionsKHR pfn_instance_exts = NULL;
        PFN_xrGetVulkanDeviceExtensionsKHR pfn_device_exts = NULL;
        xr_result = xrGetInstanceProcAddr(s_instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction *)&pfn_instance_exts);
        if (XR_FAILED(xr_result) || !pfn_instance_exts)
            return fail_xr("failed to load xrGetVulkanInstanceExtensionsKHR", xr_result);
        xr_result = xrGetInstanceProcAddr(s_instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction *)&pfn_device_exts);
        if (XR_FAILED(xr_result) || !pfn_device_exts)
            return fail_xr("failed to load xrGetVulkanDeviceExtensionsKHR", xr_result);
        if (!vr_query_extension_string(pfn_instance_exts, "runtime-required Vulkan instance extensions", &s_vk_instance_exts))
            return false;
        if (!vr_query_extension_string(pfn_device_exts, "runtime-required Vulkan device extensions", &s_vk_device_exts))
            return false;
    }

    out_options->vulkan_api_version = api_version;
    out_options->instance_extension_count = vr_ext_list_ptrs(&s_vk_instance_exts, s_vk_instance_ext_ptrs, VR_EXT_LIST_MAX);
    out_options->instance_extension_names = s_vk_instance_ext_ptrs;
    out_options->device_extension_count = vr_ext_list_ptrs(&s_vk_device_exts, s_vk_device_ext_ptrs, VR_EXT_LIST_MAX);
    out_options->device_extension_names = s_vk_device_ext_ptrs;
    return true;
}

bool vr_init(void)
{
    if (s_instance == XR_NULL_HANDLE) {
        fail("vr_init() called before vr_prepare_vulkan() created the OpenXR instance");
        return false;
    }

    /* --- 1. Adopt the game's existing SDL_GPU device (Vulkan) --- */
    /* Adopt the game's device (s_pGPUDevice / ROLLERGetGPUDevice()) rather
     * than calling SDL_CreateGPUDevice() here. A second VkDevice would be
     * invalid: vr_present_stereo_frame() waits fences and copies VkImages
     * that belong to the game's device (via game_render_end_frame_to_vr_eye()),
     * and a VkImage or fence from one VkDevice cannot be used in commands
     * submitted to a different VkDevice's queue. OpenXR therefore binds to the
     * exact same VkDevice/VkQueue the game renders with, and every downstream
     * Vulkan call in this file operates on that one shared device.
     *
     * ROLLERGetGPUDevice() is guaranteed non-NULL here because vr_init() is
     * only ever called from main() (3d.c), immediately after InitSDL()
     * returns success -- InitSDL()'s g_bVRMode branch is exactly what
     * creates this device. The NULL check below is a defensive guard
     * against that call-ordering invariant ever being violated (e.g. by a
     * future refactor), not an expected runtime path. */
    s_gpu = ROLLERGetGPUDevice();
    if (!s_gpu) {
        fail("ROLLERGetGPUDevice() returned NULL -- vr_init() must be called after InitSDL() "
             "has successfully created the game's GPU device");
        return false;
    }
    vr_log_line("INFO", "SDL_GPU driver: %s", SDL_GetGPUDeviceDriver(s_gpu));

    SDL_PropertiesID gpu_props = SDL_GetGPUDeviceProperties(s_gpu);
    if (!gpu_props) {
        fail("SDL_GetGPUDeviceProperties failed");
        return false;
    }

    s_vk_instance = (VkInstance)SDL_GetPointerProperty(gpu_props, SDL_PROP_GPU_DEVICE_VULKAN_INSTANCE_POINTER, NULL);
    s_vk_physical_device = (VkPhysicalDevice)SDL_GetPointerProperty(gpu_props, SDL_PROP_GPU_DEVICE_VULKAN_PHYSICAL_DEVICE_POINTER, NULL);
    s_vk_device = (VkDevice)SDL_GetPointerProperty(gpu_props, SDL_PROP_GPU_DEVICE_VULKAN_DEVICE_POINTER, NULL);
    s_vk_queue = (VkQueue)SDL_GetPointerProperty(gpu_props, SDL_PROP_GPU_DEVICE_VULKAN_QUEUE_POINTER, NULL);
    s_vk_queue_family_index = (Uint32)SDL_GetNumberProperty(gpu_props, SDL_PROP_GPU_DEVICE_VULKAN_QUEUE_FAMILY_INDEX_NUMBER, 0xFFFFFFFF);

    if (!s_vk_instance || !s_vk_physical_device || !s_vk_device || !s_vk_queue || s_vk_queue_family_index == 0xFFFFFFFF) {
        fail("SDL_GPU did not expose Vulkan device properties (is this the patched SDL fork? is the active backend Vulkan?)");
        return false;
    }
    vr_log_line("INFO", "got VkInstance=%p VkDevice=%p queueFamilyIndex=%u",
                (void *)s_vk_instance, (void *)s_vk_device, s_vk_queue_family_index);

    /* --- 2. Tell the runtime which physical device we use --- */
    /* Required by the spec before creating a Vulkan-backed session even though
     * SDL_GPU already chose the device: skipping it makes xrCreateSession fail
     * with XR_ERROR_VALIDATION_FAILURE on at least Monado. The v1 and v2 entry
     * points differ; the graphics binding struct is the same for both. */
    XrResult xr_result;
    VkPhysicalDevice xr_required_physical_device = VK_NULL_HANDLE;
    if (s_vk_binding == VR_VK_BINDING_ENABLE_V1) {
        PFN_xrGetVulkanGraphicsDeviceKHR pfn_get_vk_device = NULL;
        xr_result = xrGetInstanceProcAddr(s_instance, "xrGetVulkanGraphicsDeviceKHR",
                                           (PFN_xrVoidFunction *)&pfn_get_vk_device);
        if (XR_FAILED(xr_result) || !pfn_get_vk_device) {
            fail_xr("failed to load xrGetVulkanGraphicsDeviceKHR", xr_result);
            return false;
        }
        xr_result = pfn_get_vk_device(s_instance, s_system_id, s_vk_instance, &xr_required_physical_device);
        if (XR_FAILED(xr_result)) {
            fail_xr("xrGetVulkanGraphicsDeviceKHR failed", xr_result);
            return false;
        }
    } else {
        PFN_xrGetVulkanGraphicsDevice2KHR pfn_get_vk_device = NULL;
        xr_result = xrGetInstanceProcAddr(s_instance, "xrGetVulkanGraphicsDevice2KHR",
                                           (PFN_xrVoidFunction *)&pfn_get_vk_device);
        if (XR_FAILED(xr_result) || !pfn_get_vk_device) {
            fail_xr("failed to load xrGetVulkanGraphicsDevice2KHR", xr_result);
            return false;
        }
        XrVulkanGraphicsDeviceGetInfoKHR vk_device_get_info = {
            .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
            .systemId = s_system_id,
            .vulkanInstance = s_vk_instance,
        };
        xr_result = pfn_get_vk_device(s_instance, &vk_device_get_info, &xr_required_physical_device);
        if (XR_FAILED(xr_result)) {
            fail_xr("xrGetVulkanGraphicsDevice2KHR failed", xr_result);
            return false;
        }
    }
    if (xr_required_physical_device != s_vk_physical_device) {
        fail("SDL_GPU's Vulkan physical device does not match the physical device the OpenXR "
             "runtime requires -- this can happen on multi-GPU systems; creating the session "
             "against a mismatched device/physicalDevice pair is undefined behavior per the Vulkan spec");
        return false;
    }

    /* --- 3. Create the session against our existing Vulkan device --- */
    XrGraphicsBindingVulkanKHR vk_binding = {
        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, /* XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR is the same value */
        .instance = s_vk_instance,
        .physicalDevice = xr_required_physical_device,
        .device = s_vk_device,
        .queueFamilyIndex = s_vk_queue_family_index,
        .queueIndex = 0,
    };
    XrSessionCreateInfo session_create_info = {
        .type = XR_TYPE_SESSION_CREATE_INFO,
        .next = &vk_binding,
        .systemId = s_system_id,
    };
    s_session = XR_NULL_HANDLE;
    xr_result = xrCreateSession(s_instance, &session_create_info, &s_session);
    if (XR_FAILED(xr_result)) {
        fail_xr("xrCreateSession failed", xr_result);
        return false;
    }
    vr_log_line("INFO", "OpenXR session created");

    /* Pick a swapchain format the runtime actually lists, instead of assuming
     * VK_FORMAT_R8G8B8A8_UNORM (confirmed on the user's real hardware,
     * 2026-10-02: SteamVR's compositor doesn't list that at all, only
     * VK_FORMAT_R8G8B8A8_SRGB/B8G8R8A8_SRGB among 8-bit formats -- Monado
     * does list UNORM, which is why this never showed up there). Preferred in
     * order of how closely they match the resolved eye texture's own
     * R8G8B8A8_UNORM layout: UNORM is an exact match; R8G8B8A8_SRGB keeps the
     * same channel order (the game's framebuffer is already gamma-encoded
     * 8-bit color like any classic raster game, which is what an SRGB-tagged
     * image expects, so this should display correctly, not just compile);
     * the B8G8R8A8 variants have R and B swapped relative to the source
     * texture's byte layout, so vkCmdCopyImage would carry that swap through
     * unchanged -- last resort, not yet handled, flagged if it's ever picked. */
    static const VkFormat PREFERRED_SWAPCHAIN_FORMATS[] = {
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_B8G8R8A8_SRGB,
    };
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    uint32_t format_count = 0;
    if (XR_SUCCEEDED(xrEnumerateSwapchainFormats(s_session, 0, &format_count, NULL)) && format_count > 0) {
        int64_t *formats = calloc(format_count, sizeof(int64_t));
        if (formats && XR_SUCCEEDED(xrEnumerateSwapchainFormats(s_session, format_count, &format_count, formats))) {
            char list[512] = "";
            for (uint32_t i = 0; i < format_count; i++) {
                char one[24];
                snprintf(one, sizeof(one), "%s%lld", i ? " " : "", (long long)formats[i]);
                if (strlen(list) + strlen(one) < sizeof(list))
                    strcat(list, one);
            }
            vr_log_line("INFO", "runtime swapchain formats (VkFormat values): %s", list);

            for (size_t p = 0; p < SDL_arraysize(PREFERRED_SWAPCHAIN_FORMATS) && swapchain_format == VK_FORMAT_UNDEFINED; p++) {
                for (uint32_t i = 0; i < format_count; i++) {
                    if (formats[i] == (int64_t)PREFERRED_SWAPCHAIN_FORMATS[p]) {
                        swapchain_format = PREFERRED_SWAPCHAIN_FORMATS[p];
                        break;
                    }
                }
            }
            if (swapchain_format == VK_FORMAT_B8G8R8A8_UNORM || swapchain_format == VK_FORMAT_B8G8R8A8_SRGB)
                vr_log_line("WARN", "picked VkFormat %d, which has R/B swapped relative to the game's "
                                    "R8G8B8A8 framebuffer -- colors will likely look wrong until the "
                                    "eye copy swaps channels to match", (int)swapchain_format);
            else if (swapchain_format != VK_FORMAT_UNDEFINED)
                vr_log_line("INFO", "picked swapchain VkFormat %d", (int)swapchain_format);
        }
        free(formats);
    }
    if (swapchain_format == VK_FORMAT_UNDEFINED) {
        fail("the OpenXR runtime lists no swapchain format this code knows how to use "
             "(wanted R8G8B8A8_UNORM, R8G8B8A8_SRGB, B8G8R8A8_UNORM or B8G8R8A8_SRGB; "
             "see the logged list above)");
        return false;
    }

    /* --- 5. Enumerate the view configuration (expect 2 views for stereo) --- */
    s_view_count = 0;
    xr_result = xrEnumerateViewConfigurationViews(s_instance, s_system_id,
                                                   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                   0, &s_view_count, NULL);
    if (XR_FAILED(xr_result) || s_view_count == 0) {
        fail_xr("xrEnumerateViewConfigurationViews (count) failed", xr_result);
        return false;
    }
    if (s_view_count > 8) {
        char msg[128];
        snprintf(msg, sizeof(msg), "view count %u exceeds supported maximum of 8", s_view_count);
        fail(msg);
        return false;
    }
    s_view_configs = calloc(s_view_count, sizeof(XrViewConfigurationView));
    for (Uint32 i = 0; i < s_view_count; i++) {
        s_view_configs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    }
    xr_result = xrEnumerateViewConfigurationViews(s_instance, s_system_id,
                                                   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                   s_view_count, &s_view_count, s_view_configs);
    if (XR_FAILED(xr_result)) {
        fail_xr("xrEnumerateViewConfigurationViews failed", xr_result);
        return false;
    }
    vr_log_line("INFO", "view count = %u, view[0] recommended %ux%u",
                s_view_count, s_view_configs[0].recommendedImageRectWidth,
                s_view_configs[0].recommendedImageRectHeight);

    /* --- 6. Create one swapchain per eye --- */
    s_swapchains = calloc(s_view_count, sizeof(XrSwapchain));
    for (Uint32 i = 0; i < s_view_count; i++) {
        XrSwapchainCreateInfo swapchain_create_info = {
            .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
            .format = (int64_t)swapchain_format,
            .sampleCount = 1,
            .width = s_view_configs[i].recommendedImageRectWidth,
            .height = s_view_configs[i].recommendedImageRectHeight,
            .faceCount = 1,
            .arraySize = 1,
            .mipCount = 1,
        };
        xr_result = xrCreateSwapchain(s_session, &swapchain_create_info, &s_swapchains[i]);
        if (XR_FAILED(xr_result)) {
            fail_xr("xrCreateSwapchain failed", xr_result);
            return false;
        }
    }
    vr_log_line("INFO", "created %u swapchains", s_view_count);

    /* --- 6b. Create a reference space for the projection layer. A composition
     * layer's `space` must reference a real, previously-created XrSpace. LOCAL
     * with an identity pose is the reference space both xrLocateViews() (head
     * tracking, vr_locate_views()) and the projection layer are expressed in. */
    s_local_space = XR_NULL_HANDLE;
    XrReferenceSpaceCreateInfo space_create_info = {
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
        .poseInReferenceSpace = { .orientation = { .w = 1.0f }, .position = { 0 } },
    };
    xr_result = xrCreateReferenceSpace(s_session, &space_create_info, &s_local_space);
    if (XR_FAILED(xr_result)) {
        fail_xr("xrCreateReferenceSpace failed", xr_result);
        return false;
    }

    /* --- 7. Vulkan command pool/buffer for the clear commands --- */
    VkCommandPoolCreateInfo pool_create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = s_vk_queue_family_index,
    };
    VkResult vk_result = vkCreateCommandPool(s_vk_device, &pool_create_info, NULL, &s_command_pool);
    if (vk_result != VK_SUCCESS) {
        fail_vk("vkCreateCommandPool failed", vk_result);
        return false;
    }

    VkCommandBufferAllocateInfo cb_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = s_command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    vk_result = vkAllocateCommandBuffers(s_vk_device, &cb_alloc_info, &s_command_buffer);
    if (vk_result != VK_SUCCESS) {
        fail_vk("vkAllocateCommandBuffers failed", vk_result);
        return false;
    }

    return true;
}


/* --- Session lifecycle -------------------------------------------------- */

#define VR_EXIT_DRAIN_TIMEOUT_MS 2000

/* The game only shuts down by running its frontend state machine (RACING -> ... ->
 * SHUTDOWN -> QUIT inside main_loop_iteration()). Merely leaving the frame loop skips
 * frontend_shutdown_finish(), so vr_shutdown(), ShutdownSDL() and the save path never
 * run and main() exits 0 -- observed on Monado. So a quit that originates in OpenXR
 * (the runtime ending the session, or an OpenXR failure) is forwarded to the game the
 * way --vr-frames does it, and vr_run_frame() then keeps running the game's loop,
 * without OpenXR frames, until the game has shut itself down. */
static void vr_forward_quit_to_game(void)
{
    if (s_quit_forwarded)
        return;
    s_quit_forwarded = true;
    vr_log_line("INFO", "asking the game to shut down (OpenXR is ending the session)");
    quit_game = 1;
    racing = 0;
}

static void vr_frame_call_failed(const char *what, XrResult result)
{
    vr_log_line("ERROR", "%s failed (XrResult=%d); shutting down", what, (int)result);
    s_exit_status = 1;
    vr_forward_quit_to_game();
}

/* Polls every pending OpenXR event, feeds session state changes to the state
 * machine and carries out the actions it returns. Returns true when the runtime
 * wants the application to quit. */
static bool vr_poll_events(void)
{
    bool quit = false;
    XrResult poll_result;
    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    while ((poll_result = xrPollEvent(s_instance, &event)) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *state_event = (const XrEventDataSessionStateChanged *)&event;
            unsigned int actions = vr_fsm_on_state_changed(&s_fsm, (VrSessionState)state_event->state);
            vr_log_line("INFO", "session state -> %d (actions=0x%x)", (int)state_event->state, actions);
            if (actions & VR_ACTION_BEGIN_SESSION) {
                XrSessionBeginInfo begin_info = {
                    .type = XR_TYPE_SESSION_BEGIN_INFO,
                    .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                };
                XrResult begin_result = xrBeginSession(s_session, &begin_info);
                if (XR_FAILED(begin_result)) {
                    vr_frame_call_failed("xrBeginSession", begin_result);
                    quit = true;
                } else {
                    vr_fsm_session_begun(&s_fsm);
                }
            }
            if (actions & VR_ACTION_END_SESSION) {
                XrResult end_result = xrEndSession(s_session);
                vr_log_line(XR_FAILED(end_result) ? "WARN" : "INFO", "xrEndSession -> XrResult=%d", (int)end_result);
                vr_fsm_session_ended(&s_fsm);
            }
            if (actions & VR_ACTION_REQUEST_QUIT)
                quit = true;
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            /* Instance loss takes the same non-zero-exit path as a
             * failed OpenXR call (vr_frame_call_failed), not the zero-exit path a
             * clean runtime-initiated session end (STOPPING/EXITING/LOSS_PENDING,
             * handled above) takes -- the instance itself is gone, which is a
             * runtime failure, not an orderly end. */
            vr_log_line("ERROR", "OpenXR instance loss pending; shutting down");
            s_exit_status = 1;
            vr_forward_quit_to_game();
            quit = true;
        }
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
    /* XR_EVENT_UNAVAILABLE (a success code) just means the queue is empty; a failure
     * such as XR_ERROR_INSTANCE_LOST means the runtime is gone. */
    if (XR_FAILED(poll_result)) {
        vr_frame_call_failed("xrPollEvent", poll_result);
        quit = true;
    }
    return quit;
}

/* Ends a running session the spec-clean way when the GAME is quitting:
 * xrRequestExitSession, then drain events until the runtime reports STOPPING
 * (vr_poll_events() calls xrEndSession). Bounded, because the runtime answers
 * asynchronously; on timeout the still-running session is simply destroyed. When
 * the runtime is the one ending the session there is nothing to request or wait for. */
static void vr_end_session_cleanly(void)
{
    if (!vr_fsm_frames_allowed(&s_fsm) || vr_fsm_runtime_is_ending(&s_fsm))
        return;

    if (vr_fsm_should_request_exit(&s_fsm)) {
        XrResult exit_result = xrRequestExitSession(s_session);
        vr_log_line(XR_FAILED(exit_result) ? "WARN" : "INFO", "xrRequestExitSession -> XrResult=%d", (int)exit_result);
        if (XR_FAILED(exit_result))
            return; /* no STOPPING will follow: the runtime is gone or the session is not running */
    }

    Uint64 started = SDL_GetTicks();
    while (vr_fsm_frames_allowed(&s_fsm) && s_exit_status == 0 &&
           SDL_GetTicks() - started < VR_EXIT_DRAIN_TIMEOUT_MS) {
        vr_poll_events();
        if (!vr_fsm_frames_allowed(&s_fsm))
            break;
        SDL_Delay(10);
    }
    if (vr_fsm_frames_allowed(&s_fsm))
        vr_log_line("WARN", "the runtime did not report STOPPING within %d ms; destroying the session directly",
                    VR_EXIT_DRAIN_TIMEOUT_MS);
    else
        vr_log_line("INFO", "session ended after %llu ms", (unsigned long long)(SDL_GetTicks() - started));
}

/* Number of ticks that actually rendered and submitted a stereo layer, for
 * the --vr-frames N bounded-run option (g_iVRMaxFrames) and progress logging. */
static int s_vr_frames_presented = 0;
static int s_vr_empty_frames = 0;   /* ticks submitted with layerCount = 0 */

void vr_shutdown(void)
{
    vr_log_line("INFO", "shutdown (frames with layer=%d, empty frames=%d)",
                s_vr_frames_presented, s_vr_empty_frames);
    /* s_gpu is the game's own shared GPU device (see vr_init()'s comment
     * above), not one this runtime owns -- its lifecycle belongs entirely
     * to InitSDL()/ShutdownSDL() (roller.c). vr_shutdown() must wait for it
     * to go idle before tearing down the Vulkan objects THIS runtime does
     * own (the command pool/buffer), but must NOT destroy the device and
     * must NOT call SDL_Quit(): both belong to ShutdownSDL(), which runs
     * immediately after this function returns (frontend_shutdown_finish(),
     * 3d.c) -- the device is still valid here. Calling either here would
     * destroy/quit out from under ShutdownSDL()'s own subsequent cleanup
     * (double-free / double-quit). */
    vkDeviceWaitIdle(s_vk_device);
    vkFreeCommandBuffers(s_vk_device, s_command_pool, 1, &s_command_buffer);
    vkDestroyCommandPool(s_vk_device, s_command_pool, NULL);
    s_command_buffer = VK_NULL_HANDLE;
    s_command_pool = VK_NULL_HANDLE;

    for (Uint32 i = 0; i < s_view_count; i++) {
        xrDestroySwapchain(s_swapchains[i]);
    }
    free(s_swapchains);
    free(s_view_configs);
    s_swapchains = NULL;
    s_view_configs = NULL;
    s_view_count = 0;

    xrDestroySpace(s_local_space);
    s_local_space = XR_NULL_HANDLE;

    /* frontend_shutdown_finish() (3d.c) -- this function's only caller -- is
     * reached two ways. (1) Game-initiated quit: from inside main_loop_iteration(),
     * which itself runs between xrBeginFrame() and xrEndFrame() in vr_run_frame(),
     * so s_frame_open is true and the frame must be closed with an empty layer
     * before the session can end (vr_run_frame() notices s_session == NULL right
     * after main_loop_iteration() returns and bails without touching OpenXR).
     * (2) A quit forwarded from OpenXR (vr_forward_quit_to_game()): vr_run_frame()
     * then runs the game's shutdown without OpenXR frames, so no frame is open and
     * the runtime has usually ended the session already. */
    if (s_frame_open) {
        XrFrameEndInfo empty_frame_end_info = {
            .type = XR_TYPE_FRAME_END_INFO,
            .displayTime = s_frame_state.predictedDisplayTime,
            .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
            .layerCount = 0,
            .layers = NULL,
        };
        XrResult end_result = xrEndFrame(s_session, &empty_frame_end_info);
        if (XR_FAILED(end_result))
            vr_log_line("WARN", "vr_shutdown(): closing the still-open frame failed (XrResult=%d); continuing", (int)end_result);
        s_frame_open = false;
    }
    vr_end_session_cleanly();
    xrDestroySession(s_session);
    s_session = XR_NULL_HANDLE;
    xrDestroyInstance(s_instance);
    s_instance = XR_NULL_HANDLE;
    vr_fsm_init(&s_fsm);
}

int vr_exit_status(void)
{
    return s_exit_status;
}

/* --- Live head tracking + per-eye MVP construction ---------------------- */

/* Last successfully merged views (each component is its most recent valid
 * value, or the identity/zero fallback if it never was valid). */
static XrView s_last_good_views[2];
static bool   s_have_last_good_views = false;

bool vr_locate_views(XrTime predicted_display_time, XrView out_views[2])
{
    if (g_bVRFakeHead) {
        /* --vr-fake-head: deterministic scripted pose, driven by the presented-
         * frame counter (not wall-clock) so runs are reproducible. A full 360-
         * degree yaw sweep over 360 frames (1 degree per frame), with small
         * sine-wave pitch/roll and position offsets layered on top so the sweep
         * exercises more than pure yaw. Yaw is 0/90/180/270 at presented frames
         * 0/90/180/270 exactly. Quaternion is yaw(Y) * pitch(X) * roll(Z) in
         * OpenXR's axis convention (+X right, +Y up, -Z forward). */
        int frame = s_vr_frames_presented;
        float yawRad   = (float)(frame % 360) * (float)M_PI / 180.0f;
        float pitchRad = sinf((float)frame * 0.02f) * 0.15f;  /* +-~8.6 deg */
        float rollRad  = sinf((float)frame * 0.013f) * 0.10f; /* +-~5.7 deg */
        float cy = cosf(yawRad * 0.5f),   sy = sinf(yawRad * 0.5f);
        float cp = cosf(pitchRad * 0.5f), sp = sinf(pitchRad * 0.5f);
        float cr = cosf(rollRad * 0.5f),  sr = sinf(rollRad * 0.5f);
        XrQuaternionf q = {
            .x = sp*cy*cr + cp*sy*sr,
            .y = cp*sy*cr - sp*cy*sr,
            .z = cp*cy*sr - sp*sy*cr,
            .w = cp*cy*cr + sp*sy*sr,
        };
        XrVector3f pos = {
            .x = sinf((float)frame * 0.05f) * 0.05f,
            .y = 0.0f,
            .z = cosf((float)frame * 0.05f) * 0.02f,
        };
        /* ~42-degree half-angles, a plausible stand-in HMD FOV. */
        XrFovf fov = { -0.733f, 0.733f, 0.733f, -0.733f };
        for (int i = 0; i < 2; i++) {
            out_views[i] = (XrView){ .type = XR_TYPE_VIEW,
                                     .pose = { .orientation = q, .position = pos },
                                     .fov = fov };
        }
        return true;
    }

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
        vr_log_line("ERROR", "xrLocateViews failed or returned <2 views (XrResult=%d, count=%u)",
                    (int)xr_result, out_count);
        if (!s_have_last_good_views) return false;
        memcpy(out_views, s_last_good_views, sizeof(s_last_good_views));
        return true;
    }

    /* xrLocateViews succeeded, so the FOV is real even when the pose flags are
     * not. Merge per component (orientation / position) with the last-good
     * views; see vr_merge_view() (vr_view_merge.h). */
    for (int i = 0; i < 2; i++) {
        out_views[i] = vr_merge_view(&views[i], view_state.viewStateFlags,
                                     s_have_last_good_views ? &s_last_good_views[i] : NULL);
    }
    memcpy(s_last_good_views, out_views, sizeof(s_last_good_views));
    s_have_last_good_views = true;
    return true;
}

/* OpenXR reports positions in meters; ROLLER's world units are an unlabeled
 * scale, so every meter-scale quantity (head position delta, eye separation,
 * near plane) is multiplied by vr_resolve_meters_to_world() before it's
 * combined with world-unit camera state.
 *
 * The scale is derived from the car model rather than a guessed constant:
 * the car's hitbox length in world units (CalcCarSizes(), car.c, min/max over
 * each design's own coords) divided by a reference real-world car length.
 * Orientation-agnostic: the longer of the hitbox's X and Y spans is taken as
 * the length. In the shipped CarDesigns X is the long axis (~1311 world units
 * for 13 of 14 designs, ~1621 for f1wack; Y ~745, Z ~412), so X is what gets
 * picked. --vr-meters-to-world replaces the derived value entirely. */
#define VR_REFERENCE_CAR_LENGTH_M 4.2f

float vr_resolve_meters_to_world(void)
{
    static float s_cached_length_wu = -1.0f; /* car length the cached scale came from */
    static float s_cached_scale = -1.0f;
    static bool s_warned_degenerate = false;

    if (g_fVRMetersToWorldOverride > 0.0f)
        return g_fVRMetersToWorldOverride;

    /* One scale for the whole session, derived from Car[0]'s hitbox so it never
     * changes mid-session. The camera follows ViewType[0], which intro replay cuts
     * can switch to another car: 13 of 14 designs are within ~0.3% of each other's
     * length, but f1wack (index 12) is ~24% longer, so on an f1wack cut the car
     * reads ~24% longer than Car[0]'s. Accepted. */
    int carDesignIdx = Car[0].byCarDesignIdx;
    const tVec3 *pBox = CarBox.hitboxAy[carDesignIdx];
    /* CalcCarSizes() corner layout: [0]=(xLow,yLow,zLow), [1]=(xHigh,yLow,zLow),
     * [3]=(xLow,yHigh,zLow). */
    float xSpan = pBox[1].fX - pBox[0].fX;
    float ySpan = pBox[3].fY - pBox[0].fY;
    float lengthWU = (xSpan > ySpan) ? xSpan : ySpan;

    if (!(lengthWU > 0.0f)) { /* also catches NaN */
        if (!s_warned_degenerate) {
            s_warned_degenerate = true;
            vr_log_line("WARN", "vr_resolve_meters_to_world: car design %d hitbox spans are "
                                "X=%.3f Y=%.3f (expected positive) -- falling back to 100 "
                                "world units/meter", carDesignIdx, (double)xSpan, (double)ySpan);
        }
        return 100.0f; /* Last-resort default, used only for a degenerate hitbox (not the old placeholder). */
    }

    if (lengthWU != s_cached_length_wu) {
        s_cached_length_wu = lengthWU;
        s_cached_scale = lengthWU / VR_REFERENCE_CAR_LENGTH_M;
        vr_log_line("INFO", "vr_resolve_meters_to_world: car design %d hitbox length %.3f world "
                            "units -> %.4f world units/meter (reference car length %.2fm)",
                    carDesignIdx, (double)lengthWU, (double)s_cached_scale,
                    (double)VR_REFERENCE_CAR_LENGTH_M);
    }
    return s_cached_scale;
}

/* The eye projection's far clip plane, in world units (not meters). */
#define VR_EYE_FAR_PLANE_WU 100000.0f

/* Far-plane diag (headset-free pop-in check): for eye 0's fully composed
 * camera, how many track-chunk centres lie in front of the eye but beyond
 * VR_EYE_FAR_PLANE_WU (so the eye's far plane clips them), and the deepest
 * chunk-centre depth. Filled after eye 0's draw_road(), logged with each
 * eye dump and in the periodic frame line. */
static int   s_diag_chunks_in_front = 0;
static int   s_diag_chunks_beyond_far = 0;
static float s_diag_max_chunk_depth = 0.0f;

static void vr_diag_far_plane_chunks(void)
{
    extern float viewx, viewy, viewz;
    extern float vk3, vk6, vk9;
    int in_front = 0, beyond = 0;
    float max_depth = 0.0f;
    for (int c = 0; c < TRAK_LEN && c < MAX_TRACK_CHUNKS; c++) {
        /* Same chunk-centre convention as CalcVisibleTrack() (drawtrk3.c):
         * the world centre is -localdata[c].pointAy[3]. */
        const tVec3 *p = &localdata[c].pointAy[3];
        float depth = (-p->fX - viewx) * vk3 + (-p->fY - viewy) * vk6 + (-p->fZ - viewz) * vk9;
        if (depth > 0.0f) in_front++;
        if (depth > VR_EYE_FAR_PLANE_WU) beyond++;
        if (depth > max_depth) max_depth = depth;
    }
    s_diag_chunks_in_front = in_front;
    s_diag_chunks_beyond_far = beyond;
    s_diag_max_chunk_depth = max_depth;
}

/* Builds the projection-only half of a VR eye's MVP: the asymmetric FOV
 * OpenXR reports for this eye, combined with the camera's already fully
 * composed rotation and world position -- no XrView/pose/
 * quaternion of any kind appears below, only the FOV.
 *
 * The live head pose is composed exactly once, upstream: calculateview()'s
 * post-step (view.c, via vr_camera_compose_apply()) folds it into the camera,
 * so by the time vr_present_stereo_frame() calls this function, base_cam/
 * base_proj already carry the full per-eye camera (rotation AND position).
 * Composing the pose again here would apply the same head pose a second time
 * and double-count the IPD (see tests/vr_camera_compose_test.c, case (c2)).
 * What remains for this function is the asymmetric projection: build_mvp()'s
 * symmetric-FOV construction in scene_render_gpu.c cannot express an HMD's
 * asymmetric per-eye lens offset. */
void build_mvp_vr_eye_projection(float mvp_out[16],
                                  const SceneRenderCamera *base_cam,
                                  const SceneRenderProjection *base_proj,
                                  const XrFovf *fov)
{
    /* vr_camera_view_basis() (vr_camera_compose.h, the module shared with
     * tests/vr_camera_compose_test.c's case (c2)) does the transpose +
     * S-row-flip that turns base_proj->view (the full per-eye composed
     * rotation) into combined_rot, the world-to-eye rotation the asymmetric
     * projection below (mvp[11] = -1, i.e. w = -z_eye) expects, and passes
     * base_cam->viewX/Y/Z through as eye_pos UNCHANGED (base_cam->viewX/Y/Z
     * is already this eye's fully composed world position --
     * vr_camera_compose_apply() wrote it into viewx/viewy/viewz, which this
     * frame's base_cam snapshot was read from in vr_present_stereo_frame()).
     * vr_camera_view_basis() takes no pose/quaternion input at all, so
     * nothing here can double-count the eye's head pose. */
    float combined_rot[3][3];
    float eye_pos[3];
    float base_pos[3] = { base_cam->viewX, base_cam->viewY, base_cam->viewZ };
    vr_camera_view_basis(base_proj->view, base_pos, combined_rot, eye_pos);
    float eye_x = eye_pos[0], eye_y = eye_pos[1], eye_z = eye_pos[2];

    /* Asymmetric perspective from the OpenXR-reported per-eye FOV -- the
     * one place VR genuinely needs a different projection construction
     * than build_mvp()'s symmetric FOV, since HMD lenses are rarely
     * centered. This block depends only on the FOV, never on the pose. */
    float tan_left = tanf(fov->angleLeft);
    float tan_right = tanf(fov->angleRight);
    float tan_up = tanf(fov->angleUp);
    float tan_down = tanf(fov->angleDown);
    float near_plane_m = 0.05f;  /* 5cm, a typical VR near plane in real-world terms */
    float near_plane = near_plane_m * vr_resolve_meters_to_world();  /* world units. For the
                                 stock car this is ~15.6 (the old 100-units/m placeholder gave
                                 5.0), looser than SCENE_GPU_NEAR's 10.0f non-VR default;
                                 may need tuning for comfort on a real headset */
    float far_plane = VR_EYE_FAR_PLANE_WU;

    /* mvp[11] = -1 below (w = -z_eye) assumes a -Z-forward eye space, which is exactly
     * what combined_rot produces (see its derivation above). */
    float mvp[16] = {0};
    mvp[0] = 2.0f / (tan_right - tan_left);
    mvp[8] = (tan_right + tan_left) / (tan_right - tan_left);
    mvp[5] = 2.0f / (tan_up - tan_down);   /* shadercross's single Y-negation still applies downstream --
                                              see build_mvp()'s own comment on this, unchanged here */
    mvp[9] = (tan_up + tan_down) / (tan_up - tan_down);
    mvp[10] = far_plane / (near_plane - far_plane);
    mvp[11] = -1.0f;
    mvp[14] = (far_plane * near_plane) / (near_plane - far_plane);

    /* build_mvp()'s shift_y correction (scene_render_gpu.c) re-centers the horizon to
     * match the legacy SW rasterizer's fixed 199-row convention -- a non-VR-only concern
     * (matching the original game's pixel-exact HUD/horizon placement). VR has no such
     * legacy-parity requirement, and the asymmetric FOV above (tan_up/tan_down from
     * fov) already encodes whatever vertical offset the HMD's own lenses need -- stacking
     * shift_y on top would double-apply a vertical offset. Deliberately not applied here. */

    /* Fold combined_rot + eye-position translation into a view matrix,
     * then multiply by the projection built above -- the same
     * translate-by-(-R*eye) convention build_mvp() uses. combined_rot and
     * eye_x,y,z carry no pose composition of their own. */
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
}

/* --- Per-frame execution (OpenXR bracketing + per-eye present) ---------- */

static XrView       s_frame_views[2];
static bool         s_should_render_this_tick = false;

/* CPU-side frame timing for the periodic diag log (cpuMs=). Set right after
 * xrBeginFrame() succeeds in vr_run_frame(). This is a CPU wall-clock proxy
 * for the game tick plus both eyes' queueing/submission, NOT a GPU timestamp:
 * a real GPU-side measurement would need Vulkan timestamp queries, which are
 * not implemented here. */
static Uint64       s_cpuFrameStartTicks = 0;

/* Filled in by vr_present_stereo_frame() (one entry per eye actually
 * rendered this tick) and consumed by vr_run_frame() when it builds this
 * tick's xrEndFrame composition layer. s_frame_view_count_for_layer is
 * reset to 0 at the top of every vr_run_frame() call, before
 * main_loop_iteration() runs, so a tick where updatescreen()'s VR branch
 * never executes (e.g. still on a non-racing frontend state) correctly
 * submits layerCount = 0 rather than stale data from a previous tick. */
static Uint32 s_frame_view_count_for_layer = 0;
static XrCompositionLayerProjectionView s_projection_views[2];

static Uint32 view_configs_width(Uint32 i)
{
    return s_view_configs[i].recommendedImageRectWidth;
}

static Uint32 view_configs_height(Uint32 i)
{
    return s_view_configs[i].recommendedImageRectHeight;
}

bool vr_run_frame(void)
{
    if (s_instance != XR_NULL_HANDLE && vr_poll_events())
        vr_forward_quit_to_game();

    if (s_quit_forwarded) {
        /* No more OpenXR frames. Run the game's own shutdown; when its state machine
         * reaches frontend_shutdown_finish() it calls vr_shutdown(), which ends the
         * session (if the runtime has not already) and destroys the instance. */
        s_should_render_this_tick = false;
        s_frame_view_count_for_layer = 0;
        bool keep_running = main_loop_iteration() != 0;
        return keep_running && s_instance != XR_NULL_HANDLE;
    }

    if (!vr_fsm_frames_allowed(&s_fsm)) {
        SDL_Delay(10);
        return true; /* keep looping, waiting for READY */
    }

    XrResult frame_result;
    XrFrameWaitInfo frame_wait_info = { .type = XR_TYPE_FRAME_WAIT_INFO };
    s_frame_state = (XrFrameState){ .type = XR_TYPE_FRAME_STATE };
    frame_result = xrWaitFrame(s_session, &frame_wait_info, &s_frame_state);
    if (XR_FAILED(frame_result)) {
        vr_frame_call_failed("xrWaitFrame", frame_result);
        return true; /* the next call runs the game's shutdown */
    }

    XrFrameBeginInfo frame_begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    frame_result = xrBeginFrame(s_session, &frame_begin_info);
    if (XR_FAILED(frame_result)) {
        vr_frame_call_failed("xrBeginFrame", frame_result);
        return true;
    }
    /* Marks this frame as open for
     * vr_shutdown() -- see its own comment and s_frame_open's declaration
     * above. Cleared below once this function's own xrEndFrame() runs (or
     * by vr_shutdown() itself, if main_loop_iteration() below reaches it
     * first). */
    s_frame_open = true;
    s_cpuFrameStartTicks = SDL_GetPerformanceCounter();

    s_should_render_this_tick = s_frame_state.shouldRender;
    if (s_should_render_this_tick) {
        if (!vr_locate_views(s_frame_state.predictedDisplayTime, s_frame_views)) {
            s_should_render_this_tick = false; /* no pose at all yet (first frame edge case) --
                                                   skip this tick's render, still end the frame below */
        }
    }

    /* Reset before main_loop_iteration() runs -- see the struct comment on
     * s_frame_view_count_for_layer above for why this must happen every
     * tick regardless of whether updatescreen()'s VR branch runs. */
    s_frame_view_count_for_layer = 0;

    /* main_loop_iteration() runs the real game tick (input, physics,
     * frontend state machine). When it reaches race_draw()->updatescreen(),
     * updatescreen()'s g_bVRMode branch calls
     * vr_present_stereo_frame() synchronously, right here in the middle of
     * this function call -- s_frame_views/s_should_render_this_tick above
     * are what it reads, and it fills s_projection_views/
     * s_frame_view_count_for_layer for the xrEndFrame call below. */
    /* --vr-freeze-from F: from presented frame F on, block game ticks (the
     * same block SHIFT-freeze uses) so the world stands still and only the
     * head pose changes between frames -- adjacent eye dumps then differ
     * only by what the head motion reveals or hides. */
    if (g_iVRFreezeFrom >= 0 && s_vr_frames_presented >= g_iVRFreezeFrom)
        g_bVRTestFreeze = true;
    bool keep_running = main_loop_iteration() != 0;
    /* End of the CPU-side frame window for the diag log's cpuMs= figure.
     * Taken here, before xrEndFrame(), so compositor wait time is excluded. */
    Uint64 cpu_end_ticks = SDL_GetPerformanceCounter();

    /* main_loop_iteration() above can reach
     * frontend_shutdown_finish() -> vr_shutdown() on this exact tick (the
     * game-initiated quit path -- see vr_shutdown()'s own comment), which
     * destroys and NULLs s_session/s_instance (and already closed out THIS
     * frame itself with its own empty-layer xrEndFrame(), since s_frame_open
     * was still true when it ran) before returning back up to here. If that
     * happened, every OpenXR object below is already gone -- calling
     * xrEndFrame() against a destroyed session is undefined behavior per
     * the OpenXR spec (no calls are permitted on a destroyed handle) -- so
     * detect it and bail immediately without touching any OpenXR handle. */
    if (s_session == XR_NULL_HANDLE) {
        return false;
    }

    XrCompositionLayerProjection projection_layer = {
        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
        .space = s_local_space,
        .viewCount = s_frame_view_count_for_layer,   /* 0 if updatescreen()'s VR branch never ran this tick */
        .views = s_projection_views,
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
    XrResult end_result = xrEndFrame(s_session, &frame_end_info);
    s_frame_open = false;
    if (XR_FAILED(end_result)) {
        vr_frame_call_failed("xrEndFrame", end_result);
        return true; /* the next call runs the game's shutdown */
    }

    if (s_frame_view_count_for_layer > 0) {
        s_vr_frames_presented++;
        if (s_vr_frames_presented == 1 || s_vr_frames_presented % 60 == 0) {
            /* Draw counts are the last eye's queued/merged totals for this
             * tick (scene_render_gpu's repack leaves them valid until the next
             * begin_frame). cpuMs covers the game tick and both eyes' CPU work,
             * not GPU execution time. */
            int drawCmdCount = 0, vertexCount = 0;
            game_render_get_draw_stats(g_pGameRenderer, &drawCmdCount, &vertexCount);
            double cpuFrameMs = (double)(cpu_end_ticks - s_cpuFrameStartTicks) * 1000.0
                              / (double)SDL_GetPerformanceFrequency();
            vr_log_line("INFO", "frame %d submitted (views=%u layerCount=1) "
                                "view[0] pos=(%.3f,%.3f,%.3f) ori=(%.3f,%.3f,%.3f,%.3f) "
                                "chunks=%d cars=%d buildings=%d drawCmds=%d vertices=%d cpuMs=%.2f "
                                "chunksBeyondFar=%d/%d maxChunkDepth=%.0f",
                        s_vr_frames_presented, (unsigned)s_frame_view_count_for_layer,
                        s_frame_views[0].pose.position.x, s_frame_views[0].pose.position.y,
                        s_frame_views[0].pose.position.z,
                        s_frame_views[0].pose.orientation.x, s_frame_views[0].pose.orientation.y,
                        s_frame_views[0].pose.orientation.z, s_frame_views[0].pose.orientation.w,
                        TrackSize, numcars, NumVisibleBuildings, drawCmdCount, vertexCount, cpuFrameMs,
                        s_diag_chunks_beyond_far, s_diag_chunks_in_front, (double)s_diag_max_chunk_depth);
        }
        /* --vr-frames N: after N frames actually reached the compositor, ask
         * the game to quit through its own shutdown path (same flags the
         * headless snapshot mode uses), so frontend_shutdown_finish() ->
         * vr_shutdown() runs exactly as for a user-initiated quit. */
        if (g_iVRMaxFrames > 0 && s_vr_frames_presented >= g_iVRMaxFrames && !quit_game) {
            vr_log_line("INFO", "--vr-frames %d reached, requesting quit", g_iVRMaxFrames);
            quit_game = 1;
            racing = 0;
        }
        /* Test hook (ROLLER_VR_TEST_RUNTIME_EXIT_AFTER=N): ask the RUNTIME to end the
         * session after N presented frames, without going through the game's quit
         * path. The runtime then reports STOPPING exactly as it does when the user
         * quits from SteamVR, exercising the runtime-initiated shutdown path. */
        if (s_test_runtime_exit_after > 0 && s_vr_frames_presented == s_test_runtime_exit_after) {
            XrResult exit_result = xrRequestExitSession(s_session);
            vr_log_line("INFO", "test hook: xrRequestExitSession after %d frames -> XrResult=%d",
                        s_vr_frames_presented, (int)exit_result);
        }
    } else {
        s_vr_empty_frames++;
        if (s_vr_empty_frames <= 10)
            vr_log_line("INFO", "tick submitted with layerCount=0 (empty #%d, after %d presented frames, "
                                "shouldRender=%d, frontend state=%d, racing=%d)",
                        s_vr_empty_frames, s_vr_frames_presented, (int)s_frame_state.shouldRender,
                        (int)eFrontendCurrentState, (int)racing);
    }

    return keep_running;
}

void vr_present_stereo_frame(uint8 *pMainScrPtr, unsigned int uiViewType, int iDriveView)
{
    if (!s_should_render_this_tick) {
        /* The caller (updatescreen()'s g_bVRMode branch) already called
         * game_render_begin_frame() once for this tick before reaching this
         * function -- see this function's own calling-convention doc
         * comment in vr_runtime.h. That acquires an SDL GPU command buffer
         * that nothing else will ever submit or cancel if we just bail out
         * here, leaking one SDL command buffer on every tick OpenXR reports
         * shouldRender == false. Cancel it
         * explicitly instead of leaving it dangling. */
        game_render_cancel_frame(g_pGameRenderer);
        s_frame_view_count_for_layer = 0;
        return;
    }

    /* Same view-state globals draw_road() itself reads to build its
     * GameRenderCamera/GameRenderProjection (3d.c draw_road(), just after
     * its own calculateview() call -- see the long comment below on why
     * this function calls calculateview() directly instead of reading them
     * back via game_render_get_camera()/get_projection()). */
    extern float viewx, viewy, viewz;
    extern float vk1, vk2, vk3, vk4, vk5, vk6, vk7, vk8, vk9;

    for (Uint32 i = 0; i < s_view_count && i < 2; i++) {
        /* Required before every eye EXCEPT the first: scene_render_gpu_end_
         * frame_internal()'s very first line is `if (!r || !r->cmdBuf) return
         * false;`, and r->cmdBuf is set back to NULL by the previous eye's own
         * end_frame_to_vr_eye() call right after it submits (scene_render_gpu.c's
         * end_frame_internal, near its end). The ONLY place that re-acquires it
         * (SDL_AcquireGPUCommandBuffer) is scene_render_gpu_begin_frame(), which
         * also resets vertexCount/drawCmdCount/carDrawCount/etc back to 0.
         * Without calling it again here for the second eye, its draw_road()
         * calls would silently append onto whatever the first eye already
         * queued and submitted (instead of starting from a clean, empty draw
         * list), AND its end_frame_to_vr_eye() call would hard-fail immediately
         * on the NULL-cmdBuf check above -- i.e. the second eye would never
         * render at all, every single frame.
         *
         * i == 0 deliberately does NOT call this: per this function's own
         * documented calling contract (see vr_runtime.h), the caller
         * (updatescreen()'s g_bVRMode branch) is expected to have already
         * called game_render_begin_frame() once for this tick before reaching
         * this function -- exactly the same single call the non-VR path
         * already makes before its own single draw_road()+end_frame() pair.
         * Calling it again here for eye 0 too would silently discard that
         * already-acquired (but not yet submitted) command buffer by
         * unconditionally re-acquiring a new one over it. */
        if (i > 0)
            game_render_begin_frame(g_pGameRenderer);

        /* The VR-eye override must be set BEFORE draw_road(), not after it.
         * game_render_hw_draw_car() bakes scene_render_gpu_build_vp()'s
         * result into each car's draw command AT QUEUE TIME, i.e. during
         * draw_road()'s own execution, while world geometry only reads it
         * back at END-FRAME time (Pass A, scene_render_gpu_end_frame_
         * internal). An override set after draw_road() returns would leave
         * every car queued with the flat, non-VR camera, so only the world
         * geometry would get the correct eye view. The override also has to
         * persist until replaced rather than being one-shot; both
         * requirements hold together (see scene_render_gpu_build_vp()'s own
         * comment).
         *
         * calculateview()'s args here exactly match what draw_road() itself
         * passes it: draw_road() calls calculateview(uiViewMode, iCarIdx,
         * iChaseCamIdx) using ITS OWN parameter names (3d.c draw_road(),
         * right after its own signature `draw_road(uint8 *pScrPtr, int
         * iCarIdx, unsigned int uiViewMode, int iCopyImmediately, int
         * iChaseCamIdx)`). The draw_road() call below passes (pMainScrPtr,
         * uiViewType, iDriveView, -1, 0), so within THAT call draw_road's
         * own iCarIdx == uiViewType and its own uiViewMode == iDriveView --
         * hence calculateview(iDriveView, uiViewType, 0) here, using this
         * function's own parameter names. calculateview() writes only
         * view-state globals (viewx/viewy/viewz, vk1..vk9, etc.) and has no
         * other side effect. The cockpit and driver views are a pure function
         * of the car state, so the two calls produce identical globals in
         * those views. The tower and interpolated views read the previous
         * frame's position (view.c), so the eye's camera can differ slightly
         * between the two calls in those modes.
         *
         * vr_camera_compose_set_active_eye()/_clear_active_eye() bracket both
         * this call and draw_road()'s own internal calculateview(). Each call
         * composes this eye's head pose into vk1..vk9/viewx,y,z, so both see
         * the head-tracked camera. */
        vr_camera_compose_set_active_eye(
            s_frame_views[i].pose.orientation.x, s_frame_views[i].pose.orientation.y,
            s_frame_views[i].pose.orientation.z, s_frame_views[i].pose.orientation.w,
            s_frame_views[i].pose.position.x, s_frame_views[i].pose.position.y,
            s_frame_views[i].pose.position.z,
            vr_resolve_meters_to_world());
        calculateview(iDriveView, uiViewType, 0);
        /* draw_road()'s own internal calculateview() call (3d.c draw_road())
         * is immediately followed by
         * noclip_camera_apply()/chase_look_apply() -- debug-only "Free
         * Camera"/noclip overrides that also write the same view-state
         * globals. Calling them here too keeps base_cam/base_proj below
         * (read from those globals BEFORE draw_road() runs its own,
         * redundant calculateview()+override sequence) consistent with what
         * draw_road() itself will use a few lines down for world-geometry
         * queueing -- both no-arg, side-effect-only-on-those-globals
         * functions (view.h), so calling them an extra time here is cheap
         * and has no effect beyond keeping this eye's MVP override in sync
         * with normal play. Only affects debug noclip/Free Camera use. */
        noclip_camera_apply();
        chase_look_apply();

        /* Read the same globals draw_road() itself uses to build its
         * GameRenderCamera/GameRenderProjection (3d.c draw_road(), just
         * after its own calculateview() call) -- NOT game_render_get_
         * camera()/get_projection(), which read back g_pGameRenderer's
         * CACHED copy of those structs and would still hold the PREVIOUS
         * eye's (or previous frame's) values until draw_road() itself runs
         * a few lines from now and refreshes them. By this point they
         * already carry THIS eye's fully composed camera, thanks
         * to the calculateview() call above running while this eye's pose
         * was active. build_mvp_vr_eye_projection() only reads
         * base_cam->viewX/viewY/viewZ and base_proj->view, so only those
         * fields are populated here (the rest zero-init and are unused by
         * it). */
        SceneRenderCamera base_cam = {
            .viewX = viewx, .viewY = viewy, .viewZ = viewz,
        };
        SceneRenderProjection base_proj = {
            .view = {{vk1, vk2, vk3}, {vk4, vk5, vk6}, {vk7, vk8, vk9}},
        };

        float eye_mvp[16];
        build_mvp_vr_eye_projection(eye_mvp, &base_cam, &base_proj, &s_frame_views[i].fov);
        game_render_set_vr_eye_mvp(g_pGameRenderer, eye_mvp);

        /* The sky/ground split for this eye's own FOV (see vr_eye_sky_halfplane()),
         * consumed by game_render_draw_sky() inside draw_road() below. */
        {
            float sky_a, sky_b, sky_c;
            vr_eye_sky_halfplane(base_proj.view,
                                 tanf(s_frame_views[i].fov.angleLeft), tanf(s_frame_views[i].fov.angleRight),
                                 tanf(s_frame_views[i].fov.angleUp), tanf(s_frame_views[i].fov.angleDown),
                                 &sky_a, &sky_b, &sky_c);
            game_render_set_vr_sky_halfplane(sky_a, sky_b, sky_c);
        }

        /* draw_road() recomputes calculateview()/calculatetransform() fresh
         * (redundant but harmless, see above) and queues this eye's draw
         * list into g_pGameRenderer's single windowless scene instance --
         * there are two draw_road() calls here (one per eye), not two
         * renderer instances. The VR-eye override set above is now active
         * BEFORE any of this call's cars are queued. */
        draw_road(pMainScrPtr, uiViewType, iDriveView, -1, 0);
        if (i == 0)
            vr_diag_far_plane_chunks(); /* camera globals still hold eye 0's composed camera */
        vr_camera_compose_clear_active_eye();
        game_render_clear_vr_sky_halfplane();

        Uint32 image_index = 0;
        XrSwapchainImageAcquireInfo acquire_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (XR_FAILED(xrAcquireSwapchainImage(s_swapchains[i], &acquire_info, &image_index))) {
            /* draw_road() above already queued this eye's cars/world
             * geometry into a command buffer that game_render_end_frame_to_
             * vr_eye() will now never be called to submit -- cancel it
             * explicitly rather than leaking it.
             * No swapchain image was acquired yet, so nothing to release. */
            game_render_cancel_frame(g_pGameRenderer);
            return;
        }
        XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
        if (XR_FAILED(xrWaitSwapchainImage(s_swapchains[i], &wait_info))) {
            /* Same pending-command-buffer leak as above. The matching release
             * call is still attempted to keep xrAcquireSwapchainImage/
             * xrWaitSwapchainImage/xrReleaseSwapchainImage's strict ordering
             * contract intact for whatever comes next on this swapchain.
             * Per the spec, xrReleaseSwapchainImage is only valid after a
             * successful xrWaitSwapchainImage. Since the wait above failed, no
             * image was waited on, so this call is expected to fail itself
             * (XR_ERROR_CALL_ORDER_INVALID) and do nothing. It is a
             * best-effort attempt to stop the sequence desyncing further, not
             * a recovery. The result is logged rather than assumed, so a
             * genuinely different failure here isn't silently swallowed. */
            game_render_cancel_frame(g_pGameRenderer);
            XrSwapchainImageReleaseInfo release_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            XrResult release_result = xrReleaseSwapchainImage(s_swapchains[i], &release_info);
            if (XR_FAILED(release_result)) {
                vr_log_line("WARN", "xrReleaseSwapchainImage after a failed xrWaitSwapchainImage "
                                    "also failed as expected (XrResult=%d)", (int)release_result);
            }
            return;
        }

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
        /* Fetched unconditionally, regardless of whether `resolved` came back
         * NULL -- scene_render_gpu's lastVrFence handoff is one-shot and
         * leaks (never SDL_ReleaseGPUFence'd) if the getter isn't called
         * every single time a render happens, even on a failure path (see
         * scene_render_gpu_get_last_fence's doc comment). In the current
         * implementation resolved/fence always rise and fall together (both
         * NULL or both set -- scene_render_gpu_end_frame_internal only hands
         * back a fence when submission itself succeeded, which is also what
         * makes the offscreen texture valid), but consuming+releasing the
         * fence here unconditionally, before any early return, costs nothing
         * and closes off that leak path even if that invariant ever changes. */
        SDL_GPUFence *fence = game_render_get_last_vr_fence(g_pGameRenderer);
        if (fence) {
            /* Fence-wait serialization: simple and correct. Semaphore-based
             * overlap between the GPU and the copy is a possible later
             * optimisation, not done here. */
            SDL_WaitForGPUFences(s_gpu, true, &fence, 1);
            SDL_ReleaseGPUFence(s_gpu, fence);
        }
        /* game_render_end_frame_to_vr_eye() (-> scene_render_gpu_end_frame_
         * internal) always either submits or internally cancels the pending
         * command buffer itself before returning, success or failure -- no
         * separate game_render_cancel_frame() call is needed here regardless
         * of what `resolved` comes back as. Only the swapchain image
         * acquired above still needs releasing on this failure path. */
        if (!resolved) {
            XrSwapchainImageReleaseInfo release_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(s_swapchains[i], &release_info);
            return;
        }

        SDL_PropertiesID tex_props = SDL_GetGPUTextureProperties(resolved);
        VkImage resolved_vk_image = (VkImage)SDL_GetPointerProperty(
            tex_props, SDL_PROP_GPU_TEXTURE_VULKAN_IMAGE_POINTER, NULL);
        if (!resolved_vk_image) {
            vr_log_line("ERROR", "SDL_GetGPUTextureProperties did not expose a VkImage "
                                 "for the resolved eye texture (is this the patched SDL fork?)");
            XrSwapchainImageReleaseInfo release_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(s_swapchains[i], &release_info);
            return;
        }

        /* --vr-eye-dump-dir test hook: in-process readback of this eye's
         * resolved texture. Placed AFTER the fence wait above (the GPU must
         * have finished producing `resolved`) and BEFORE the vkCmdCopyImage
         * below. The readback submits its own command buffer and never
         * touches the swapchain copy, so the image copied into OpenXR is
         * unchanged. Every 90 presented frames by default (with --vr-fake-head that
         * is yaw 0/90/180/270); --vr-eye-dump-every/--vr-eye-dump-from narrow it
         * to a consecutive window for frame-to-frame pop-in comparisons. */
        if (g_szVREyeDumpDir[0] && s_vr_frames_presented >= g_iVREyeDumpFrom
            && ((s_vr_frames_presented - g_iVREyeDumpFrom) % g_iVREyeDumpEvery) == 0) {
            char dump_path[320];
            SDL_snprintf(dump_path, sizeof(dump_path), "%s/eye%u_frame%d.png",
                         g_szVREyeDumpDir, (unsigned)i, s_vr_frames_presented);
            if (game_render_dump_vr_eye_png(g_pGameRenderer, resolved, eye_w, eye_h, dump_path))
                vr_log_line("INFO", "eye PNG dump: %s (track chunks in front=%d beyond far plane %.0f=%d, max chunk depth %.0f)",
                            dump_path, s_diag_chunks_in_front, (double)VR_EYE_FAR_PLANE_WU,
                            s_diag_chunks_beyond_far, (double)s_diag_max_chunk_depth);
            else
                vr_log_line("WARN", "eye PNG dump failed for %s", dump_path);
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
        /* Pre-copy layout of the resolved eye texture: NOT VK_IMAGE_LAYOUT_GENERAL
         * (an earlier, unverified assumption) -- checked directly against
         * the patched SDL fork's Vulkan backend (/root/sdl-vr-fork/src/gpu/vulkan/
         * SDL_gpu_vulkan.c) rather than left as a guess:
         *
         *   - ensure_offscreen_texture() (scene_render_gpu.c) creates offscreenTex
         *     with usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER.
         *   - SDL_gpu_vulkan.c's VULKAN_INTERNAL_DefaultTextureUsageMode() checks
         *     the SAMPLER bit FIRST (comment: "NOTE: order matters here!"), so this
         *     texture's "default" usage mode is VULKAN_TEXTURE_USAGE_MODE_SAMPLER,
         *     whose layout (VULKAN_INTERNAL_TextureSubresourceMemoryBarrier's
         *     destinationUsageMode switch) is VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
         *   - VULKAN_EndRenderPass() unconditionally transitions every color/resolve
         *     attachment subresource back to its texture's default usage mode when
         *     the render pass ends (comment in SDL_gpu_vulkan.c: "When binding a
         *     texture as a render pass attachment, we transition the resource on
         *     BeginRenderPass and transition it back to its default on EndRenderPass").
         *   - scene_render_gpu.c's end_frame_internal() uses offscreenTex as the
         *     resolve/color target for BOTH Pass 1 (3D scene) and Pass 2 (HUD
         *     overlay, colorInfo.texture = resolveTarget again) -- each SDL_EndGPU-
         *     RenderPass call re-applies the same back-to-default transition, and
         *     neither pass sets colorInfo.cycle (so this is also the confirmation
         *     the cycle:false requirement needs -- offscreenTex is genuinely
         *     never cycled, only resized via a full SDL_CreateGPUTexture when w/h
         *     changes between eyes, which is fine since we re-query the VkImage
         *     pointer fresh every eye anyway).
         *   - For the windowless VR path (bReadback=false, r->window=NULL), no
         *     further pass touches offscreenTex after Pass 2, so by the time our
         *     fence (submitted at the very end of end_frame_internal) signals, the
         *     texture's real hardware layout is exactly VK_IMAGE_LAYOUT_SHADER_
         *     READ_ONLY_OPTIMAL -- confirmed against the driver code actually
         *     doing the transition, not inferred from usage-flag conventions alone.
         *
         * Consequence for the barrier below: oldLayout must be SHADER_READ_ONLY_
         * OPTIMAL, not GENERAL or UNDEFINED, or this barrier itself is invalid
         * (wrong oldLayout = image content undefined per the Vulkan spec, and a
         * hard validation error under XR_APILAYER_LUNARG_core_validation).
         *
         * Equally important: this function must transition the image back to VK_IMAGE_LAYOUT_SHADER_
         * READ_ONLY_OPTIMAL after the copy (see src_back_to_sampler below), because
         * SDL_GPU's own barrier logic (VULKAN_INTERNAL_PrepareTextureSubresourceFor
         * Write, comment: "always do barrier because of layout transitions") does
         * NOT track the texture's real current layout across frames -- it always
         * computes next frame's Pass-1 barrier assuming the texture is still sitting
         * in its default (SAMPLER) layout. Leaving it in TRANSFER_SRC_OPTIMAL after
         * our copy would silently desync SDL_GPU's internal bookkeeping from the
         * texture's real Vulkan state and corrupt the very next frame's render pass
         * for this same eye. */
        VkImageMemoryBarrier src_to_transfer_src = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = resolved_vk_image, .subresourceRange = subresource_range,
        };
        VkImageMemoryBarrier dst_to_transfer_dst = src_to_transfer_src;
        dst_to_transfer_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        dst_to_transfer_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dst_to_transfer_dst.image = target_image;
        /* Access masks (added after the first hardware run under
         * VK_LAYER_KHRONOS_validation's synchronization validation, which
         * reported READ_AFTER_WRITE / WRITE_AFTER_WRITE hazards for the
         * original all-zero masks): a layout transition counts as a write, so
         * the copy's TRANSFER_READ (source) / TRANSFER_WRITE (destination)
         * must be in the pre-barrier's dstAccessMask, and the post-copy
         * transitions must have the copy's accesses as srcAccessMask. */
        src_to_transfer_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        dst_to_transfer_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        VkImageMemoryBarrier pre_barriers[2] = { src_to_transfer_src, dst_to_transfer_dst };
        /* TOP_OF_PIPE_BIT as srcStage (no explicit access masks) is deliberate:
         * real synchronization here is provided externally by the SDL_WaitForGPUFences call above (we already
         * know the GPU has finished writing resolved_vk_image before this command
         * buffer is even recorded), so the barrier only needs to be spec-valid for
         * the LAYOUT transition, not for ordering a real GPU-side dependency. */
        vkCmdPipelineBarrier(s_command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, pre_barriers);

        VkImageCopy copy_region = {
            .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
            .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
            .extent = { eye_w, eye_h, 1 },
        };
        vkCmdCopyImage(s_command_buffer, resolved_vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

        /* Post-copy: restore the resolved eye texture to the layout SDL_GPU's own
         * bookkeeping assumes it's sitting in (see the long comment above), and
         * hand the swapchain image back in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL.
         * The OpenXR spec (XR_KHR_vulkan_enable/enable2, "Swapchain Image Layout")
         * says that on xrReleaseSwapchainImage the runtime interprets the image as
         * "Having a memory layout compatible with VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_
         * OPTIMAL for color images" and that "The application is responsible for
         * transitioning the swapchain image back to the image layout ... that the
         * OpenXR runtime requires". GENERAL would only work by luck: Monado's
         * release barrier assumes COLOR_ATTACHMENT_OPTIMAL. Colour-attachment access masks pair with the
         * COLOR_ATTACHMENT_OUTPUT dst stage below. */
        VkImageMemoryBarrier src_back_to_sampler = src_to_transfer_src;
        src_back_to_sampler.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        src_back_to_sampler.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        src_back_to_sampler.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        src_back_to_sampler.dstAccessMask = 0;
        VkImageMemoryBarrier dst_to_color_attachment = dst_to_transfer_dst;
        dst_to_color_attachment.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dst_to_color_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        dst_to_color_attachment.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dst_to_color_attachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkImageMemoryBarrier post_barriers[2] = { src_back_to_sampler, dst_to_color_attachment };
        /* COLOR_ATTACHMENT_OUTPUT as dstStage (matches the swapchain image's
         * colour-attachment dst access; the resolved-texture barrier has dst access
         * 0, valid at any stage). Same externally-synchronized reasoning as the
         * pre-copy barrier above: the
         * next real GPU usage of either image happens in a later, independently
         * submitted/fenced command buffer (next frame's SDL_GPU render pass for
         * resolved_vk_image, the OpenXR runtime's own compositor for target_image),
         * not anything ordered against this command buffer directly. */
        vkCmdPipelineBarrier(s_command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 2, post_barriers);
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
