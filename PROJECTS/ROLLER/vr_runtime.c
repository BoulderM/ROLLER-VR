/* PROJECTS/ROLLER/vr_runtime.c
 *
 * VR Phase 2: opens the OpenXR session against SDL_GPU's Vulkan device
 * (unchanged from Phase 1) and drives the real game loop in stereo. vr_init()
 * is Phase 1's main()'s setup portion (steps 1-8: device, instance, system,
 * session, swapchains, reference space, command pool). Phase 1's own
 * 120-frame clear-color loop and shutdown are replaced by vr_run_frame()/
 * vr_present_stereo_frame() (this task's sibling tasks) and vr_shutdown().
 */
#include "vr_runtime.h"
#include "vr_view_merge.h"
#include "vr_ext_list.h"
#include "vr_log.h"
#include "vr_session_fsm.h"

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
/* task-7-round2 New Important 1: true from the moment vr_run_frame()'s own
 * xrBeginFrame() succeeds until its own xrEndFrame() call (success or
 * failure) or vr_shutdown()'s frame-close-out below runs. Lets vr_shutdown()
 * tell apart the two shutdown paths that reach it (see vr_shutdown()'s own
 * comment): called mid-frame, from inside vr_run_frame() via
 * main_loop_iteration()->frontend_shutdown_finish() (game-initiated quit),
 * vs. called at the top level after vr_run_frame()'s own event loop already
 * saw XR_SESSION_STATE_STOPPING and returned (runtime-initiated shutdown,
 * no frame outstanding). */
static bool s_frame_open = false;
/* Moved up from its original spot in the "Per-frame execution" section below
 * (task-7-round2) so vr_shutdown() -- defined above that section -- can read
 * s_frame_state.predictedDisplayTime when closing out a still-open frame on
 * the game-initiated quit path (see vr_shutdown()'s own comment). Still set
 * fresh by vr_run_frame() every tick via xrWaitFrame(), same as before. */
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

/* --- Vulkan interop negotiation (Phase 3 M1) ---------------------------- */

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
    instance_create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
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
    /* task-7-review Finding 1 (Critical): this used to call its own
     * SDL_CreateGPUDevice() here, creating a SECOND, independent VkDevice
     * from the one InitSDL()'s VR branch (roller.c) already created for the
     * game itself (s_pGPUDevice / ROLLERGetGPUDevice()). OpenXR was then
     * bound to THIS runtime's own device while vr_present_stereo_frame()
     * went on to wait fences and copy VkImages that actually belonged to
     * the GAME's device (via game_render_end_frame_to_vr_eye()) using a
     * queue/command buffer from a DIFFERENT device -- invalid Vulkan usage
     * (a VkImage/fence from one VkDevice cannot be used in commands
     * submitted to a different VkDevice's queue). Fixed by adopting the
     * game's own device instead of creating a second one: OpenXR now binds
     * to the exact same VkDevice/VkQueue the game renders with, and every
     * downstream Vulkan call in this file operates on that one shared
     * device throughout.
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

    /* Log what the runtime offers: the eye swapchains below are created as
     * VK_FORMAT_R8G8B8A8_UNORM, which a runtime may not list (SteamVR prefers sRGB). */
    uint32_t format_count = 0;
    if (XR_SUCCEEDED(xrEnumerateSwapchainFormats(s_session, 0, &format_count, NULL)) && format_count > 0) {
        int64_t *formats = calloc(format_count, sizeof(int64_t));
        if (formats && XR_SUCCEEDED(xrEnumerateSwapchainFormats(s_session, format_count, &format_count, formats))) {
            char list[512] = "";
            bool has_unorm = false;
            for (uint32_t i = 0; i < format_count; i++) {
                char one[24];
                snprintf(one, sizeof(one), "%s%lld", i ? " " : "", (long long)formats[i]);
                if (strlen(list) + strlen(one) < sizeof(list))
                    strcat(list, one);
                if (formats[i] == (int64_t)VK_FORMAT_R8G8B8A8_UNORM)
                    has_unorm = true;
            }
            vr_log_line("INFO", "runtime swapchain formats (VkFormat values): %s", list);
            if (!has_unorm)
                vr_log_line("WARN", "the runtime does not list VK_FORMAT_R8G8B8A8_UNORM (%d), which the eye "
                                    "swapchains use; xrCreateSwapchain is likely to fail",
                            (int)VK_FORMAT_R8G8B8A8_UNORM);
        }
        free(formats);
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
    VkFormat swapchain_format = VK_FORMAT_R8G8B8A8_UNORM;
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


/* --- Session lifecycle (Phase 3 M1) -------------------------------------- */

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
            /* Per the M1 spec, instance loss takes the same non-zero-exit path as a
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

/* --- Live head tracking + per-eye MVP construction (Phase 2) ------------ */

/* Last successfully merged views (each component is its most recent valid
 * value, or the identity/zero fallback if it never was valid). */
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

/* Standard quaternion-to-rotation-matrix conversion (m[row][col], naive
 * array read = standard math convention). For a quaternion q that rotates
 * "local" space into some base/reference space -- which is exactly what
 * OpenXR's XrPosef.orientation is defined to do (XrView.pose is relative to
 * the space passed to xrLocateViews, here s_local_space) -- the resulting
 * matrix m is the LOCAL-TO-BASE orientation matrix: column j of m (varying
 * row, fixed col) is local axis j expressed in base-space coordinates. */
static void quat_to_mat3(const XrQuaternionf *q, float m[3][3])
{
    float x = q->x, y = q->y, z = q->z, w = q->w;
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] =     2 * (x * y - z * w); m[0][2] =     2 * (x * z + y * w);
    m[1][0] =     2 * (x * y + z * w); m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] =     2 * (y * z - x * w);
    m[2][0] =     2 * (x * z - y * w); m[2][1] =     2 * (y * z + x * w); m[2][2] = 1 - 2 * (x * x + y * y);
}

