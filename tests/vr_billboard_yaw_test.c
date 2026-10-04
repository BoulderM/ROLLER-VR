/* tests/vr_billboard_yaw_test.c
 *
 * Pure-C checks for the VR scenery billboard yaw (vr_billboard_yaw_deg() and
 * vr_camera_compose_base_view(), vr_camera_compose.{h,c}).
 *
 * Trees and advert balloons are flat cards (building plans 9, 10, 15: four
 * corners in the plan's Y-Z plane). building.c turns them by a yaw before
 * placing them: the flat game uses the camera heading, worlddirn, which in a
 * VR eye is the head's yaw (calculateview() writes the composed angles back),
 * so the cards spun in place as the head turned.
 *
 * Nothing here takes its expected value from the formula under test:
 * - the camera rotation is calculatetransform()'s vk1..vk9 formula
 *   (transfrm.c), copied with plain trig in place of the tsin/tcos tables;
 * - card corners use building_transform_plan_coords()'s rotation for zero
 *   pitch and roll, and "faces the viewer" is checked on the card's normal
 *   from the cross product of its corners, not on the yaw angle;
 * - on-screen drift is measured through the eye's world-geometry projection
 *   (vr_camera_view_basis() plus a 4x4 OpenXR asymmetric projection, the
 *   matrices build_mvp_vr_eye_projection() uses), as the GPU draws the card.
 *
 * Part 1 confirms the bug: with the head-composed yaw (what worlddirn holds in
 * a VR eye) the card's corners move on screen against their own earlier
 * world position as the head turns, with the world frozen.
 * Part 2 checks the fix: the yaw from vr_billboard_yaw_deg() at the base
 * camera position from vr_camera_compose_base_view() keeps every corner fixed
 * in the world (zero drift) for any head pose, turns the card face-on to the
 * viewer, and equals the flat game's yaw when the camera looks straight at it. */
#include <math.h>
#include <stdio.h>
#include <string.h>
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

/* calculatetransform(-1, dir, elev, tilt, ...)'s vk1..vk9, dir/elev/tilt as
 * 14-bit angles, read as {{vk1,vk2,vk3},{vk4,vk5,vk6},{vk7,vk8,vk9}}. */
static void calc_transform(int dir, int elev, int tilt, float m[3][3])
{
    float d = (float)dir * 2.0f * PI / 16384.0f;
    float e = (float)elev * 2.0f * PI / 16384.0f;
    float t = (float)tilt * 2.0f * PI / 16384.0f;
    float v10 = cosf(d) * sinf(e), v11 = sinf(d) * sinf(e);
    m[0][0] = sinf(d) * cosf(t) - v10 * sinf(t);
    m[0][1] = -sinf(d) * sinf(t) - v10 * cosf(t);
    m[0][2] = cosf(d) * cosf(e);
    m[1][0] = -cosf(d) * cosf(t) - v11 * sinf(t);
    m[1][1] = cosf(d) * sinf(t) - v11 * cosf(t);
    m[1][2] = sinf(d) * cosf(e);
    m[2][0] = cosf(e) * sinf(t);
    m[2][1] = cosf(e) * cosf(t);
    m[2][2] = sinf(e);
}

/* tree_coords (plans.c): the tree card. */
static const float TREE[4][3] = {
    { 0.0f, 372.30099f, 0.003f }, { 0.0f, -372.30099f, 0.003f },
    { 0.0f, -372.30099f, 1712.558f }, { 0.0f, 372.30099f, 1712.558f },
};

/* building_transform_plan_coords() for zero pitch and roll: matrix rows
 * (cos y, -sin y, 0), (sin y, cos y, 0), (0, 0, 1), then the placed origin. */
static void place_card(const float org[3], float yawDeg, float out[4][3])
{
    float y = yawDeg * 0.0174532925199f;
    for (int i = 0; i < 4; i++) {
        out[i][0] = TREE[i][0] * cosf(y) - TREE[i][1] * sinf(y) + org[0];
        out[i][1] = TREE[i][0] * sinf(y) + TREE[i][1] * cosf(y) + org[1];
        out[i][2] = TREE[i][2] + org[2];
    }
}

/* The eye's world-geometry projection (see vr_effect_projection_test.c). */
static int true_ndc(const float rot[3][3], const float eye[3], const float t[4],
                    const float w[3], float out[2])
{
    const float n = 15.6f, f = 100000.0f;
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
    return 1;
}

