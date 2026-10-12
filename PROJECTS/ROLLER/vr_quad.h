/* PROJECTS/ROLLER/vr_quad.h
 *
 * One OpenXR quad layer: its own single-image swapchain, and the copy from an
 * already-rendered SDL_GPUTexture into that swapchain's image (the same raw
 * Vulkan vkCmdCopyImage handoff vr_runtime.c already uses for the eyes, via
 * SDL_GetGPUTextureProperties/SDL_PROP_GPU_TEXTURE_VULKAN_IMAGE_POINTER).
 * Pure plumbing: callers decide what texture, pose and size to submit. */
#ifndef VR_QUAD_H
#define VR_QUAD_H

#include <stdbool.h>
#include <stdint.h>
#include <openxr/openxr.h>
#include <SDL3/SDL_gpu.h>

typedef struct {
    XrSwapchain swapchain;
    uint32_t width, height;
    const char *debug_name; /* for log lines only, e.g. "menu" or "hud" */
} VrQuad;

/* swapchain_format: the already-resolved runtime VkFormat from vr_init() (an
 * int64_t to match XrSwapchainCreateInfo.format; cast your VkFormat to it). */
bool vr_quad_init(XrSession session, int64_t swapchain_format,
                   uint32_t width, uint32_t height, const char *debug_name, VrQuad *out);

/* Copies src (an SDL_GPUTexture of exactly src_w x src_h) into quad's
 * swapchain image, and fills *out_layer ready to go in xrEndFrame's layers
 * array. space/pose/size are used as given; the caller picks the reference
 * space and anchoring policy. Returns false (out_layer left zeroed, nothing to
 * submit this frame) on any failure, logged.
 *
 * Preconditions on src, the same ones the eye copy in vr_runtime.c relies on:
 *   - every GPU command that writes src has already completed (the caller has
 *     waited on its fence, or on SDL_WaitForGPUIdle()); the copy's barriers do
 *     not order against SDL_GPU's own submissions;
 *   - src was created with SDL_GPU_TEXTUREUSAGE_SAMPLER, so SDL_GPU's Vulkan
 *     backend leaves it resting in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
 *     between passes. The copy transitions it to TRANSFER_SRC_OPTIMAL and back
 *     to SHADER_READ_ONLY_OPTIMAL, so SDL_GPU's layout bookkeeping stays right.
 *
 * Uses vr_runtime.c's own copy command buffer and queue (drained with
 * vkQueueWaitIdle before returning), so it must not run while vr_runtime.c's
 * eye copy is mid-record; both run on the game thread, one after the other.
 * No SDL_GPUDevice parameter is needed: SDL_GetGPUTextureProperties() takes
 * only the texture. */
bool vr_quad_submit(VrQuad *quad, SDL_GPUTexture *src,
                     uint32_t src_w, uint32_t src_h, XrSpace space, XrPosef pose,
                     XrExtent2Df size, bool blend_source_alpha,
                     XrCompositionLayerQuad *out_layer);

void vr_quad_destroy(VrQuad *quad);

#endif /* VR_QUAD_H */