/* --- 3x3 matrix helpers for the head-pose composition below --------------
 * (naive [row][col] array reads throughout, matching quat_to_mat3's own
 * convention -- these are NOT aware of proj->view's [col][row] storage
 * quirk; that translation happens once, explicitly, where base_rot is
 * read out of base_proj->view below). */
static void mat3_transpose(const float src[3][3], float dst[3][3])
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            dst[r][c] = src[c][r];
}

static void mat3_mul(const float a[3][3], const float b[3][3], float out[3][3])
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            float sum = 0.0f;
            for (int k = 0; k < 3; k++) sum += a[r][k] * b[k][c];
            out[r][c] = sum;
        }
}

/* Left-multiplies by S = diag(1, 1, -1) -- negates row 2 only, leaves rows
 * 0/1 unchanged. S is its own inverse (S*S = I). It converts between
 * OpenXR's local/reference-space axis convention (+X right, +Y up, -Z
 * forward: a right-handed frame, matching the eye/view-space convention the
 * kept projection matrix below expects) and the game's own convention
 * (+X right, +Y up, +Z forward: a left-handed frame; R_true/base_rot,
 * vk1..vk9). The two frames share X/Y and differ only in the sign of Z, so
 * a single-axis flip is the full, exact conversion between them. Because
 * det(S) = -1 it is a reflection (handedness change), not a rotation --
 * which is exactly why it is applied explicitly instead of being folded
 * into a rotation matrix. */
static void mat3_flip_row2(const float src[3][3], float dst[3][3])
{
    dst[0][0] = src[0][0]; dst[0][1] = src[0][1]; dst[0][2] = src[0][2];
    dst[1][0] = src[1][0]; dst[1][1] = src[1][1]; dst[1][2] = src[1][2];
    dst[2][0] = -src[2][0]; dst[2][1] = -src[2][1]; dst[2][2] = -src[2][2];
}

/* OpenXR reports positions in meters; ROLLER's world units are a much
 * smaller, unlabeled scale (SCENE_GPU_NEAR/FAR in scene_render_gpu.h are
 * 10.0f/20000000.0f world units, and menu_render_gpu.c's track-overview
 * camera sits "hundreds of thousands of world units" from the track --
 * both say world units are far finer-grained than a meter). No numeric
 * world-unit-per-meter constant exists anywhere else in this codebase
 * (checked for CAR_WIDTH/TRACK_WIDTH/scale-factor style constants; the
 * only related quantities, e.g. fTrackHalfWidth, are per-track data, not
 * fixed constants) to anchor a derived value against. This is therefore a
 * PLACEHOLDER -- pick something in the right ballpark (1 world unit = 1cm
 * is a common engine convention) and revisit once there's an actual
 * rendered VR scene to eyeball against a known real-world reference (e.g.
 * seated eye height in the cockpit). Applied to every meter-scale quantity
 * below (head position delta, near plane) before it's combined
 * with world-unit camera state. */
