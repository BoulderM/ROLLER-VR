/* tests/vr_effect_projection_test.c
 *
 * Pure-C checks for vr_eye_view_to_ndc() (vr_camera_compose.{h,c}): where a
 * VR eye places a screen-space effect (smoke, sparks, name tag) that the game
 * projects on the CPU from engine camera space.
 *
 * The expected value is NOT taken from the function's own formula. Each case
 * builds an eye camera and a world point, then projects the world point the
 * way the GPU projects the eye's world geometry: vr_camera_view_basis() for
 * the world-to-eye view matrix and a 4x4 OpenXR asymmetric projection from
 * the FOV tangents (the matrix build_mvp_vr_eye_projection() uses), multiplied
 * out and divided by w. The function under test only gets the engine
 * camera-space point (vx, vy, vz), computed with vk dot products the way
 * DisplayCarSmoke() does.
 *
 * Part 1 confirms the bug: the flat-window placement DisplayCarSmoke() used
 * (VIEWDIST, xbase, ybase, scr_size, winw/winh, measured in the VR attract
 * race: 200/159/115/64/320x200, and the 270/640x400 SVGA cockpit set) is far
 * from the eye's true projection, and its error changes with head pitch.
 * Part 2 checks the fix: vr_eye_view_to_ndc() lands on the true projection
 * (x, y and depth) for level, pitched, rolled and asymmetric-FOV eyes, and a
 * sprite offset in camera space lands on the projected offset world point. */
#include <math.h>
#include <stdio.h>
#include "vr_camera_compose.h"

static int g_failures = 0;
static const float PI = 3.14159265f;

static void check(const char *label, int ok)
{
    if (!ok) {
        printf("FAIL: %s\n", label);
        g_failures++;
    }
}

/* Game camera rotation in the vk1..vk9 [row][col] convention: column 0 =
 * right, column 1 = up, column 2 = forward, in world space (+Z up). Yaw about
 * world Z, then pitch (positive = look up), then roll about forward. */
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

/* The eye's world-geometry projection: view matrix from vr_camera_view_basis()
 * plus an OpenXR asymmetric projection (depth [0, 1], w = -z_eye), as a
 * column-major 4x4 product, applied to a world point. */
static int true_ndc(const float rot[3][3], const float eye[3], const float t[4] /* l r u d */,
                    float n, float f, const float w[3], float out[3])
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
    float clip[4];
    for (int r = 0; r < 4; r++)
        clip[r] = M[r] * w[0] + M[4 + r] * w[1] + M[8 + r] * w[2] + M[12 + r];
    if (clip[3] <= 0.0f) return 0;
    out[0] = clip[0] / clip[3];
    out[1] = clip[1] / clip[3];
    out[2] = clip[2] / clip[3];
    return 1;
}

/* Engine camera space, as DisplayCarSmoke() computes it from vk1..vk9. */
static void cam_space(const float rot[3][3], const float eye[3], const float w[3], float v[3])
{
    float d[3] = { w[0] - eye[0], w[1] - eye[1], w[2] - eye[2] };
    for (int c = 0; c < 3; c++) v[c] = d[0] * rot[0][c] + d[1] * rot[1][c] + d[2] * rot[2][c];
}

/* DisplayCarSmoke()'s flat-window placement (sx, sy as integers, then
 * game_render_quad_screen()'s x/winw, y/winh conversion to NDC). */
static void flat_ndc(const float v[3], int viewdist, int xbase, int ybase, int scr_size,
                     int winw, int winh, float out[2])
{
    double invZ = 1.0 / v[2];
    int sx = (int)lround((double)viewdist * v[0] * invZ + xbase) * scr_size >> 6;
    int sy = (scr_size * (199 - (int)lround((double)viewdist * v[1] * invZ + ybase))) >> 6;
    out[0] = (float)sx / (float)winw * 2.0f - 1.0f;
    out[1] = 1.0f - (float)sy / (float)winh * 2.0f;
}

/* A world point at distance dist along the direction that is (az, el) degrees
 * off the camera's forward axis (az right, el up, in the camera's frame). */
static void point_off_axis(const float rot[3][3], const float eye[3], float az, float el,
                           float dist, float w[3])
{
    float a = az * PI / 180.0f, e = el * PI / 180.0f;
    float cx = cosf(e) * sinf(a), cy = sinf(e), cz = cosf(e) * cosf(a);
    for (int i = 0; i < 3; i++)
        w[i] = eye[i] + dist * (cx * rot[i][0] + cy * rot[i][1] + cz * rot[i][2]);
}

static const float NEAR_WU = 15.6f, FAR_WU = 100000.0f;

