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
    /* Forward = column 2, up = column 1 -- this header's own
     * vr_camera_compose() derivation comment above establishes col 2 as
     * this engine's world-forward column for a local-to-world matrix in
     * this [row][col] convention; vr_runtime.c's build_mvp_vr_eye_
     * projection() (via vr_camera_view_basis() below) relies on the same
     * convention. */
    float fx = rot[0][2], fy = rot[1][2], fz = rot[2][2];
    float ux = rot[0][1], uy = rot[1][1], uz = rot[2][1];

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

    /* Bank/roll: rotate the up vector by -azimuth then by -elevation, same
     * as getworldangles()'s final four lines, using our up vector directly
     * in place of its separately-constructed "roll vector" (ours is already
     * in world coordinates, needing no further chunk transform). */
    float rollRotatedX =  ux * cosAz + uy * sinAz;
    float rollRotatedY = -ux * sinAz + uy * cosAz;
    float finalX = rollRotatedX * sinEl - uz * cosEl;
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
