/* PROJECTS/ROLLER/vr_quad.c -- see vr_quad.h */
#include "vr_quad.h"
#include "vr_runtime.h"  /* vr_runtime_vk_queue(), vr_runtime_copy_command_buffer() */
#include "vr_log.h"

#include <SDL3/SDL.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr_platform.h>  /* XrSwapchainImageVulkan2KHR */

#include <stdlib.h>
#include <string.h>

bool vr_quad_init(XrSession session, int64_t swapchain_format,
                   uint32_t width, uint32_t height, const char *debug_name, VrQuad *out)
{
    memset(out, 0, sizeof(*out));
    out->debug_name = debug_name;
    out->width = width;
    out->height = height;

    /* TRANSFER_DST: the image is a vkCmdCopyImage destination. SAMPLED: the
     * compositor samples quad layers. COLOR_ATTACHMENT: not because anything
     * renders into it, but because XR_KHR_vulkan_enable hands the image back to
     * the runtime in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL on release (the
     * layout the eye copy also ends in, vr_runtime.c), and a Vulkan barrier into
     * that layout is only valid for an image created with the colour-attachment
     * usage bit. Same three flags as the eye swapchains. */
    XrSwapchainCreateInfo create_info = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT
                    | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
        .format = swapchain_format,
        .sampleCount = 1,
        .width = width,
        .height = height,
        .faceCount = 1,
        .arraySize = 1,
        .mipCount = 1,
    };
    XrResult r = xrCreateSwapchain(session, &create_info, &out->swapchain);
    if (XR_FAILED(r)) {
        vr_log_line("WARN", "vr_quad(%s): xrCreateSwapchain failed (XrResult=%d)", debug_name, (int)r);
        memset(out, 0, sizeof(*out));
        return false;
    }
    vr_log_line("INFO", "vr_quad(%s): created %ux%u swapchain", debug_name, width, height);
    return true;
}

static void vr_quad_release_image(VrQuad *quad)
{
    XrSwapchainImageReleaseInfo release_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    XrResult r = xrReleaseSwapchainImage(quad->swapchain, &release_info);
    if (XR_FAILED(r))
        vr_log_line("WARN", "vr_quad_submit(%s): xrReleaseSwapchainImage failed (XrResult=%d)",
                    quad->debug_name, (int)r);
}

