/* PROJECTS/ROLLER/vr_skeleton_main.c
 *
 * VR Phase 1 walking skeleton: proves OpenXR can open a session against the
 * same Vulkan device SDL_GPU owns, and render a solid color to both eyes.
 * Deliberately does not touch any ROLLER game/rendering code — see the
 * "Refinement from the spec" note in the implementation plan this came from.
 */
#include <SDL3/SDL.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
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

    /* Also required by the spec (XR_KHR_vulkan_enable2) when the app supplies
     * its own already-created Vulkan device, as we do here via SDL_GPU: the
     * runtime must be told which physical device we're using before
     * xrCreateSession, even though we already know it ourselves. Skipping
     * this call makes xrCreateSession fail with XR_ERROR_VALIDATION_FAILURE
     * ("Has not called xrGetVulkanGraphicsDevice2KHR") on at least Monado. */
    PFN_xrGetVulkanGraphicsDevice2KHR pfn_get_vk_device = NULL;
    xr_result = xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsDevice2KHR",
                                       (PFN_xrVoidFunction *)&pfn_get_vk_device);
    if (XR_FAILED(xr_result) || !pfn_get_vk_device) {
        fail_xr("failed to load xrGetVulkanGraphicsDevice2KHR", xr_result);
    }
    XrVulkanGraphicsDeviceGetInfoKHR vk_device_get_info = {
        .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
        .systemId = system_id,
        .vulkanInstance = vk_instance,
    };
    VkPhysicalDevice xr_required_physical_device = VK_NULL_HANDLE;
    xr_result = pfn_get_vk_device(instance, &vk_device_get_info, &xr_required_physical_device);
    if (XR_FAILED(xr_result)) {
        fail_xr("xrGetVulkanGraphicsDevice2KHR failed", xr_result);
    }

    /* --- 4. Create the session against our existing Vulkan device --- */
    XrGraphicsBindingVulkan2KHR vk_binding = {
        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .instance = vk_instance,
        .physicalDevice = xr_required_physical_device,
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
