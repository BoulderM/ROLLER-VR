/* PROJECTS/ROLLER/vr_quad_math.c -- see vr_quad_math.h */
#include "vr_quad_math.h"

#include <math.h>

float vr_quad_yaw_from_quat(float qx, float qy, float qz, float qw)
{
    /* Forward = R(q) * (0,0,-1) = -(third column of the rotation matrix):
     *   R[0][2] = 2(xz + wy), R[2][2] = 1 - 2(x^2 + y^2)
     * so fwd_x = -2(xz + wy), fwd_z = -(1 - 2(x^2 + y^2)). A heading `yaw`
     * about +Y has forward (-sin yaw, 0, -cos yaw), hence
     * yaw = atan2(-fwd_x, -fwd_z). (The M4 plan's draft had atan2(fwd_x,
     * -fwd_z), which returns -yaw; tests/vr_menu_quad_yaw_test.c caught it.) */
    float fwd_x = -2.0f * (qx * qz + qw * qy);
    float fwd_z = -(1.0f - 2.0f * (qx * qx + qy * qy));
    return atan2f(-fwd_x, -fwd_z);
}

void vr_quad_yaw_anchor(const float head_pos[3], float yaw, float distance,
                        float out_pos[3], float out_quat[4])
{
    out_pos[0] = head_pos[0] - sinf(yaw) * distance;
    out_pos[1] = head_pos[1];
    out_pos[2] = head_pos[2] - cosf(yaw) * distance;
    out_quat[0] = 0.0f;
    out_quat[1] = sinf(yaw * 0.5f);
    out_quat[2] = 0.0f;
    out_quat[3] = cosf(yaw * 0.5f);
}
