#ifndef VR_VIEW_MERGE_H
#define VR_VIEW_MERGE_H

/* Header-only, dependency-free (OpenXR types only) helper for
 * vr_locate_views(): merges one freshly located XrView with the last
 * known-good one according to the runtime's XrViewStateFlags. Lives in its own
 * header so vr_mvp_math_test.c can test the real function instead of a copy. */

#include <stdbool.h>
#include <openxr/openxr.h>

/* Orientation and position validity are handled independently (a 3DoF /
 * orientation-only headset reports ORIENTATION_VALID without POSITION_VALID and
 * must still render): each component comes from `located` when its valid bit is
 * set, else from `last_good` when there is one (NULL = none yet), else from
 * identity orientation / zero position. The FOV is always the one the runtime
 * returned from the successful xrLocateViews call -- it is not pose-tracking
 * data, and an all-zero FOV would make tanf(0) divide by zero in
 * build_mvp_vr_eye(). */
static inline XrView vr_merge_view(const XrView *located, XrViewStateFlags flags,
                                   const XrView *last_good)
{
    XrView out = *located;
    out.type = XR_TYPE_VIEW;
    out.next = NULL;
    out.fov = located->fov;

    if (!(flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        if (last_good) {
            out.pose.orientation = last_good->pose.orientation;
        } else {
            out.pose.orientation = (XrQuaternionf){ 0.0f, 0.0f, 0.0f, 1.0f };
        }
    }
    if (!(flags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
        if (last_good) {
            out.pose.position = last_good->pose.position;
        } else {
            out.pose.position = (XrVector3f){ 0.0f, 0.0f, 0.0f };
        }
    }
    return out;
}

#endif /* VR_VIEW_MERGE_H */
