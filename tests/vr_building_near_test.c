/* tests/vr_building_near_test.c
 *
 * Pure-C checks for vr_eye_corner_near_clipped() (vr_camera_compose.{h,c}):
 * DrawBuilding()'s per-corner near-plane flag in a VR eye. A polygon whose
 * four corners are all flagged is dropped before it reaches the GPU.
 *
 * Part 1: literal cases. The eye's near plane is 15.6 world units (5 cm at
 * ~312 units per metre). A corner 40 units ahead is visible to the eye but
 * below the flat window's 80-unit limit; one 10 units ahead and one behind
 * the eye are clipped; a corner well off to the side (outside the lateral
 * FOV) is not near-clipped, the GPU clips it laterally.
 *
 * Part 2: the expected flag is NOT taken from the function's formula. Each
 * corner is a world point projected the way the GPU projects the eye's world
 * geometry (vr_camera_view_basis() plus the 4x4 OpenXR projection, as
 * build_mvp_vr_eye_projection() builds it): clipped exactly when the clip-
 * space depth is negative or w <= 0. The function only gets the engine
 * camera-space point computed with vk dot products, like DrawBuilding().
 *
 * Part 3: a wall beside the car (corners 20..70 units ahead, inside the
 * eye's view per the true projection) is dropped by the flat 80-unit rule
 * and kept by the eye's rule. */
#include <math.h>
#include <stdio.h>
#include "vr_camera_compose.h"

static int g_failures = 0;
static int g_checks = 0;
static const float PI = 3.14159265f;

static void check(const char *label, int ok)
{
    g_checks++;
    if (!ok) {
        printf("FAIL: %s\n", label);
        g_failures++;
    }
}

/* Game camera rotation in the vk1..vk9 [row][col] convention: column 0 =
 * right, column 1 = up, column 2 = forward, in world space (+Z up). */
static void game_camera(float yaw, float pitch, float roll, float m[3][3])
{
    float fwd[3] = { cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), sinf(pitch) };
    float up0[3] = { -sinf(pitch) * cosf(yaw), -sinf(pitch) * sinf(yaw), cosf(pitch) };
    float rgt[3] = { up0[1] * fwd[2] - up0[2] * fwd[1],
                     up0[2] * fwd[0] - up0[0] * fwd[2],
                     up0[0] * fwd[1] - up0[1] * fwd[0] };
    float cr = cosf(roll), sr = sinf(roll);
    for (int i = 0; i < 3; i++) {
        m[i][0] = rgt[i] * cr + up0[i] * sr;
        m[i][1] = -rgt[i] * sr + up0[i] * cr;
        m[i][2] = fwd[i];
    }
}

/* Clip-space (z, w) of a world point through the eye's world-geometry MVP. */
static void true_clip_zw(const float rot[3][3], const float eye[3], const float t[4],
                         float n, float f, const float w[3], float *cz, float *cw)
{
    float R[3][3], pos[3];
    vr_camera_view_basis(rot, eye, R, pos);
    float V[16] = { 0 }, P[16] = { 0 }, M[16];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) V[c * 4 + r] = R[r][c];
        V[12 + r] = -(R[r][0] * pos[0] + R[r][1] * pos[1] + R[r][2] * pos[2]);
    }
    V[15] = 1.0f;
    P[0] = 2.0f / (t[1] - t[0]);
    P[8] = (t[1] + t[0]) / (t[1] - t[0]);
    P[5] = 2.0f / (t[2] - t[3]);
    P[9] = (t[2] + t[3]) / (t[2] - t[3]);
    P[10] = f / (n - f);
    P[11] = -1.0f;
    P[14] = f * n / (n - f);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += P[k * 4 + r] * V[c * 4 + k];
            M[c * 4 + r] = s;
        }
    *cz = M[2] * w[0] + M[6] * w[1] + M[10] * w[2] + M[14];
    *cw = M[3] * w[0] + M[7] * w[1] + M[11] * w[2] + M[15];
}

static int true_in_view(const float rot[3][3], const float eye[3], const float t[4],
                        float n, float f, const float w[3])
{
    float R[3][3], pos[3];
    (void)n; (void)f;
    vr_camera_view_basis(rot, eye, R, pos);
    float d[3] = { w[0] - pos[0], w[1] - pos[1], w[2] - pos[2] };
    float ex = R[0][0] * d[0] + R[0][1] * d[1] + R[0][2] * d[2];
    float ey = R[1][0] * d[0] + R[1][1] * d[1] + R[1][2] * d[2];
    float ez = R[2][0] * d[0] + R[2][1] * d[1] + R[2][2] * d[2];
    if (ez >= 0.0f) return 0;  /* OpenXR eye looks down -Z */
    float tx = ex / -ez, ty = ey / -ez;
    return tx >= t[0] && tx <= t[1] && ty >= t[3] && ty <= t[2];
}

/* Engine camera space, as DrawBuilding() computes it from vk1..vk9. */
static void cam_space(const float rot[3][3], const float eye[3], const float w[3], float v[3])
{
    float d[3] = { w[0] - eye[0], w[1] - eye[1], w[2] - eye[2] };
    for (int c = 0; c < 3; c++) v[c] = d[0] * rot[0][c] + d[1] * rot[1][c] + d[2] * rot[2][c];
}

