/* PROJECTS/ROLLER/vr_mvp_math_test.c
 *
 * Standalone numeric test for build_mvp_vr_eye()'s (vr_runtime.c) head-pose
 * composition math. Not wired into `zig build` (no unit-test harness exists
 * for this kind of pure-math C function in this codebase yet) -- build and
 * run directly:
 *
 *   gcc -Wall -Wextra vr_mvp_math_test.c -o /tmp/vr_mvp_math_test -lm && /tmp/vr_mvp_math_test
 *
 * Exits 0 and prints "ALL PASS" if every check passes; exits 1 and prints
 * the first failing check's details otherwise (main() returns non-zero on
 * any failure).
 *
 * This is a *reimplementation* of the same matrix-composition formulas used
 * in vr_runtime.c's build_mvp_vr_eye() (quat_to_mat3, mat3_transpose,
 * mat3_mul, mat3_flip_row2, and the combined_rot/right_world/head_pos_world
 * derivation), not a call into the real function -- build_mvp_vr_eye()
 * itself isn't easily unit-testable in isolation without pulling in the
 * full OpenXR/Vulkan/SDL headers it's compiled against (XrView,
 * SceneRenderCamera, SceneRenderProjection). Reimplementing the pure math
 * here keeps the test self-contained and fast to run, at the cost of the
 * two copies being able to drift apart -- acceptable given how small and
 * stable this math is (3 helper functions + one composition), and it's
 * exactly what caught the two Critical bugs this file exists to guard
 * against (the original numeric verification was done this same way, just
 * not committed -- this commit fixes that).
 *
 * Covers (per the task-6 fix-round review):
 *   (a) identity head pose reduces combined_rot to S * R_true, and a point
 *       straight ahead of the (possibly turned) cockpit still projects to
 *       eye-space (0, 0, -distance) -- i.e. screen-center, w > 0.
 *   (b) a yaw-left-30deg head pose brings a world point that is genuinely
 *       30deg to the left of the cockpit's forward direction to screen-
 *       center, with w > 0.
 *   (c) a pitch-up-30deg head pose brings a world point that is genuinely
 *       30deg above the cockpit's forward direction to screen-center, with
 *       w > 0.
 *   (d) identity head pose with a NON-trivial cockpit orientation (car
 *       turned 90deg in world) still centers the cockpit's own new forward
 *       direction -- exercises the V/R_true half of the composition, not
 *       just the S/H half that (a)-(c) mostly exercise with R_true == I.
 */
#include <math.h>
#include <stdio.h>

#define EPS 1e-3f

/* --- Exact reimplementation of vr_runtime.c's helpers --- */

static void quat_to_mat3(float x, float y, float z, float w, float m[3][3])
{
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] =     2 * (x * y - z * w); m[0][2] =     2 * (x * z + y * w);
    m[1][0] =     2 * (x * y + z * w); m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] =     2 * (y * z - x * w);
    m[2][0] =     2 * (x * z - y * w); m[2][1] =     2 * (y * z + x * w); m[2][2] = 1 - 2 * (x * x + y * y);
}

static void mat3_transpose(const float src[3][3], float dst[3][3])
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            dst[r][c] = src[c][r];
}

static void mat3_mul(const float a[3][3], const float b[3][3], float out[3][3])
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            float sum = 0.0f;
            for (int k = 0; k < 3; k++) sum += a[r][k] * b[k][c];
            out[r][c] = sum;
        }
}

static void mat3_flip_row2(const float src[3][3], float dst[3][3])
{
    dst[0][0] = src[0][0]; dst[0][1] = src[0][1]; dst[0][2] = src[0][2];
    dst[1][0] = src[1][0]; dst[1][1] = src[1][1]; dst[1][2] = src[1][2];
    dst[2][0] = -src[2][0]; dst[2][1] = -src[2][1]; dst[2][2] = -src[2][2];
}

static void mat3_identity(float m[3][3])
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            m[r][c] = (r == c) ? 1.0f : 0.0f;
}