static float angle_diff_deg(float a, float b)
{
    float d = fmodf(a - b, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return fabsf(d);
}

/* Head pose: yaw about OpenXR +Y, then a small pitch about +X, plus a lean. */
static void head_pose(float yawDeg, float pitchDeg, float q[4], float p[3])
{
    float y = yawDeg * PI / 180.0f, x = pitchDeg * PI / 180.0f;
    float cy = cosf(y * 0.5f), sy = sinf(y * 0.5f), cx = cosf(x * 0.5f), sx = sinf(x * 0.5f);
    q[0] = cy * sx; q[1] = sy * cx; q[2] = -sy * sx; q[3] = cy * cx;  /* q_yaw * q_pitch */
    p[0] = 0.05f * sinf(y); p[1] = 0.02f; p[2] = -0.03f * cosf(y);   /* metres */
}

int main(void)
{
    char label[256];
    const float t[4] = { -0.733f, 0.733f, 0.733f, -0.733f };  /* l r u d, the fake head's FOV */
    const float M2W = 312.0f;

    /* --- Flat agreement: camera looking straight at the object ------------- */
    for (int dir = 0; dir < 16384; dir += 731) {
        float rot[3][3];
        calc_transform(dir, 0, 0, rot);
        const float cam[3] = { 5000.0f, -2000.0f, 300.0f };
        float obj[3] = { cam[0] + 2500.0f * rot[0][2], cam[1] + 2500.0f * rot[1][2], 0.0f };
        /* The camera heading in degrees; building.c's flat yaw is the same
         * value truncated to whole degrees (integer 360 * worlddirn / 0x3FFF). */
        float heading = (float)dir * 360.0f / 16384.0f;
        float flat = (float)(360 * dir / 0x3FFF);
        float yaw = vr_billboard_yaw_deg(obj[0], obj[1], cam[0], cam[1], -999.0f);
        snprintf(label, sizeof label, "dir %d: dead-ahead yaw %.3f vs heading %.3f", dir, yaw, heading);
        check(label, angle_diff_deg(yaw, heading) < 0.01f);
        snprintf(label, sizeof label, "dir %d: dead-ahead yaw %.3f vs flat %.0f", dir, yaw, flat);
        check(label, angle_diff_deg(yaw, flat) < 1.0f);
    }
    check("viewer straight above: fallback",
          vr_billboard_yaw_deg(10.0f, 20.0f, 10.0f, 20.0f, 42.0f) == 42.0f);

    /* --- Base view plumbing: outside the active-eye bracket it reports
     * nothing; inside it reports the UNcomposed camera for any head pose. */
    {
        float pos[3]; int yaw;
        vr_camera_compose_clear_active_eye();
        check("base view outside the eye bracket: false", !vr_camera_compose_base_view(pos, &yaw));
    }

    /* --- Head sweep over a frozen world -------------------------------------- */
    const int baseDirs[3] = { 0, 3000, 11000 };
    const float objOff[6][2] = {  /* in the base camera's frame: forward, right (world units) */
        { 3000.0f, 0.0f }, { 2500.0f, 1500.0f }, { 2500.0f, -1500.0f },
        { 1200.0f, 900.0f }, { 6000.0f, -2500.0f }, { 800.0f, -300.0f },
    };
    float oldSum = 0.0f, oldMax = 0.0f, newMax = 0.0f, newWorldMax = 0.0f, worstFaceDot = 1.0f;
    int oldN = 0, newN = 0;
    for (int b = 0; b < 3; b++) {
        float base[3][3];
        calc_transform(baseDirs[b], 0, 0, base);
        const float camPos[3] = { -4200.0f, 7300.0f, 260.0f };
        for (int o = 0; o < 6; o++) {
            float org[3];
            for (int i = 0; i < 2; i++)
                org[i] = camPos[i] + objOff[o][0] * base[i][2] + objOff[o][1] * base[i][0];
            org[2] = 0.0f;

            float refOld[4][3], refNew[4][3];
            int haveRef = 0;
            for (int hy = -60; hy <= 60; hy += 5) {
                float q[4], p[3], rot[3][3], pos[3];
                head_pose((float)hy, 0.3f * (float)hy, q, p);
                /* The VR eye: compose the head into the base camera the way
                 * calculateview()'s post-step does. */
                memcpy(rot, base, sizeof rot);
                memcpy(pos, camPos, sizeof pos);
                int composedYaw, cp, cr;
                vr_camera_compose_set_active_eye(q[0], q[1], q[2], q[3], p[0], p[1], p[2], M2W);
                check("compose apply in the eye bracket", vr_camera_compose_apply(rot, pos, &composedYaw, &cp, &cr));
                float bpos[3]; int byaw;
                int haveBase = vr_camera_compose_base_view(bpos, &byaw);
                vr_camera_compose_clear_active_eye();
                check("base view in the eye bracket", haveBase);
                snprintf(label, sizeof label, "base view pos (%g,%g,%g) is the uncomposed camera",
                         bpos[0], bpos[1], bpos[2]);
                check(label, fabsf(bpos[0] - camPos[0]) < 1e-3f && fabsf(bpos[1] - camPos[1]) < 1e-3f
                             && fabsf(bpos[2] - camPos[2]) < 1e-3f);
                snprintf(label, sizeof label, "base view yaw %d vs base dir %d", byaw, baseDirs[b]);
                check(label, angle_diff_deg((float)byaw * 360.0f / 16384.0f,
                                            (float)baseDirs[b] * 360.0f / 16384.0f) < 0.1f);

                /* Old: the flat formula fed the composed (head) yaw. */
                float cOld[4][3], cNew[4][3];
                place_card(org, (float)(360 * composedYaw / 0x3FFF), cOld);
                /* New: face the base camera position. */
                place_card(org, vr_billboard_yaw_deg(org[0], org[1], bpos[0], bpos[1],
                                                     (float)(360 * byaw / 0x3FFF)), cNew);
                if (!haveRef) {
                    memcpy(refOld, cOld, sizeof refOld);
                    memcpy(refNew, cNew, sizeof refNew);
                    haveRef = 1;
                }

                /* Face-on: card normal (corner cross product) vs the
                 * horizontal direction to the viewer. */
                {
                    float e1[3], e2[3], nrm[3];
                    for (int i = 0; i < 3; i++) { e1[i] = cNew[1][i] - cNew[0][i]; e2[i] = cNew[3][i] - cNew[0][i]; }
                    nrm[0] = e1[1] * e2[2] - e1[2] * e2[1];
                    nrm[1] = e1[2] * e2[0] - e1[0] * e2[2];
                    nrm[2] = e1[0] * e2[1] - e1[1] * e2[0];
                    float dx = bpos[0] - org[0], dy = bpos[1] - org[1];
                    float dot = (nrm[0] * dx + nrm[1] * dy)
                              / (sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]) * sqrtf(dx * dx + dy * dy));
                    if (fabsf(dot) < worstFaceDot) worstFaceDot = fabsf(dot);
                }

                /* Drift on screen against the card's own first-seen corners,
                 * through this head pose's eye projection, while in view. */
                float ctr[3] = { org[0], org[1], 856.0f }, cn[2];
                if (!true_ndc(rot, pos, t, ctr, cn) || fabsf(cn[0]) > 1.0f || fabsf(cn[1]) > 1.0f)
                    continue;
                float dOld = 0.0f, dNew = 0.0f, wNew = 0.0f;
                int ok = 1;
                for (int k = 0; k < 4 && ok; k++) {
                    float a[2], r[2];
                    if (!true_ndc(rot, pos, t, cOld[k], a) || !true_ndc(rot, pos, t, refOld[k], r)) { ok = 0; break; }
                    float e = hypotf(a[0] - r[0], a[1] - r[1]); if (e > dOld) dOld = e;
                    if (!true_ndc(rot, pos, t, cNew[k], a) || !true_ndc(rot, pos, t, refNew[k], r)) { ok = 0; break; }
                    e = hypotf(a[0] - r[0], a[1] - r[1]); if (e > dNew) dNew = e;
                    for (int i = 0; i < 3; i++) {
                        float m = fabsf(cNew[k][i] - refNew[k][i]); if (m > wNew) wNew = m;
                    }
                }
                if (!ok) continue;
                oldN++; oldSum += dOld; if (dOld > oldMax) oldMax = dOld;
                newN++; if (dNew > newMax) newMax = dNew;
                if (wNew > newWorldMax) newWorldMax = wNew;
                snprintf(label, sizeof label, "base %d obj %d head %+d: new drift %.6f NDC", baseDirs[b], o, hy, dNew);
                check(label, dNew < 1e-4f);
            }
        }
    }
    printf("billboard head sweep (+-60 deg yaw, +-18 deg pitch, lean), %d in-view samples:\n", oldN);
    printf("  head-yaw billboard (old): drift vs first-seen corners mean %.4f max %.4f NDC\n",
           oldN ? oldSum / (float)oldN : 0.0f, oldMax);
    printf("  base-camera-facing (new): drift max %.6f NDC, corner world move max %.4f units, "
           "worst |cos| card normal vs viewer %.6f\n", newMax, newWorldMax, worstFaceDot);
    check("enough in-view samples", oldN >= 60);
    check("the old head-yaw card visibly drifts (bug reproduced)", oldMax > 0.05f);
    check("new card corners fixed in the world", newWorldMax < 1e-3f);
    check("new card face-on to the viewer", worstFaceDot > 0.9999f);

    if (g_failures) {
        printf("vr_billboard_yaw_test: %d FAILED\n", g_failures);
        return 1;
    }
    printf("vr_billboard_yaw_test: all passed\n");
    return 0;
}
