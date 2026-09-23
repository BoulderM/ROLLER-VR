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

#endif /* VR_RUNTIME_H */