/* Rotation about world +Y by angle theta (radians), right-hand rule. */
static void mat3_rot_y(float theta, float m[3][3])
{
    float s = sinf(theta), c = cosf(theta);
    m[0][0] = c;  m[0][1] = 0; m[0][2] = s;
    m[1][0] = 0;  m[1][1] = 1; m[1][2] = 0;
    m[2][0] = -s; m[2][1] = 0; m[2][2] = c;
}

static void mat3_vec_mul(const float m[3][3], const float v[3], float out[3])
{
    for (int r = 0; r < 3; r++)
        out[r] = m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2];
}

/* --- Test harness --- */

static int g_failures = 0;

static void check_close(const char *label, float got, float expected)
{
    if (fabsf(got - expected) > EPS) {
        printf("FAIL: %s -- got %f, expected %f (diff %f)\n",
               label, (double)got, (double)expected, (double)(got - expected));
        g_failures++;
    }
}

/* Computes combined_rot = H^T * S * R_true, exactly mirroring
 * build_mvp_vr_eye()'s derivation, given base_rot (V, naive [row][col] =
 * cockpit local-to-world) and a head orientation quaternion. */
static void compute_combined_rot(const float base_rot[3][3],
                                  float qx, float qy, float qz, float qw,
                                  float combined_rot[3][3])
{
    float r_true[3][3];
    mat3_transpose(base_rot, r_true);

    float head_rot[3][3];
    quat_to_mat3(qx, qy, qz, qw, head_rot);

    float head_rot_T[3][3];
    mat3_transpose(head_rot, head_rot_T);

    float s_r_true[3][3];
    mat3_flip_row2(r_true, s_r_true);

    mat3_mul(head_rot_T, s_r_true, combined_rot);
}

/* Feeds a world point through combined_rot (pure rotation, no eye-position
 * offset -- callers pick world points relative to a coincident cockpit/eye
 * origin) and checks it lands at eye-space (0, 0, -distance), i.e.
 * screen-center with w = -z_eye > 0, per the kept projection's convention. */
static void check_centers_at_distance(const char *label,
                                       const float combined_rot[3][3],
                                       const float world_point[3],
                                       float expected_distance)
{
    float eye_space[3];
    mat3_vec_mul(combined_rot, world_point, eye_space);
    char buf[160];
    snprintf(buf, sizeof(buf), "%s eye_space.x (should be ~0, centered)", label);
    check_close(buf, eye_space[0], 0.0f);
    snprintf(buf, sizeof(buf), "%s eye_space.y (should be ~0, centered)", label);
    check_close(buf, eye_space[1], 0.0f);
    snprintf(buf, sizeof(buf), "%s eye_space.z (should be -distance, -Z fwd)", label);
    check_close(buf, eye_space[2], -expected_distance);
    float w = -eye_space[2]; /* mvp[11] = -1 convention: w = -z_eye */
    if (!(w > 0.0f)) {
        printf("FAIL: %s w = %f, expected > 0 (point should be visible)\n", label, (double)w);
        g_failures++;
    }
}

