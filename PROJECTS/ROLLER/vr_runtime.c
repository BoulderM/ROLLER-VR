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
