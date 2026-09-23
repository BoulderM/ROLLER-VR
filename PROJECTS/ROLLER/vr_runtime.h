#ifndef VR_RUNTIME_H
#define VR_RUNTIME_H

#include <stdbool.h>
#include <openxr/openxr.h>
#include "scene_render_types.h"

bool vr_init(void);
void vr_shutdown(void);

/* Locates both eyes' head-tracked poses for the given predicted display
 * time. Falls back to the last-known-good pose (or identity, if there is
 * none yet -- e.g. the very first frame) whenever the runtime reports
 * invalid/untracked poses, so callers always get two usable XrViews.
 * Returns false only when xrLocateViews itself hard-fails or returns fewer
 * than 2 views and there is no last-known-good pose to fall back to. */
bool vr_locate_views(XrTime predicted_display_time, XrView out_views[2]);

/* Builds a full model-view-projection matrix for one eye: the game's normal
 * car-follow camera (base_cam/base_proj, unchanged, computed elsewhere by
 * calculateview()/calculatetransform()) composed with that eye's live
 * head-tracked pose (xr_view) and a per-eye IPD offset (eye_offset_m,
 * signed -- negative for the left eye, positive for the right), projected
 * with the asymmetric FOV OpenXR reports for the eye (xr_view->fov).
 * mvp_out uses the same column-major convention as scene_render_gpu.c's
 * build_mvp() output. vpW/vpH are currently unused (reserved for future
 * comfort/resolution tuning). */
void build_mvp_vr_eye(float mvp_out[16],
                       const SceneRenderCamera *base_cam,
                       const SceneRenderProjection *base_proj,
                       const XrView *xr_view,
                       float eye_offset_m,
                       int vpW, int vpH);

/* --- Per-frame execution (Phase 2 game-loop integration) ----------------- */

/* Drives one OpenXR frame: polls events, handles session state transitions
 * (including the initial xrBeginSession once XR_SESSION_STATE_READY), and
 * brackets the real game tick (main_loop_iteration()) between
 * xrWaitFrame/xrBeginFrame and xrEndFrame. Call in a tight loop from
 * frontend_run_game_loop()'s VR branch (3d.c) in place of calling
 * main_loop_iteration() directly -- vr_run_frame() calls it internally.
 * Returns false when the game itself wants to quit (main_loop_iteration()
 * returned 0) or the OpenXR session is exiting/lost; true otherwise
 * (including the "not session_running yet" wait state, where the caller
 * should just keep looping).
 *
 * While main_loop_iteration() runs, if it reaches race_draw()->
 * updatescreen(), updatescreen()'s g_bVRMode branch calls
 * vr_present_stereo_frame() (below) synchronously, partway through this
 * call -- this function's own per-tick state (the frame state, both eyes'
 * located views, and whether this tick should render at all) is what that
 * call reads. */
bool vr_run_frame(void);

/* Renders and presents both eyes for the current tick. Called synchronously
 * from inside main_loop_iteration() (via updatescreen()'s g_bVRMode branch,
 * a later task), partway through vr_run_frame() above -- not called
 * directly by frontend_run_game_loop(). pMainScrPtr/uiViewType/iDriveView
 * are exactly the arguments the existing non-VR path already passes to
 * draw_road() from updatescreen() (pMainScrPtr, ViewType[0], DriveView[0]);
 * this function calls draw_road() itself, once per eye, overriding the MVP
 * with that eye's head-tracked, IPD-offset matrix each time. No-op (and
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

#endif /* VR_RUNTIME_H */