int main(void)
{
    /* (a) Identity head pose, identity cockpit orientation (R_true == I,
     * i.e. cockpit axes == world axes: +X right, +Y up, +Z forward). */
    {
        float V[3][3];
        mat3_identity(V);
        float combined_rot[3][3];
        compute_combined_rot(V, 0.0f, 0.0f, 0.0f, 1.0f, combined_rot);

        /* combined_rot should reduce to exactly S (since R_true == I too). */
        float expected[3][3] = { {1,0,0}, {0,1,0}, {0,0,-1} };
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) {
                char buf[64];
                snprintf(buf, sizeof(buf), "(a) combined_rot[%d][%d]", r, c);
                check_close(buf, combined_rot[r][c], expected[r][c]);
            }

        /* A point straight ahead of the cockpit (+Z, distance 100) must
         * land centered, w > 0. */
        float world_point[3] = { 0.0f, 0.0f, 100.0f };
        check_centers_at_distance("(a) identity/straight-ahead", combined_rot, world_point, 100.0f);
    }

    /* (b) Yaw-left 30deg head pose, identity cockpit orientation.
     * Quaternion for +30deg about Y (qy(theta) = (0, sin(theta/2), 0, cos(theta/2)))
     * makes the head's local forward (-Z) come out, after the full S/H
     * chain (see build_mvp_vr_eye()'s own derivation comment), pointing at
     * world direction (-sin(theta), 0, cos(theta)) -- i.e. rotated toward
     * -X (left, given +X = right, +Z = forward, +Y = up is right-handed).
     * So a world point placed exactly along that direction is "genuinely
     * 30deg to the left" and must land centered after the yaw. */
    {
        float V[3][3];
        mat3_identity(V);
        float theta = 30.0f * (float)M_PI / 180.0f;
        float qy = sinf(theta / 2.0f), qw = cosf(theta / 2.0f);
        float combined_rot[3][3];
        compute_combined_rot(V, 0.0f, qy, 0.0f, qw, combined_rot);

        float distance = 100.0f;
        float world_point[3] = { -sinf(theta) * distance, 0.0f, cosf(theta) * distance };
        check_centers_at_distance("(b) yaw-left-30", combined_rot, world_point, distance);

        /* Negative control: the OLD (buggy) formula the task-6 fix-round
         * review flagged -- combined_rot = head_rot * base_rot directly
         * (no S conversion, no transpose) -- must NOT center this same
         * point, confirming this test actually discriminates correct from
         * inverted-yaw behavior rather than trivially passing either way. */
        float head_rot[3][3];
        quat_to_mat3(0.0f, qy, 0.0f, qw, head_rot);
        float buggy_combined[3][3];
        mat3_mul(head_rot, V, buggy_combined);
        float buggy_eye_space[3];
        mat3_vec_mul(buggy_combined, world_point, buggy_eye_space);
        if (fabsf(buggy_eye_space[0]) < EPS && fabsf(buggy_eye_space[1]) < EPS
            && buggy_eye_space[2] < 0.0f) {
            printf("FAIL: (b) negative control -- the old buggy formula unexpectedly "
                   "also centers this point; the test doesn't discriminate correct "
                   "from inverted yaw\n");
            g_failures++;
        }
    }

    /* (c) Pitch-up 30deg head pose, identity cockpit orientation.
     * Quaternion for +30deg about X (qx(theta) = (sin(theta/2), 0, 0, cos(theta/2)))
     * makes the head's local forward come out pointing at world direction
     * (0, sin(theta), cos(theta)) -- rotated toward +Y (up), matching
     * "pitch up" given +Y = up, +Z = forward. */
    {
        float V[3][3];
        mat3_identity(V);
        float theta = 30.0f * (float)M_PI / 180.0f;
        float qx = sinf(theta / 2.0f), qw = cosf(theta / 2.0f);
        float combined_rot[3][3];
        compute_combined_rot(V, qx, 0.0f, 0.0f, qw, combined_rot);

        float distance = 100.0f;
        float world_point[3] = { 0.0f, sinf(theta) * distance, cosf(theta) * distance };
        check_centers_at_distance("(c) pitch-up-30", combined_rot, world_point, distance);
    }

    /* (d) Identity head pose, NON-trivial cockpit orientation: car turned
     * 90deg in world (cockpit's own forward is now world +X instead of
     * +Z). Exercises the V/R_true half of the composition -- with the head
     * perfectly still, the cockpit's own new forward direction must still
     * land centered, exactly like the non-VR base camera would show. */
    {
        float V[3][3]; /* cockpit local-to-world, rotated 90deg about Y */
        mat3_rot_y(90.0f * (float)M_PI / 180.0f, V);
        float combined_rot[3][3];
        compute_combined_rot(V, 0.0f, 0.0f, 0.0f, 1.0f, combined_rot);

        /* Cockpit's own forward in world coords = V's column 2. */
        float cockpit_forward[3] = { V[0][2], V[1][2], V[2][2] };
        float distance = 100.0f;
        float world_point[3] = {
            cockpit_forward[0] * distance, cockpit_forward[1] * distance, cockpit_forward[2] * distance
        };
        check_centers_at_distance("(d) turned-cockpit/identity-head", combined_rot, world_point, distance);
    }

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