static const float NEAR_WU = 15.6f, FAR_WU = 100000.0f;
static const float T_SYM[4] = { -0.733f, 0.733f, 0.733f, -0.733f };
/* A Quest/Index-like asymmetric left eye (l r u d). */
static const float T_ASYM[4] = { -1.39f, 0.84f, 0.97f, -1.19f };

static int clipped(float vx, float vy, float vz, const float t[4])
{
    return vr_eye_corner_near_clipped(vx, vy, vz, t[0], t[1], t[2], t[3], NEAR_WU, FAR_WU);
}

int main(void)
{
    /* Part 1: literal cases. */
    check("40 units ahead is not clipped (flat rule clips it)", clipped(0, 0, 40, T_SYM) == 0);
    check("79 units ahead is not clipped", clipped(5, -3, 79, T_SYM) == 0);
    check("16 units ahead is not clipped", clipped(0, 0, 16, T_SYM) == 0);
    check("15 units ahead is clipped", clipped(0, 0, 15, T_SYM) == 1);
    check("10 units ahead is clipped", clipped(0, 0, 10, T_SYM) == 1);
    check("on the eye plane is clipped", clipped(3, 3, 0, T_SYM) == 1);
    check("behind the eye is clipped", clipped(0, 0, -5, T_SYM) == 1);
    check("far off to the side, 40 ahead, is not near-clipped", clipped(200, 0, 40, T_SYM) == 0);
    check("asymmetric FOV: 40 ahead is not clipped", clipped(-30, -20, 40, T_ASYM) == 0);
    check("asymmetric FOV: 12 ahead is clipped", clipped(-30, -20, 12, T_ASYM) == 1);

    /* Part 2: against the eye's world-geometry projection. */
    const float eye[3] = { 12000.0f, -3400.0f, 850.0f };
    const float *tans[2] = { T_SYM, T_ASYM };
    int mism = 0, nClipped = 0, nKept = 0;
    for (int ti = 0; ti < 2; ti++)
        for (int yi = 0; yi < 8; yi++)
            for (int pi = -2; pi <= 2; pi++) {
                float rot[3][3];
                game_camera(yi * 45.0f * PI / 180.0f, pi * 15.0f * PI / 180.0f,
                            (pi % 2) * 0.1f, rot);
                for (int k = 0; k < 400; k++) {
                    /* Points around the eye, depth -40..+200 along forward. */
                    float fx = (float)((k * 37) % 41 - 20) * 6.0f;
                    float fy = (float)((k * 53) % 29 - 14) * 6.0f;
                    float fz = -40.0f + (float)((k * 7919) % 241);
                    float w[3];
                    for (int i = 0; i < 3; i++)
                        w[i] = eye[i] + fx * rot[i][0] + fy * rot[i][1] + fz * rot[i][2];
                    float v[3], cz, cw;
                    cam_space(rot, eye, w, v);
                    true_clip_zw(rot, eye, tans[ti], NEAR_WU, FAR_WU, w, &cz, &cw);
                    int expect = (cw <= 0.0f) || (cz < 0.0f);
                    /* Skip points within float noise of the near plane. */
                    if (fabsf(v[2] - NEAR_WU) < 0.01f || fabsf(v[2]) < 0.01f) continue;
                    int got = clipped(v[0], v[1], v[2], tans[ti]);
                    if (got != expect) mism++;
                    if (expect) nClipped++; else nKept++;
                }
            }
    check("matches the eye MVP's near clip for every sampled point", mism == 0);
    check("sample covers clipped points", nClipped > 1000);
    check("sample covers kept points", nKept > 1000);

    /* Part 3: a wall beside the car, every corner 20..70 units ahead. */
    {
        float rot[3][3];
        game_camera(30.0f * PI / 180.0f, 0.0f, 0.0f, rot);
        const float local[4][3] = {  /* right, up, forward in camera space */
            { 25.0f, -10.0f, 20.0f }, { 25.0f, 30.0f, 20.0f },
            { 25.0f, 30.0f, 70.0f },  { 25.0f, -10.0f, 70.0f },
        };
        int flatAll = 1, eyeAll = 1, anyInView = 0;
        for (int c = 0; c < 4; c++) {
            float w[3], v[3];
            for (int i = 0; i < 3; i++)
                w[i] = eye[i] + local[c][0] * rot[i][0] + local[c][1] * rot[i][1]
                     + local[c][2] * rot[i][2];
            cam_space(rot, eye, w, v);
            if (!((int)v[2] < 80)) flatAll = 0;   /* DrawBuilding()'s flat rule */
            if (!clipped(v[0], v[1], v[2], T_SYM)) eyeAll = 0;
            if (true_in_view(rot, eye, T_SYM, NEAR_WU, FAR_WU, w)) anyInView = 1;
        }
        check("wall beside the car is in the eye's view", anyInView == 1);
        check("flat rule drops the whole wall", flatAll == 1);
        check("eye rule keeps the wall", eyeAll == 0);
    }

    printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