static void run_case(const char *name, float yaw, float pitch, float roll, const float t[4])
{
    float rot[3][3];
    game_camera(yaw, pitch, roll, rot);
    const float eye[3] = { 12000.0f, -3400.0f, 850.0f };
    char label[200];
    int n = 0;
    for (int az = -40; az <= 40; az += 10)
        for (int el = -30; el <= 30; el += 10)
            for (int k = 0; k < 3; k++) {
                float dist = (k == 0) ? 60.0f : (k == 1) ? 1500.0f : 40000.0f;
                float w[3], v[3], tr[3], x, y, z;
                point_off_axis(rot, eye, (float)az, (float)el, dist, w);
                if (!true_ndc(rot, eye, t, NEAR_WU, FAR_WU, w, tr)) continue;
                cam_space(rot, eye, w, v);
                int ok = vr_eye_view_to_ndc(v[0], v[1], v[2], t[0], t[1], t[2], t[3],
                                            NEAR_WU, FAR_WU, &x, &y, &z);
                snprintf(label, sizeof label, "%s: az%+d el%+d d%.0f projects", name, az, el, dist);
                check(label, ok);
                if (!ok) continue;
                n++;
                snprintf(label, sizeof label, "%s: az%+d el%+d d%.0f x %g vs %g, y %g vs %g",
                         name, az, el, dist, x, tr[0], y, tr[1]);
                check(label, fabsf(x - tr[0]) < 1e-4f && fabsf(y - tr[1]) < 1e-4f);
                snprintf(label, sizeof label, "%s: az%+d el%+d d%.0f depth %.7f vs %.7f",
                         name, az, el, dist, z, tr[2]);
                check(label, fabsf(z - tr[2]) < 1e-4f);  /* float MVP at world coords ~1e4 */

                /* A sprite corner: offset by s along the camera's right and up
                 * axes in camera space must land on the projected offset world
                 * point (sizes scale with the eye's FOV, not the flat window's). */
                float s = 0.1f * dist, wc[3], tc[3], cx, cy, cz;
                for (int i = 0; i < 3; i++) wc[i] = w[i] + s * rot[i][0] + s * rot[i][1];
                if (true_ndc(rot, eye, t, NEAR_WU, FAR_WU, wc, tc)
                    && vr_eye_view_to_ndc(v[0] + s, v[1] + s, v[2], t[0], t[1], t[2], t[3],
                                          NEAR_WU, FAR_WU, &cx, &cy, &cz)) {
                    snprintf(label, sizeof label, "%s: az%+d el%+d d%.0f sprite corner", name, az, el, dist);
                    check(label, fabsf(cx - tc[0]) < 1e-4f && fabsf(cy - tc[1]) < 1e-4f);
                }
            }
    snprintf(label, sizeof label, "%s: enough points projected", name);
    check(label, n > 100);
}

