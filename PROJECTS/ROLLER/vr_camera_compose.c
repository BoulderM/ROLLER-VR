#include "vr_camera_compose.h"
#include <math.h>

void vr_quat_to_mat3(float x, float y, float z, float w, float m[3][3])
{
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] =     2 * (x * y - z * w); m[0][2] =     2 * (x * z + y * w);
    m[1][0] =     2 * (x * y + z * w); m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] =     2 * (y * z - x * w);
    m[2][0] =     2 * (x * z - y * w); m[2][1] =     2 * (y * z + x * w); m[2][2] = 1 - 2 * (x * x + y * y);
}

void vr_mat3_transpose(const float src[3][3], float dst[3][3])
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            dst[r][c] = src[c][r];
}

void vr_mat3_mul(const float a[3][3], const float b[3][3], float out[3][3])
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            float sum = 0.0f;
            for (int k = 0; k < 3; k++) sum += a[r][k] * b[k][c];
            out[r][c] = sum;
        }
}

void vr_mat3_flip_row2(const float src[3][3], float dst[3][3])
{
    dst[0][0] = src[0][0]; dst[0][1] = src[0][1]; dst[0][2] = src[0][2];
    dst[1][0] = src[1][0]; dst[1][1] = src[1][1]; dst[1][2] = src[1][2];
    dst[2][0] = -src[2][0]; dst[2][1] = -src[2][1]; dst[2][2] = -src[2][2];
}

