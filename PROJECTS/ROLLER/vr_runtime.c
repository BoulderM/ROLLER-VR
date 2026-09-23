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

#include <SDL3/SDL.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static bool s_session_running;

static bool fail(const char *what)
{
    fprintf(stderr, "vr_runtime: %s\n", what);
    return false;
}

static bool fail_xr(const char *what, XrResult result)
{
    fprintf(stderr, "vr_runtime: %s (XrResult=%d)\n", what, (int)result);
    return false;
}

static bool fail_vk(const char *what, VkResult result)
{
    fprintf(stderr, "vr_runtime: %s (VkResult=%d)\n", what, (int)result);
    return false;
}

bool vr_init(void)
{
    /* --- 1. Create the SDL_GPU device (Vulkan) --- */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fail(SDL_GetError());
        return false;
    }

    s_gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
    if (!s_gpu) {
        fail(SDL_GetError());
        return false;
    }
    printf("vr_runtime: SDL_GPU driver: %s\n", SDL_GetGPUDeviceDriver(s_gpu));

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
    printf("vr_runtime: got VkInstance=%p VkDevice=%p queueFamilyIndex=%u\n",
           (void *)s_vk_instance, (void *)s_vk_device, s_vk_queue_family_index);

    /* --- 2. Create the OpenXR instance --- */
    const char *enabled_extensions[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };

    XrInstanceCreateInfo instance_create_info = { .type = XR_TYPE_INSTANCE_CREATE_INFO };
    strncpy(instance_create_info.applicationInfo.applicationName, "ROLLER VR Skeleton",
            XR_MAX_APPLICATION_NAME_SIZE - 1);
    instance_create_info.applicationInfo.applicationVersion = 1;
    instance_create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    instance_create_info.enabledExtensionCount = 1;
    instance_create_info.enabledExtensionNames = enabled_extensions;

    s_instance = XR_NULL_HANDLE;
    XrResult xr_result = xrCreateInstance(&instance_create_info, &s_instance);
    if (XR_FAILED(xr_result)) {
        fail_xr("xrCreateInstance failed", xr_result);
        return false;
    }

    /* --- 3. Get the system (the "head" device — the Simulated HMD when no real headset is attached) --- */
    XrSystemGetInfo system_get_info = {
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };
    xr_result = xrGetSystem(s_instance, &system_get_info, &s_system_id);
    if (XR_FAILED(xr_result)) {
        fail_xr("xrGetSystem failed (no HMD available from the active OpenXR runtime?)", xr_result);
        return false;
    }

    /* Required by the OpenXR spec before creating a Vulkan-backed session,
     * even though we already have our own device and don't act on the
     * returned version range here. */
    PFN_xrGetVulkanGraphicsRequirements2KHR pfn_get_vk_requirements = NULL;
    xr_result = xrGetInstanceProcAddr(s_instance, "xrGetVulkanGraphicsRequirements2KHR",
                                       (PFN_xrVoidFunction *)&pfn_get_vk_requirements);
    if (XR_FAILED(xr_result) || !pfn_get_vk_requirements) {
        fail_xr("failed to load xrGetVulkanGraphicsRequirements2KHR", xr_result);
        return false;
    }
    XrGraphicsRequirementsVulkan2KHR vk_requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    xr_result = pfn_get_vk_requirements(s_instance, s_system_id, &vk_requirements);
    if (XR_FAILED(xr_result)) {
        fail_xr("xrGetVulkanGraphicsRequirements2KHR failed", xr_result);
        return false;
    }

    /* Also required by the spec (XR_KHR_vulkan_enable2) when the app supplies
     * its own already-created Vulkan device, as we do here via SDL_GPU: the
     * runtime must be told which physical device we're using before
     * xrCreateSession, even though we already know it ourselves. Skipping
     * this call makes xrCreateSession fail with XR_ERROR_VALIDATION_FAILURE
     * ("Has not called xrGetVulkanGraphicsDevice2KHR") on at least Monado. */
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
    VkPhysicalDevice xr_required_physical_device = VK_NULL_HANDLE;
    xr_result = pfn_get_vk_device(s_instance, &vk_device_get_info, &xr_required_physical_device);
    if (XR_FAILED(xr_result)) {
        fail_xr("xrGetVulkanGraphicsDevice2KHR failed", xr_result);
        return false;
    }
    if (xr_required_physical_device != s_vk_physical_device) {
        fail("SDL_GPU's Vulkan physical device does not match the physical device the OpenXR "
             "runtime requires (xrGetVulkanGraphicsDevice2KHR) -- this can happen on multi-GPU "
             "systems; creating the session against a mismatched device/physicalDevice pair is "
             "undefined behavior per the Vulkan spec");
        return false;
    }

    /* --- 4. Create the session against our existing Vulkan device --- */
    XrGraphicsBindingVulkan2KHR vk_binding = {
        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
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
    printf("vr_runtime: OpenXR session created\n");

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
    printf("vr_runtime: view count = %u, view[0] recommended %ux%u\n",
           s_view_count, s_view_configs[0].recommendedImageRectWidth,
           s_view_configs[0].recommendedImageRectHeight);

    /* --- 6. Create one swapchain per eye --- */
    VkFormat swapchain_format = VK_FORMAT_R8G8B8A8_UNORM;
    s_swapchains = calloc(s_view_count, sizeof(XrSwapchain));
    for (Uint32 i = 0; i < s_view_count; i++) {
        XrSwapchainCreateInfo swapchain_create_info = {
            .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
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
    printf("vr_runtime: created %u swapchains\n", s_view_count);

    /* --- 6b. Create a reference space for the projection layer. A composition
     * layer's `space` must reference a real, previously-created XrSpace — this
     * runtime doesn't do head tracking yet (Phase 2 follow-up tasks add it),
     * so LOCAL with an identity pose is enough to satisfy the API contract. */
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

    s_session_running = false;

    return true;
}

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

/* --- Live head tracking + per-eye MVP construction (Phase 2) ------------ */

/* Pure branch logic extracted out of vr_locate_views() so it can be unit-
 * tested without a real OpenXR session (no headset is attached to
 * fatal-gpu, so real tracking-loss can't be triggered on demand). */
static bool vr_pose_is_usable(XrViewStateFlags flags)
{
    return (flags & (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT))
           == (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT);
}

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

    if (!vr_pose_is_usable(view_state.viewStateFlags)) {
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
 * OpenXR's local/reference-space axis convention (-Z forward, matching the
 * eye/view-space convention the kept projection matrix below expects) and
 * the game's own +Z-forward convention (R_true/base_rot, vk1..vk9) -- both
 * are right-handed, +X right/+Y up, and differ only in which way Z points,
 * so a single-axis flip is the full, exact conversion between them. */
static void mat3_flip_row2(const float src[3][3], float dst[3][3])
{
    dst[0][0] = src[0][0]; dst[0][1] = src[0][1]; dst[0][2] = src[0][2];
    dst[1][0] = src[1][0]; dst[1][1] = src[1][1]; dst[1][2] = src[1][2];
    dst[2][0] = -src[2][0]; dst[2][1] = -src[2][1]; dst[2][2] = -src[2][2];
}

/* OpenXR reports positions/IPD in meters; ROLLER's world units are a much
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
 * below (head position delta, IPD offset, near plane) before it's combined
 * with world-unit camera state. */
#define VR_METERS_TO_WORLD 100.0f

void build_mvp_vr_eye(float mvp_out[16],
                       const SceneRenderCamera *base_cam,
                       const SceneRenderProjection *base_proj,
                       const XrView *xr_view,
                       float eye_offset_m,
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

    /* Eye offset along the COMBINED eye's local right axis, and the head's
     * tracked position delta, both need the SAME world -> ref-space chain
     * as the orientation above, just run forward instead of inverted:
     * ref-space/head-local coordinates -> S -> cockpit-frame numbering ->
     * V -> world. (Column 0 of a local-to-base matrix is that frame's own
     * local +X expressed in base coordinates -- so column 0 of V*S*H is
     * the head's local right axis, after both the head's own rotation and
     * the cockpit's current world heading are applied, expressed in world
     * coordinates -- not head_rot's right axis in isolation, which would
     * ignore the car's heading entirely and only be correct while the car
     * faces the reference space's own reference direction.) Both
     * quantities are OpenXR meters, so both are scaled by
     * VR_METERS_TO_WORLD before joining the world-unit camera position. */
    float s_head_rot[3][3]; /* S * H */
    mat3_flip_row2(head_rot, s_head_rot);
    float v_s_h[3][3]; /* V * S * H */
    mat3_mul(base_rot, s_head_rot, v_s_h);
    float right_world[3] = { v_s_h[0][0], v_s_h[1][0], v_s_h[2][0] };

    float p_xr[3] = {
        xr_view->pose.position.x, xr_view->pose.position.y, xr_view->pose.position.z
    };
    float s_p_xr[3] = { p_xr[0], p_xr[1], -p_xr[2] }; /* S * p_xr */
    float head_pos_world[3];
    for (int r = 0; r < 3; r++)
        head_pos_world[r] = (base_rot[r][0] * s_p_xr[0]
                            + base_rot[r][1] * s_p_xr[1]
                            + base_rot[r][2] * s_p_xr[2]) * VR_METERS_TO_WORLD;

    float eye_offset_world = eye_offset_m * VR_METERS_TO_WORLD;
    float eye_x = base_cam->viewX + head_pos_world[0] + right_world[0] * eye_offset_world;
    float eye_y = base_cam->viewY + head_pos_world[1] + right_world[1] * eye_offset_world;
    float eye_z = base_cam->viewZ + head_pos_world[2] + right_world[2] * eye_offset_world;

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