bool vr_quad_submit(VrQuad *quad, SDL_GPUTexture *src,
                     uint32_t src_w, uint32_t src_h, XrSpace space, XrPosef pose,
                     XrExtent2Df size, bool blend_source_alpha,
                     XrCompositionLayerQuad *out_layer)
{
    memset(out_layer, 0, sizeof(*out_layer));
    if (!quad->swapchain || !src || src_w != quad->width || src_h != quad->height) {
        vr_log_line("WARN", "vr_quad_submit(%s): bad args (swapchain=%p src=%p %ux%u vs quad %ux%u)",
                    quad->debug_name, (void *)quad->swapchain, (void *)src, src_w, src_h,
                    quad->width, quad->height);
        return false;
    }

    /* Looked up before acquiring, so a missing VkImage costs no swapchain image. */
    SDL_PropertiesID tex_props = SDL_GetGPUTextureProperties(src);
    VkImage src_vk_image = (VkImage)SDL_GetPointerProperty(
        tex_props, SDL_PROP_GPU_TEXTURE_VULKAN_IMAGE_POINTER, NULL);
    if (!src_vk_image) {
        vr_log_line("ERROR", "vr_quad_submit(%s): no VkImage on the source texture (patched SDL fork?)",
                    quad->debug_name);
        return false;
    }

    XrSwapchainImageAcquireInfo acquire_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    uint32_t image_index = 0;
    XrResult r = xrAcquireSwapchainImage(quad->swapchain, &acquire_info, &image_index);
    if (XR_FAILED(r)) {
        vr_log_line("WARN", "vr_quad_submit(%s): xrAcquireSwapchainImage failed (XrResult=%d)",
                    quad->debug_name, (int)r);
        return false;
    }
    XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
    r = xrWaitSwapchainImage(quad->swapchain, &wait_info);
    if (XR_FAILED(r)) {
        /* xrReleaseSwapchainImage is only valid after a successful
         * xrWaitSwapchainImage, so nothing is released here (see the eye
         * copy's matching comment in vr_present_stereo_frame()). */
        vr_log_line("WARN", "vr_quad_submit(%s): xrWaitSwapchainImage failed (XrResult=%d)",
                    quad->debug_name, (int)r);
        return false;
    }

    uint32_t xr_image_count = 0;
    xrEnumerateSwapchainImages(quad->swapchain, 0, &xr_image_count, NULL);
    XrSwapchainImageVulkan2KHR *xr_images = xr_image_count
        ? (XrSwapchainImageVulkan2KHR *)calloc(xr_image_count, sizeof(*xr_images)) : NULL;
    if (!xr_images) {
        vr_log_line("WARN", "vr_quad_submit(%s): no swapchain images (count=%u)", quad->debug_name, xr_image_count);
        vr_quad_release_image(quad);
        return false;
    }
    for (uint32_t k = 0; k < xr_image_count; k++) xr_images[k].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    r = xrEnumerateSwapchainImages(quad->swapchain, xr_image_count, &xr_image_count,
                                   (XrSwapchainImageBaseHeader *)xr_images);
    VkImage target_image = (XR_SUCCEEDED(r) && image_index < xr_image_count)
        ? xr_images[image_index].image : VK_NULL_HANDLE;
    free(xr_images);
    if (target_image == VK_NULL_HANDLE) {
        vr_log_line("WARN", "vr_quad_submit(%s): xrEnumerateSwapchainImages failed (XrResult=%d, index %u of %u)",
                    quad->debug_name, (int)r, image_index, xr_image_count);
        vr_quad_release_image(quad);
        return false;
    }

    /* The copy borrows vr_runtime.c's own copy command buffer and queue, the
     * same ones the eye copy uses (vkResetCommandBuffer, record, vkQueueSubmit,
     * vkQueueWaitIdle). Both drain the queue with vkQueueWaitIdle before
     * returning, so the buffer is never in flight when it is reset here. */
    VkCommandBuffer cmd = vr_runtime_copy_command_buffer();
    VkQueue queue = vr_runtime_vk_queue();
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                             .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cmd, &begin_info);

    const VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1,
    };
    /* Same barriers as the eye copy (vr_present_stereo_frame(), whose comments
     * carry the reasoning): src leaves and re-enters SDL_GPU's SAMPLER layout,
     * dst goes UNDEFINED -> TRANSFER_DST -> COLOR_ATTACHMENT_OPTIMAL (the layout
     * XR_KHR_vulkan_enable says the runtime expects on release).
     *
     * srcStageMask is ALL_COMMANDS (not TOP_OF_PIPE) with a MEMORY_WRITE source
     * access: within one queue, a pipeline barrier's first synchronization scope
     * covers every command submitted earlier on that queue, so this is enough to
     * order against whatever wrote src -- a prior vkQueueSubmit, with no fence
     * wait needed first -- without knowing whether the write was a transfer, a
     * render pass, or something else. TOP_OF_PIPE/0 (the first draft) waits on
     * nothing at all and raced SDL_GPU's own upload of a freshly-written src. */
    VkImageMemoryBarrier pre[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = src_vk_image, .subresourceRange = range,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = target_image, .subresourceRange = range,
        },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 2, pre);

    VkImageCopy region = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .extent = { src_w, src_h, 1 },
    };
    vkCmdCopyImage(cmd, src_vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier post[2] = { pre[0], pre[1] };
    post[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    post[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    post[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    post[0].dstAccessMask = 0;
    post[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    post[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    post[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    post[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, NULL, 0, NULL, 2, post);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VkResult vk_r = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    if (vk_r == VK_SUCCESS)
        vkQueueWaitIdle(queue); /* the buffer is borrowed, not owned: drained, never freed here */
    vr_quad_release_image(quad);
    if (vk_r != VK_SUCCESS) {
        vr_log_line("WARN", "vr_quad_submit(%s): vkQueueSubmit failed (VkResult=%d)", quad->debug_name, (int)vk_r);
        return false;
    }

    /* The whole single-layer image: offset 0, full size, array index 0
     * (arraySize = 1 at creation). */
    out_layer->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    out_layer->next = NULL;
    out_layer->layerFlags = blend_source_alpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
    out_layer->space = space;
    out_layer->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    out_layer->subImage.swapchain = quad->swapchain;
    out_layer->subImage.imageRect.offset.x = 0;
    out_layer->subImage.imageRect.offset.y = 0;
    out_layer->subImage.imageRect.extent.width = (int32_t)quad->width;
    out_layer->subImage.imageRect.extent.height = (int32_t)quad->height;
    out_layer->subImage.imageArrayIndex = 0;
    out_layer->pose = pose;
    out_layer->size = size;
    return true;
}

void vr_quad_destroy(VrQuad *quad)
{
    if (quad->swapchain)
        xrDestroySwapchain(quad->swapchain);
    memset(quad, 0, sizeof(*quad));
}