void vr_camera_compose(const float base_rot[3][3], const float base_pos[3],
                        float qx, float qy, float qz, float qw,
                        float px, float py, float pz,
                        float meters_to_world,
                        float out_rot[3][3], float out_pos[3])
{
    /* Copy base_rot first -- out_rot/out_pos may alias base_rot/base_pos,
     * and we read base_rot repeatedly below. */
    float V[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            V[r][c] = base_rot[r][c];
    float base_pos_copy[3] = { base_pos[0], base_pos[1], base_pos[2] };

    float H[3][3];
    vr_quat_to_mat3(qx, qy, qz, qw, H);

    /* H_game = S*H*S: flip sign of row 2 and column 2 (double-negating
     * H[2][2], leaving it unchanged). Written directly rather than via two
     * vr_mat3_flip_row2() calls to avoid an unnecessary intermediate copy. */
    float H_game[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            float sign = ((r == 2) ? -1.0f : 1.0f) * ((c == 2) ? -1.0f : 1.0f);
            H_game[r][c] = H[r][c] * sign;
        }

    vr_mat3_mul(V, H_game, out_rot);

    /* head_pos_world = V * S * p_xr * meters_to_world (S*p_xr just negates
     * the Z component of the raw OpenXR position). */
    float s_p[3] = { px, py, -pz };
    for (int r = 0; r < 3; r++)
        out_pos[r] = base_pos_copy[r]
                   + meters_to_world * (V[r][0] * s_p[0] + V[r][1] * s_p[1] + V[r][2] * s_p[2]);
}

void vr_camera_view_basis(const float base_rot[3][3], const float base_pos[3],
                           float out_combined_rot[3][3], float out_eye_pos[3])
{
    float r_true[3][3];
    vr_mat3_transpose(base_rot, r_true);
    vr_mat3_flip_row2(r_true, out_combined_rot);

    out_eye_pos[0] = base_pos[0];
    out_eye_pos[1] = base_pos[1];
    out_eye_pos[2] = base_pos[2];
}

/* Mirrors control.c's getangle() exactly (same formula, same literal) so
 * the 14-bit indices this module produces are consistent with every other
 * angle in the engine -- reimplemented here (not called directly) to keep
 * this module link-independent of control.c's large dependency chain. If
 * getangle() itself ever changes, update this to match. */
static int vr_angle14(float x, float y)
{
    if (x == 0 && y == 0) return 0;
    double a = atan2((double)y, (double)x) * 16384.0 / 6.28318530718;
    return (int)a & 0x3FFF;
}

void vr_camera_angles_from_rotation(const float rot[3][3],
                                     int *out_yaw, int *out_pitch, int *out_roll)
{
    /* Forward = column 2, roll vector = minus column 0 -- this header's own
     * vr_camera_compose() derivation comment establishes col 2 as this
     * engine's world-forward column for a local-to-world matrix in this
     * [row][col] convention; vr_runtime.c's build_mvp_vr_eye_projection()
     * (via vr_camera_view_basis() below) relies on the same convention. */
    float fx = rot[0][2], fy = rot[1][2], fz = rot[2][2];
    /* Roll vector = MINUS column 0 (not the up column): getworldangles()'s
     * roll vector (sR*sP*cY - cR*sY, sR*sP*sY + cR*cY, -sR*cP) is exactly
     * -(vk1, vk4, vk7) of calculatetransform() (transfrm.c:43-57). */
    float rollx = -rot[0][0], rolly = -rot[1][0], rollz = -rot[2][0];

    /* Azimuth from the forward vector's horizontal projection -- mirrors
     * getworldangles()'s "iNewAzimuth = getangle(fProjX, fProjY)". */
    int azimuth = vr_angle14(fx, fy);
    float cosAz = cosf((float)azimuth * (2.0f * (float)M_PI / 16384.0f));
    float sinAz = sinf((float)azimuth * (2.0f * (float)M_PI / 16384.0f));

    /* Elevation from the forward vector's horizontal magnitude (recovered
     * exactly by rotating the horizontal projection back onto its own
     * azimuth direction -- mirrors getworldangles()'s "fRotatedX") vs. its
     * Z component. */
    float rotatedX = fx * cosAz + fy * sinAz;
    int elevation = vr_angle14(rotatedX, fz);
    float cosEl = cosf((float)elevation * (2.0f * (float)M_PI / 16384.0f));
    float sinEl = sinf((float)elevation * (2.0f * (float)M_PI / 16384.0f));

    /* Bank/roll: rotate the roll vector (minus column 0) by -azimuth then by
     * -elevation, the same as getworldangles()'s final steps. */
    float rollRotatedX =  rollx * cosAz + rolly * sinAz;
    float rollRotatedY = -rollx * sinAz + rolly * cosAz;
    float finalX = rollRotatedX * sinEl - rollz * cosEl;
    int bank = vr_angle14(rollRotatedY, finalX);

    *out_yaw = azimuth;
    *out_pitch = elevation;
    *out_roll = bank;
}

static bool s_active = false;
static float s_qx, s_qy, s_qz, s_qw;
static float s_px, s_py, s_pz;
static float s_meters_to_world;

void vr_camera_compose_set_active_eye(float qx, float qy, float qz, float qw,
                                       float px, float py, float pz,
                                       float meters_to_world)
{
    s_qx = qx; s_qy = qy; s_qz = qz; s_qw = qw;
    s_px = px; s_py = py; s_pz = pz;
    s_meters_to_world = meters_to_world;
    s_active = true;
}

void vr_camera_compose_clear_active_eye(void)
{
    s_active = false;
}

bool vr_camera_compose_apply(float rot[3][3], float pos[3],
                              int *out_yaw, int *out_pitch, int *out_roll)
{
    if (!s_active) return false;
    vr_camera_compose(rot, pos, s_qx, s_qy, s_qz, s_qw, s_px, s_py, s_pz,
                       s_meters_to_world, rot, pos);
    vr_camera_angles_from_rotation(rot, out_yaw, out_pitch, out_roll);
    return true;
}

void vr_eye_sky_halfplane(const float base_rot[3][3],
                          float tan_left, float tan_right,
                          float tan_up, float tan_down,
                          float *out_a, float *out_b, float *out_c)
{
    /* World up (+Z) in OpenXR eye space: column 2 of the world-to-eye
     * rotation vr_camera_view_basis() builds (the projection path's own
     * view matrix, so the sky split and the geometry agree exactly). */
    float combined_rot[3][3], eye_pos[3];
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    vr_camera_view_basis(base_rot, zero, combined_rot, eye_pos);
    float ux = combined_rot[0][2], uy = combined_rot[1][2], uz = combined_rot[2][2];
    /* ray(x, y) = (X, Y, -1), X = x*(r-l)/2 + (r+l)/2, Y = y*(u-d)/2 + (u+d)/2;
     * dot(ray, up) = a*x + b*y + c. */
    *out_a = ux * (tan_right - tan_left) * 0.5f;
    *out_b = uy * (tan_up - tan_down) * 0.5f;
    *out_c = ux * (tan_right + tan_left) * 0.5f + uy * (tan_up + tan_down) * 0.5f - uz;
}

bool vr_eye_view_to_ndc(float vx, float vy, float vz,
                        float tan_left, float tan_right,
                        float tan_up, float tan_down,
                        float near_plane, float far_plane,
                        float *out_x, float *out_y, float *out_z)
{
    if (!(vz > 0.0f)) return false;
    float X = vx / vz, Y = vy / vz;
    *out_x = (2.0f * X - (tan_right + tan_left)) / (tan_right - tan_left);
    *out_y = (2.0f * Y - (tan_up + tan_down)) / (tan_up - tan_down);
    *out_z = far_plane / (far_plane - near_plane) * (vz - near_plane) / vz;
    return true;
}