int main(void)
{
    /* --vr-fake-head's symmetric 42-degree FOV and an asymmetric, HMD-like one. */
    float ts = tanf(0.733f);
    const float sym[4]  = { -ts, ts, ts, -ts };
    const float asym[4] = { tanf(-0.87f), tanf(0.75f), tanf(0.82f), tanf(-0.95f) };
    const float eye[3] = { 0.0f, 0.0f, 0.0f };

    /* ---- Part 1: the flat-window placement is not the eye's projection ---- */
    {
        /* Same world point, a puff 1500 units ahead, 20 deg right and 10 deg
         * up of a level camera; the eye pitched by -8.6..+8.6 deg (the fake
         * head's range). */
        float rot0[3][3], w[3];
        game_camera(0.0f, 0.0f, 0.0f, rot0);
        point_off_axis(rot0, eye, 20.0f, 10.0f, 1500.0f, w);
        float errMin = 1e9f, errMax = 0.0f, yFlatMin = 1e9f, yFlatMax = -1e9f, yTrueMin = 1e9f, yTrueMax = -1e9f;
        for (int p = -86; p <= 86; p += 2) {
            float rot[3][3], v[3], tr[3], fl[2];
            game_camera(0.0f, (float)p * 0.1f * PI / 180.0f, 0.0f, rot);
            if (!true_ndc(rot, eye, sym, NEAR_WU, FAR_WU, w, tr)) continue;
            cam_space(rot, eye, w, v);
            flat_ndc(v, 200, 159, 115, 64, 320, 200, fl);
            float err = hypotf(fl[0] - tr[0], fl[1] - tr[1]);
            if (err < errMin) errMin = err;
            if (err > errMax) errMax = err;
            if (fl[1] < yFlatMin) yFlatMin = fl[1];
            if (fl[1] > yFlatMax) yFlatMax = fl[1];
            if (tr[1] < yTrueMin) yTrueMin = tr[1];
            if (tr[1] > yTrueMax) yTrueMax = tr[1];
        }
        printf("flat VIEWDIST 200, 320x200 vs eye (42 deg): placement error %.3f..%.3f NDC; "
               "over a 17.2 deg pitch sweep the effect moves %.3f NDC vertically, its world point %.3f (%.2fx)\n",
               errMin, errMax, yFlatMax - yFlatMin, yTrueMax - yTrueMin,
               (yFlatMax - yFlatMin) / (yTrueMax - yTrueMin));
        check("flat placement is off the eye projection by > 0.05 NDC", errMin > 0.05f);
        check("flat placement moves > 1.5x faster than the world under pitch",
              (yFlatMax - yFlatMin) > 1.5f * (yTrueMax - yTrueMin));

        /* SVGA cockpit set (VIEWDIST 270, scr_size 128, 640x400). */
        float rot[3][3], v[3], tr[3], fl[2];
        game_camera(0.0f, 0.0f, 0.0f, rot);
        true_ndc(rot, eye, sym, NEAR_WU, FAR_WU, w, tr);
        cam_space(rot, eye, w, v);
        flat_ndc(v, 270, 159, 115, 128, 640, 400, fl);
        printf("flat VIEWDIST 270, 640x400 vs eye (42 deg), level: placed (%.3f, %.3f), true (%.3f, %.3f): "
               "%.2fx horizontally, %.2fx vertically from the centre (plus the ybase offset)\n",
               fl[0], fl[1], tr[0], tr[1], fl[0] / tr[0], (fl[1] - 0.16f) / tr[1]);
        check("SVGA flat placement is off the eye projection by > 0.1 NDC",
              hypotf(fl[0] - tr[0], fl[1] - tr[1]) > 0.1f);

        /* Depth: the flat particle depth (SCENE_GPU_NEAR/FAR 10 / 2e7) vs the
         * eye's (15.6 / 1e5) for a puff 1000 units away. */
        float flatZ = (1000.0f - 10.0f) * (2e7f / (2e7f - 10.0f)) / 1000.0f;
        float vv[3] = { 0.0f, 0.0f, 1000.0f }, x, y, z;
        vr_eye_view_to_ndc(vv[0], vv[1], vv[2], sym[0], sym[1], sym[2], sym[3], NEAR_WU, FAR_WU, &x, &y, &z);
        float behind = NEAR_WU / (1.0f - flatZ * (FAR_WU - NEAR_WU) / FAR_WU);
        printf("depth of a puff at 1000 units: flat %.5f, eye %.5f; with the flat depth it hides behind "
               "anything nearer than %.0f units\n", flatZ, z, behind);
        check("flat particle depth is behind the eye's own depth", flatZ > z + 1e-3f);
    }

    /* ---- Part 2: vr_eye_view_to_ndc() is the eye's projection ---- */
    run_case("level, symmetric", 0.3f, 0.0f, 0.0f, sym);
    run_case("pitch down 15, symmetric", 1.1f, -15.0f * PI / 180.0f, 0.0f, sym);
    run_case("pitch up 20, roll 25, asymmetric", -2.0f, 20.0f * PI / 180.0f, 25.0f * PI / 180.0f, asym);
    run_case("roll -40, asymmetric", 2.5f, -5.0f * PI / 180.0f, -40.0f * PI / 180.0f, asym);

    {
        float x = 7.0f, y = 7.0f, z = 7.0f;
        check("behind the eye: returns false", !vr_eye_view_to_ndc(1.0f, 1.0f, -5.0f, sym[0], sym[1], sym[2], sym[3],
                                                                    NEAR_WU, FAR_WU, &x, &y, &z));
        check("behind the eye: writes nothing", x == 7.0f && y == 7.0f && z == 7.0f);
        /* Closed form: a point on the view axis of a symmetric eye is the centre. */
        vr_eye_view_to_ndc(0.0f, 0.0f, 500.0f, sym[0], sym[1], sym[2], sym[3], NEAR_WU, FAR_WU, &x, &y, &z);
        check("on-axis point is the NDC centre", fabsf(x) < 1e-6f && fabsf(y) < 1e-6f);
        /* Closed form: a point at the right edge of the FOV is x = 1. */
        vr_eye_view_to_ndc(ts * 500.0f, 0.0f, 500.0f, sym[0], sym[1], sym[2], sym[3], NEAR_WU, FAR_WU, &x, &y, &z);
        check("point at the FOV edge is x = 1", fabsf(x - 1.0f) < 1e-5f);
    }

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
