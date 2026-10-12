/* tests/vr_menu_quad_yaw_test.c
 *
 * Pure-C checks for the menu quad's anchor math (vr_quad_yaw_from_quat() and
 * vr_quad_yaw_anchor(), vr_quad_math.{h,c}).
 *
 * Expected values never come from the formula under test: the head's forward
 * vector is computed here by rotating (0,0,-1) with the full quaternion
 * sandwich product q*v*conj(q), and "yaw" is defined from that vector. */
#include "vr_quad_math.h"

#include <math.h>
#include <stdio.h>

#define PI_F 3.14159265f

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

static void quat_from_yaw(float yaw_rad, float *x, float *y, float *z, float *w)
{
    *x = 0; *z = 0;
    *y = sinf(yaw_rad * 0.5f);
    *w = cosf(yaw_rad * 0.5f);
}

/* Hamilton product a*b, (x,y,z,w). */
static void quat_mul(const float a[4], const float b[4], float out[4])
{
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

/* v' = q * (v,0) * conj(q): the rotation the quaternion stands for. */
static void quat_rotate(const float q[4], const float v[3], float out[3])
{
    float p[4] = { v[0], v[1], v[2], 0.0f }, qc[4] = { -q[0], -q[1], -q[2], q[3] }, t[4], r[4];
    quat_mul(q, p, t);
    quat_mul(t, qc, r);
    out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
}

/* Reference yaw: angle about +Y (CCW from above, 0 = -Z) of the rotated
 * forward vector's horizontal part. Forward (-sin y, 0, -cos y) <=> yaw y. */
static float reference_yaw(const float q[4])
{
    const float fwd0[3] = { 0.0f, 0.0f, -1.0f };
    float f[3];
    quat_rotate(q, fwd0, f);
    return atan2f(-f[0], -f[2]);
}

static float angle_diff_deg(float a_deg, float b_deg)
{
    float d = fmodf(a_deg - b_deg + 540.0f, 360.0f) - 180.0f;
    return fabsf(d);
}

int main(void)
{
    /* Identity (facing -Z, OpenXR convention): yaw 0. */
    CHECK(fabsf(vr_quad_yaw_from_quat(0, 0, 0, 1)) < 1e-4f);

    /* Sign: 90 deg about +Y turns -Z into -X (CCW from above), i.e. the head
     * looks left. The reference agrees, and the function must too. */
    {
        float q[4];
        quat_from_yaw(PI_F * 0.5f, &q[0], &q[1], &q[2], &q[3]);
        const float fwd0[3] = { 0.0f, 0.0f, -1.0f };
        float f[3];
        quat_rotate(q, fwd0, f);
        CHECK(fabsf(f[0] + 1.0f) < 1e-4f && fabsf(f[2]) < 1e-4f);
        float got = vr_quad_yaw_from_quat(q[0], q[1], q[2], q[3]) * 180.0f / PI_F;
        printf("yaw(+90 deg about +Y) = %.2f deg (expected +90.00)\n", got);
        CHECK(fabsf(got - 90.0f) < 0.5f);
    }

    /* Round trip through quat_from_yaw across the circle. */
    for (float deg = -179.0f; deg <= 179.0f; deg += 17.0f) {
        float qx, qy, qz, qw;
        quat_from_yaw(deg * PI_F / 180.0f, &qx, &qy, &qz, &qw);
        float back = vr_quad_yaw_from_quat(qx, qy, qz, qw) * 180.0f / PI_F;
        if (fabsf(back - deg) >= 0.5f)
            printf("  round trip %.1f deg -> %.2f deg\n", deg, back);
        CHECK(fabsf(back - deg) < 0.5f);
    }

    /* Pitch and roll must not perturb the extracted yaw: yaw0 * pitch, and
     * yaw0 * pitch * roll (intrinsic: yaw, then pitch about the new X, then
     * roll about the new Z), against the reference yaw of the same quaternion. */
    for (float yaw_deg = -170.0f; yaw_deg <= 170.0f; yaw_deg += 40.0f) {
        for (float pitch_deg = -60.0f; pitch_deg <= 60.0f; pitch_deg += 30.0f) {
            float y[4], p[4] = { sinf(pitch_deg * PI_F / 360.0f), 0, 0, cosf(pitch_deg * PI_F / 360.0f) };
            float rl[4] = { 0, 0, sinf(25.0f * PI_F / 360.0f), cosf(25.0f * PI_F / 360.0f) };
            float yp[4], ypr[4];
            quat_from_yaw(yaw_deg * PI_F / 180.0f, &y[0], &y[1], &y[2], &y[3]);
            quat_mul(y, p, yp);
            quat_mul(yp, rl, ypr);
            float got_yp = vr_quad_yaw_from_quat(yp[0], yp[1], yp[2], yp[3]) * 180.0f / PI_F;
            float got_ypr = vr_quad_yaw_from_quat(ypr[0], ypr[1], ypr[2], ypr[3]) * 180.0f / PI_F;
            float ref_ypr = reference_yaw(ypr) * 180.0f / PI_F;
            if (angle_diff_deg(got_yp, yaw_deg) >= 1.0f || angle_diff_deg(got_ypr, ref_ypr) >= 1.0f)
                printf("  yaw %.0f pitch %.0f: yaw*pitch -> %.2f, yaw*pitch*roll -> %.2f (ref %.2f)\n",
                       yaw_deg, pitch_deg, got_yp, got_ypr, ref_ypr);
            CHECK(angle_diff_deg(got_yp, yaw_deg) < 1.0f);
            CHECK(angle_diff_deg(got_ypr, ref_ypr) < 1.0f);
        }
    }

    /* The anchor: placed `distance` ahead along the head's horizontal forward,
     * at head height, and its own orientation's forward (-Z) is that same
     * direction (quad upright, facing the viewer). */
    for (float yaw_deg = -170.0f; yaw_deg <= 170.0f; yaw_deg += 20.0f) {
        float head_q[4], p[4] = { sinf(20.0f * PI_F / 360.0f), 0, 0, cosf(20.0f * PI_F / 360.0f) }, hq[4];
        quat_from_yaw(yaw_deg * PI_F / 180.0f, &head_q[0], &head_q[1], &head_q[2], &head_q[3]);
        quat_mul(head_q, p, hq); /* head pitched 20 deg up */
        const float head_pos[3] = { 0.3f, 1.6f, -0.2f };
        const float fwd0[3] = { 0.0f, 0.0f, -1.0f };
        float f[3];
        quat_rotate(hq, fwd0, f);
        float hlen = sqrtf(f[0] * f[0] + f[2] * f[2]);
        float expect[3] = { head_pos[0] + 2.0f * f[0] / hlen, head_pos[1], head_pos[2] + 2.0f * f[2] / hlen };

        float yaw = vr_quad_yaw_from_quat(hq[0], hq[1], hq[2], hq[3]);
        float pos[3], q[4];
        vr_quad_yaw_anchor(head_pos, yaw, 2.0f, pos, q);
        float err = fabsf(pos[0] - expect[0]) + fabsf(pos[1] - expect[1]) + fabsf(pos[2] - expect[2]);
        float qf[3];
        quat_rotate(q, fwd0, qf);
        float ferr = fabsf(qf[0] - f[0] / hlen) + fabsf(qf[1]) + fabsf(qf[2] - f[2] / hlen);
        if (err >= 1e-3f || ferr >= 1e-3f)
            printf("  anchor yaw %.0f: pos (%.3f,%.3f,%.3f) expected (%.3f,%.3f,%.3f); quad fwd (%.3f,%.3f,%.3f)\n",
                   yaw_deg, pos[0], pos[1], pos[2], expect[0], expect[1], expect[2], qf[0], qf[1], qf[2]);
        CHECK(err < 1e-3f);
        CHECK(ferr < 1e-3f);
    }

    if (g_failures) { printf("%d check(s) FAILED\n", g_failures); return 1; }
    printf("vr_menu_quad_yaw_test: ALL PASS\n");
    return 0;
}
