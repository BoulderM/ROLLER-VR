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
 * space and anchoring policy. blend_source_alpha: blend by src's alpha,
 * read as straight (unpremultiplied) alpha -- both
 * BLEND_TEXTURE_SOURCE_ALPHA_BIT and UNPREMULTIPLIED_ALPHA_BIT are set;
 * false submits the quad opaque. Returns false (out_layer left zeroed, nothing to
 * submit this frame) on any failure, logged.
 *
 * Precondition on src: it was created with SDL_GPU_TEXTUREUSAGE_SAMPLER, so
 * SDL_GPU's Vulkan backend leaves it resting in
 * VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL between passes. The copy
 * transitions it to TRANSFER_SRC_OPTIMAL and back, so SDL_GPU's layout
 * bookkeeping stays right. The caller does NOT need to have waited on a
 * fence or SDL_WaitForGPUIdle() first: the copy's first barrier uses
 * VK_PIPELINE_STAGE_ALL_COMMANDS_BIT/VK_ACCESS_MEMORY_WRITE_BIT as its source
 * scope, which orders against whatever last wrote src on this same queue
 * (same-queue submission order is enough for that scope to apply), whether
 * or not the caller already synced on the CPU side.
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