#define VR_METERS_TO_WORLD 100.0f

void build_mvp_vr_eye(float mvp_out[16],
                       const SceneRenderCamera *base_cam,
                       const SceneRenderProjection *base_proj,
                       const XrView *xr_view,
                       int vpW, int vpH)
{
    /* base_rot is a raw copy of base_proj->view -- the vk1..vk9 basis
     * calculateview()/calculatetransform() computed this frame, unchanged
     * from the existing non-VR camera. scene_render_gpu.c's build_mvp()
     * documents this array as "view[col][row]": the true world-to-camera
     * rotation matrix R_true satisfies R_true[row][col] == view[col][row].
     * Equivalently, reading base_rot the "naive" way -- base_rot[i][j] as
     * plain [row][col] -- gives R_true's TRANSPOSE, i.e. the cockpit's own
     * LOCAL-TO-WORLD orientation matrix (column j = the cockpit's local
     * axis j expressed in world space), here called V to match the
     * derivation below. This was cross-checked three ways against the
     * existing codebase before relying on it: (1) build_mvp()'s own
     * view-col-row storage-convention comment and its tx/ty/tz translation
     * formula, (2) the per-vertex world->camera transform used pervasively
     * elsewhere (car.c/horizon.c/drawtrk3.c/tower.c: viewX = dx*vk1 +
     * dy*vk4 + dz*vk7, i.e. R_true's row 0 is (vk1,vk4,vk7), NOT the
     * (vk1,vk2,vk3) grouping transfrm.c's local-track-chunk position setup
     * uses for an unrelated one-off calculation), and (3) the existing
     * "camera forward in world X/Y/Z" extraction at scene_render_gpu.c
     * (proj.view[0][2], proj.view[1][2], proj.view[2][2] == vk3,vk6,vk9).
     * All three agree with base_rot-naive-read == cockpit-local-to-world
     * == V, and that V's forward column (col 2, +Z) is the game's own
     * +Z-forward convention.
     *
     * --- The composition (fixed after task review found two Critical bugs
     * in an earlier version of this function: the kept projection below
     * assumes a -Z-forward eye space, per OpenXR/GL convention, but this
     * function was feeding it rotations built directly out of V/head_rot
     * with no conversion out of the game's +Z-forward convention and no
     * conversion out of OpenXR's own -Z-forward reference-space axes --
     * net effect: nothing rendered in front of the viewer, and head
     * yaw/pitch came out inverted where it did render) ---
     *
     * Let S = diag(1, 1, -1) (mat3_flip_row2 below) -- the axis flip that
     * converts between OpenXR's -Z-forward convention and the game's
     * +Z-forward convention (both share the same +X-right/+Y-up axes).
     * Let R_true = transpose(V) = the game's own world-to-camera matrix
     * (+Z forward). Let H = head_rot = quat_to_mat3(xr_view->pose.orientation),
     * the head's LOCAL-TO-REFERENCE-SPACE orientation (both the head's own
     * local axes and the reference space's axes are OpenXR-native, i.e.
     * -Z-forward). Then, chasing world -> cockpit -> reference-space ->
     * head-local through each conversion in turn:
     *
     *   world -> cockpit(+Z fwd):        R_true
     *   cockpit(+Z fwd) -> ref-space(-Z fwd, same physical axes): S (S^-1 == S)
     *   ref-space -> head-local:          H^T   (H is local-to-base, so
     *                                             base-to-local is H's
     *                                             transpose/inverse)
     *
     *   combined_rot = H^T * S * R_true
     *
     * This is WORLD-TO-EYE, expressed in the eye's own OpenXR-native
     * (-Z-forward) axes -- exactly the eye-space convention the kept
     * projection matrix below (mvp[11] = -1, i.e. w = -z_eye) expects, so
     * no further conversion is needed after this. Sanity-checked by hand
     * for the identity-head-pose case (H = I): combined_rot reduces to
     * S * R_true, and combined_rot^T * (0,0,-1) == V's own forward column
     * (R_true's own world-forward direction) -- i.e. with the head
     * perfectly still, whatever the base non-VR camera would put at
     * screen-center still lands at the eye's -Z (screen-center) direction.
     * Re-derived independently (not copied from a spec) and re-verified
     * numerically for identity, yaw-left-30 deg, and pitch-up-30 deg cases
     * in vr_mvp_math_test.c (committed alongside this fix). */
    float base_rot[3][3]; /* V, cockpit local-to-world */
    memcpy(base_rot, base_proj->view, sizeof(base_rot));

    float r_true[3][3]; /* R_true = V^T, game's own +Z-forward world-to-camera */
    mat3_transpose(base_rot, r_true);

    /* head_rot, naive read, is H: the head's LOCAL-TO-REFERENCE-SPACE
     * orientation matrix (see quat_to_mat3's own comment), both frames in
     * OpenXR's native -Z-forward convention. */
    float head_rot[3][3];
    quat_to_mat3(&xr_view->pose.orientation, head_rot);

    float head_rot_T[3][3];
    mat3_transpose(head_rot, head_rot_T);

    float s_r_true[3][3]; /* S * R_true */
    mat3_flip_row2(r_true, s_r_true);

    /* combined_rot = H^T * S * R_true -- see the derivation above. Stored
     * naive [row][col], matching proj->view's own R_true convention, which
     * is exactly what the view-matrix assembly below (and build_mvp()'s
     * own convention) expects. */
    float combined_rot[3][3];
    mat3_mul(head_rot_T, s_r_true, combined_rot);

    /* The eye's position: the game camera's own world position plus this
     * eye's tracked position in the reference space, taken through the same
     * chain as the orientation above but run forward instead of inverted:
     * ref-space coordinates -> S -> cockpit-frame numbering -> V -> world.
     * (V's columns are the cockpit's local axes in world coordinates, so
     * V * S * p converts a reference-space offset into a world-space offset,
     * taking the car's current heading into account.) Units: OpenXR meters,
     * scaled by VR_METERS_TO_WORLD to join the world-unit camera position.
     *
     * xr_view->pose is PER EYE: xrLocateViews already reports each eye's own
     * position with the runtime's IPD included (left and right differ by the
     * IPD along the head's right axis), and the same pose is what
     * vr_run_frame() submits in XrCompositionLayerProjectionView.pose. So no
     * separate per-eye IPD offset is applied here -- an earlier version added
     * a further +-31.5 mm along the head right axis (as the design plan
     * prescribed), which counted the IPD twice and made the pose rendered
     * differ from the pose submitted. */
    float p_xr[3] = {
        xr_view->pose.position.x, xr_view->pose.position.y, xr_view->pose.position.z
    };
    float s_p_xr[3] = { p_xr[0], p_xr[1], -p_xr[2] }; /* S * p_xr */
    float head_pos_world[3];
    for (int r = 0; r < 3; r++)
        head_pos_world[r] = (base_rot[r][0] * s_p_xr[0]
                            + base_rot[r][1] * s_p_xr[1]
                            + base_rot[r][2] * s_p_xr[2]) * VR_METERS_TO_WORLD;

    float eye_x = base_cam->viewX + head_pos_world[0];
    float eye_y = base_cam->viewY + head_pos_world[1];
    float eye_z = base_cam->viewZ + head_pos_world[2];

    /* Asymmetric perspective from the OpenXR-reported per-eye FOV -- this is
     * the one place VR genuinely needs a different projection construction
     * than build_mvp()'s symmetric FOV, since HMD lenses are rarely centered. */
    float tan_left = tanf(xr_view->fov.angleLeft);
    float tan_right = tanf(xr_view->fov.angleRight);
    float tan_up = tanf(xr_view->fov.angleUp);
    float tan_down = tanf(xr_view->fov.angleDown);
    float near_plane_m = 0.05f;  /* 5cm, a typical VR near plane in real-world terms */
    float near_plane = near_plane_m * VR_METERS_TO_WORLD;  /* world units -- with the
                                 VR_METERS_TO_WORLD placeholder this comes out to 5.0,
                                 tighter than SCENE_GPU_NEAR's 10.0f world-unit non-VR
                                 default, as intended; revisit alongside VR_METERS_TO_WORLD
                                 in Phase 3 comfort tuning once both can be calibrated
                                 against an actual rendered scene */
    float far_plane = 100000.0f;  /* already a world-unit quantity (not meters) -- same
                                 order of magnitude as the game's own track-overview
                                 camera distances, no conversion needed */

    /* mvp[11] = -1 below (w = -z_eye) assumes a -Z-forward eye space, which is exactly
     * what combined_rot now produces (see its derivation above) -- this asymmetric-FOV
     * block itself is otherwise unchanged from the original draft, since the only real
     * bug here was combined_rot feeding it a +Z-forward rotation. */
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
     * xr_view->fov) already encodes whatever vertical offset the HMD's own lenses need
     * -- stacking shift_y on top would double-apply a vertical offset. Deliberately not
     * applied here. */

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

