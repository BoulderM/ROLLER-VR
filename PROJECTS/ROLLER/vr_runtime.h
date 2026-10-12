#ifndef VR_RUNTIME_H
#define VR_RUNTIME_H

#include <stdbool.h>
#include <SDL3/SDL_gpu.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include "scene_render_types.h"

/* Step 1 of VR startup, called by InitSDL()'s VR branch BEFORE the GPU device is
 * created: creates the OpenXR instance, finds the headset, and asks the runtime
 * which Vulkan API version and instance/device extensions its Vulkan interop
 * needs. Fills *out_options (arrays owned by this module, valid until
 * vr_shutdown()) so SDL_GPU can create a compatible VkInstance/VkDevice via
 * SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER. Uses XR_KHR_vulkan_enable
 * (application-created Vulkan objects) and falls back to XR_KHR_vulkan_enable2
 * with a logged warning. Returns false on failure (details in the log). */
bool vr_prepare_vulkan(SDL_GPUVulkanOptions *out_options);

/* Step 2, called from main() after InitSDL(): adopts SDL_GPU's device, creates
 * the session, swapchains and reference space. Requires vr_prepare_vulkan(). */
bool vr_init(void);
void vr_shutdown(void);

/* Process exit status for a VR run: 0 for an orderly end (game quit, or the
 * runtime ending the session), 1 if an OpenXR frame call failed. */
int vr_exit_status(void);

/* Locates both eyes' head-tracked poses for the given predicted display
 * time. The FOV is always the runtime's. Orientation and position are merged
 * independently (vr_merge_view(), vr_view_merge.h): each comes from the located
 * view when the runtime flags it valid, else from the last-known-good view,
 * else identity/zero -- so an orientation-only (3DoF) headset still renders and
 * the first frame never produces a zero-FOV/NaN view. Returns false only when
 * xrLocateViews itself hard-fails or returns fewer than 2 views and there is
 * no last-known-good view to fall back to. */
bool vr_locate_views(XrTime predicted_display_time, XrView out_views[2]);

/* World-units-per-meter scale for every meter-scale VR quantity (head position
 * delta, eye separation, near plane). Returns g_fVRMetersToWorldOverride
 * (--vr-meters-to-world) when set, else derives it from Car[0]'s bounding box
 * in CarBox (car.h): car length in world units / a reference real-world car
 * length. The derived scale is stable for the session. Cheap enough to call
 * per eye per frame. Requires Car[]/CarBox to be populated (CalcCarSizes(),
 * car.c) -- true whenever a race is being presented. */
float vr_resolve_meters_to_world(void);

/* Builds a full model-view-projection matrix for one eye: base_cam/base_proj
 * (computed elsewhere by calculateview()/calculatetransform(), called with
 * this eye's pose active via vr_camera_compose_set_active_eye() -- see
 * vr_present_stereo_frame()) already carry that eye's FULLY composed
 * head-tracked camera, rotation and position alike; this function does not
 * compose any pose of its own. It only builds the asymmetric-FOV projection
 * OpenXR reports for the eye (fov) on top of that already-composed camera --
 * the one piece of VR eye rendering build_mvp()'s symmetric-FOV construction
 * (scene_render_gpu.c) can't express, since HMD lenses are rarely centered.
 * mvp_out uses the same column-major convention as scene_render_gpu.c's
 * build_mvp() output. */
void build_mvp_vr_eye_projection(float mvp_out[16],
                                  const SceneRenderCamera *base_cam,
                                  const SceneRenderProjection *base_proj,
                                  const XrFovf *fov);

/* --- Per-frame execution (game-loop integration) ------------------------- */

/* Drives one OpenXR frame: polls events, handles session state transitions
 * (including the initial xrBeginSession once XR_SESSION_STATE_READY), and
 * brackets the real game tick (main_loop_iteration()) between
 * xrWaitFrame/xrBeginFrame and xrEndFrame. Call in a tight loop from
 * frontend_run_game_loop()'s VR branch (3d.c) in place of calling
 * main_loop_iteration() directly -- vr_run_frame() calls it internally.
 * Returns false once the game has finished its own shutdown. A quit that
 * originates in OpenXR (the runtime ending the session, an OpenXR failure) does
 * not return early: it is forwarded to the game, and later calls run the game's
 * loop without OpenXR frames until frontend_shutdown_finish() has run. Returns
 * true otherwise (including the "not session_running yet" wait state, where the
 * caller should just keep looping).
 *
 * While main_loop_iteration() runs, if it reaches race_draw()->
 * updatescreen(), updatescreen()'s g_bVRMode branch calls
 * vr_present_stereo_frame() (below) synchronously, partway through this
 * call -- this function's own per-tick state (the frame state, both eyes'
 * located views, and whether this tick should render at all) is what that
 * call reads. */
bool vr_run_frame(void);

/* Renders and presents both eyes for the current tick. Called synchronously
 * from inside main_loop_iteration() (via updatescreen()'s g_bVRMode branch),
 * partway through vr_run_frame() above -- not called
 * directly by frontend_run_game_loop(). pMainScrPtr/uiViewType/iDriveView
 * are exactly the arguments the existing non-VR path already passes to
 * draw_road() from updatescreen() (pMainScrPtr, ViewType[0], DriveView[0]);
 * this function calls draw_road() itself, once per eye, overriding the MVP
 * with that eye's head-tracked matrix each time. No-op (and
 * clears the pending composition layer's view count to 0, so vr_run_frame()
 * submits a valid empty xrEndFrame) if this tick's OpenXR frame state said
 * shouldRender == false.
 *
 * Calling-convention precondition: the caller must have already called
 * game_render_begin_frame(g_pGameRenderer) once for this tick before this
 * function runs -- exactly the same single call the non-VR path already
 * makes (updatescreen()'s existing top-of-function call) before its own
 * single draw_road()+game_render_end_frame() pair. This function calls
 * game_render_begin_frame() itself before every eye AFTER the first (each
 * eye's own game_render_end_frame_to_vr_eye() call consumes/submits the
 * command buffer, so a fresh one must be re-acquired before the next eye's
 * draw_road() can queue anything) but deliberately does not call it again
 * for the first eye, to avoid discarding the caller's already-acquired,
 * not-yet-submitted command buffer. */
void vr_present_stereo_frame(uint8 *pMainScrPtr, unsigned int uiViewType, int iDriveView);

/* SDL_GPU's Vulkan queue and the runtime's one copy command buffer (from a
 * VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT pool), as the eye copy uses
 * them. vr_quad_submit() (vr_quad.c) borrows both for its own copy: reset,
 * record, submit, vkQueueWaitIdle -- never freed or kept in flight by it.
 * Valid between vr_init() and vr_shutdown(). */
VkQueue vr_runtime_vk_queue(void);
VkCommandBuffer vr_runtime_copy_command_buffer(void);

#endif /* VR_RUNTIME_H */