/* --- Per-frame execution (Task 7: OpenXR bracketing + per-eye present) --- */

static XrView       s_frame_views[2];
static bool         s_should_render_this_tick = false;

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
    /* task-7-round2 New Important 1: marks this frame as open for
     * vr_shutdown() -- see its own comment and s_frame_open's declaration
     * above. Cleared below once this function's own xrEndFrame() runs (or
     * by vr_shutdown() itself, if main_loop_iteration() below reaches it
     * first). */
    s_frame_open = true;

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
     * updatescreen()'s g_bVRMode branch (a later task) calls
     * vr_present_stereo_frame() synchronously, right here in the middle of
     * this function call -- s_frame_views/s_should_render_this_tick above
     * are what it reads, and it fills s_projection_views/
     * s_frame_view_count_for_layer for the xrEndFrame call below. */
    bool keep_running = main_loop_iteration() != 0;

    /* task-7-round2 New Important 1: main_loop_iteration() above can reach
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
        if (s_vr_frames_presented == 1 || s_vr_frames_presented % 60 == 0)
            vr_log_line("INFO", "frame %d submitted (views=%u layerCount=1) "
                                "view[0] pos=(%.3f,%.3f,%.3f) ori=(%.3f,%.3f,%.3f,%.3f)",
                        s_vr_frames_presented, (unsigned)s_frame_view_count_for_layer,
                        s_frame_views[0].pose.position.x, s_frame_views[0].pose.position.y,
                        s_frame_views[0].pose.position.z,
                        s_frame_views[0].pose.orientation.x, s_frame_views[0].pose.orientation.y,
                        s_frame_views[0].pose.orientation.z, s_frame_views[0].pose.orientation.w);
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
         * shouldRender == false for (task-7-review Finding 4). Cancel it
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

        /* --- task-7-review Finding 2 (Important) ---
         * game_render_hw_draw_car() bakes scene_render_gpu_build_vp()'s
         * result into each car's draw command AT QUEUE TIME, i.e. during
         * draw_road()'s own execution -- while world geometry only reads it
         * back at END-FRAME time (Pass A, scene_render_gpu_end_frame_
         * internal). Setting the VR-eye override AFTER draw_road() returns
         * (as this used to) meant cars were already queued with the flat,
         * non-VR camera by the time the override was set -- only world
         * geometry got the correct eye view. Fixed by computing this eye's
         * camera and setting the override BEFORE calling draw_road(), so
         * it's already active before any car gets queued. (This alone isn't
         * enough -- see scene_render_gpu_build_vp()'s own comment on why the
         * override also had to become persist-until-replaced instead of
         * one-shot; both halves of the fix are required together.)
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
         * function's own parameter names. calculateview() only writes to
         * view-state globals (viewx/viewy/viewz, vk1..vk9, etc.) as a pure
         * function of Car[] state (verified by reading view.c/transfrm.c:
         * no Car[] field is ever written, and every global it touches is
         * fully overwritten -- not accumulated -- from this call's own
         * inputs each time) -- calling it here and then again, redundantly,
         * inside draw_road() itself produces the exact same globals both
         * times and has no other side effect. */
        calculateview(iDriveView, uiViewType, 0);
        /* task-7-round2 Minor: draw_road()'s own internal calculateview()
         * call (3d.c draw_road()) is immediately followed by
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
         * a few lines from now and refreshes them. build_mvp_vr_eye() only
         * reads base_cam->viewX/viewY/viewZ and base_proj->view, so only
         * those fields are populated here (the rest zero-init and are
         * unused by it). */
        SceneRenderCamera base_cam = {
            .viewX = viewx, .viewY = viewy, .viewZ = viewz,
        };
        SceneRenderProjection base_proj = {
            .view = {{vk1, vk2, vk3}, {vk4, vk5, vk6}, {vk7, vk8, vk9}},
        };

        float eye_mvp[16];
        build_mvp_vr_eye(eye_mvp, &base_cam, &base_proj, &s_frame_views[i],
                          (int)view_configs_width(i), (int)view_configs_height(i));
        game_render_set_vr_eye_mvp(g_pGameRenderer, eye_mvp);

        /* draw_road() recomputes calculateview()/calculatetransform() fresh
         * (redundant but harmless, see above) and queues this eye's draw
         * list into g_pGameRenderer's single windowless scene instance --
         * there are two draw_road() calls here (one per eye), not two
         * renderer instances. The VR-eye override set above is now active
         * BEFORE any of this call's cars are queued. */
        draw_road(pMainScrPtr, uiViewType, iDriveView, -1, 0);

        Uint32 image_index = 0;
        XrSwapchainImageAcquireInfo acquire_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (XR_FAILED(xrAcquireSwapchainImage(s_swapchains[i], &acquire_info, &image_index))) {
            /* draw_road() above already queued this eye's cars/world
             * geometry into a command buffer that game_render_end_frame_to_
             * vr_eye() will now never be called to submit -- cancel it
             * explicitly (task-7-review Finding 4) rather than leaking it.
             * No swapchain image was acquired yet, so nothing to release. */
            game_render_cancel_frame(g_pGameRenderer);
            return;
        }
        XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
        if (XR_FAILED(xrWaitSwapchainImage(s_swapchains[i], &wait_info))) {
            /* Same pending-command-buffer leak as above (task-7-review
             * Finding 4), plus we still attempt the matching release call
             * here to keep xrAcquireSwapchainImage/xrWaitSwapchainImage/
             * xrReleaseSwapchainImage's strict ordering contract intact for
             * whatever comes next on this swapchain. task-7-round2 Minor:
             * an earlier version of this comment claimed the release "keeps
             * the swapchain usable" here, i.e. that it recovers cleanly --
             * that's not accurate. Per the spec, xrReleaseSwapchainImage is
             * only valid after a successful xrWaitSwapchainImage; since the
             * wait above FAILED, no image was actually waited, so this call
             * is expected to itself fail (XR_ERROR_CALL_ORDER_INVALID) and
             * do nothing -- it's a best-effort attempt to keep the sequence
             * from desyncing further, not a real recovery. Logged rather
             * than assumed, so a genuinely different failure here isn't
             * silently swallowed. */
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
            /* Fence-wait serialization (Approach A from the design spec) --
             * simple and correct; semaphore-based overlap is explicitly
             * deferred to Phase 3 per the spec. */
            SDL_WaitForGPUFences(s_gpu, true, &fence, 1);
            SDL_ReleaseGPUFence(s_gpu, fence);
        }
        /* game_render_end_frame_to_vr_eye() (-> scene_render_gpu_end_frame_
         * internal) always either submits or internally cancels the pending
         * command buffer itself before returning, success or failure -- no
         * separate game_render_cancel_frame() call is needed here regardless
         * of what `resolved` comes back as. Only the swapchain image
         * acquired above still needs releasing on this failure path
         * (task-7-review Finding 4). */
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
         * (the plan's original, unverified assumption) -- checked directly against
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
         *     Task 1's cycle:false requirement needs -- offscreenTex is genuinely
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
         * Equally important and NOT present in the plan's original code: this
         * function must transition the image back to VK_IMAGE_LAYOUT_SHADER_
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
        /* TOP_OF_PIPE_BIT as srcStage (no explicit access masks) mirrors Phase 1's
         * own already-hardware-tested barrier pattern: real synchronization here is
         * provided externally by the SDL_WaitForGPUFences call above (we already
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
         * OpenXR runtime requires". (Phase 1/early Phase 2 left it in GENERAL,
         * which only worked by luck: Monado's release barrier assumes
         * COLOR_ATTACHMENT_OPTIMAL.) Colour-attachment access masks pair with the
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
