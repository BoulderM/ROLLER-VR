/* tests/vr_camera_compose_test.c
 *
 * Standalone numeric test for vr_camera_compose.c. Build and run:
 *   zig build test-vr-pure
 * or directly:
 *   gcc -Wall -Wextra -I../PROJECTS/ROLLER vr_camera_compose_test.c \
 *       ../PROJECTS/ROLLER/vr_camera_compose.c -lm -o /tmp/t && /tmp/t
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "vr_camera_compose.h"

#define EPS 1e-3f
static int g_failures = 0;

static void check_close(const char *label, float got, float expected)
{
    if (fabsf(got - expected) > EPS) {
        printf("FAIL: %s -- got %f, expected %f (diff %f)\n",
               label, (double)got, (double)expected, (double)(got - expected));
        g_failures++;
    }
}

static void mat3_identity(float m[3][3])
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            m[r][c] = (r == c) ? 1.0f : 0.0f;
}

static void mat3_rot_y(float theta, float m[3][3])
{
    float s = sinf(theta), c = cosf(theta);
    m[0][0] = c;  m[0][1] = 0; m[0][2] = s;
    m[1][0] = 0;  m[1][1] = 1; m[1][2] = 0;
    m[2][0] = -s; m[2][1] = 0; m[2][2] = c;
}

/* Cross-check against the engine's now-retired build_mvp_vr_eye()'s own
 * derivation (back when it composed the live head pose itself): S*out_rotᵗ
 * must equal combined_rot = Hᵗ*S*Rᵗ (R = Vᵗ) -- see vr_camera_compose.h's
 * own derivation comment for the algebra. Reimplements combined_rot
 * independently (not calling vr_runtime.c) so this test has no dependency
 * on OpenXR headers. */
static void check_matches_old_combined_rot(const char *label, const float V[3][3],
                                            float qx, float qy, float qz, float qw)
{
    float out_rot[3][3], out_pos[3];
    float zero_pos[3] = {0, 0, 0};
    vr_camera_compose(V, zero_pos, qx, qy, qz, qw, 0, 0, 0, 1.0f, out_rot, out_pos);

    float out_rot_T[3][3];
    vr_mat3_transpose(out_rot, out_rot_T);
    float s_out_rot_T[3][3];
    vr_mat3_flip_row2(out_rot_T, s_out_rot_T);

    float R_true[3][3];
    vr_mat3_transpose(V, R_true);
    float H[3][3];
    vr_quat_to_mat3(qx, qy, qz, qw, H);
    float H_T[3][3];
    vr_mat3_transpose(H, H_T);
    float S_R_true[3][3];
    vr_mat3_flip_row2(R_true, S_R_true);
    float combined_rot[3][3];
    vr_mat3_mul(H_T, S_R_true, combined_rot);

    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%s cross-check[%d][%d]", label, r, c);
            check_close(buf, s_out_rot_T[r][c], combined_rot[r][c]);
        }
}

int main(void)
{
    /* (a) Identity head pose, identity cockpit: composed rotation stays
     * identity, composed position stays the base position unchanged. */
    {
        float V[3][3]; mat3_identity(V);
        float base_pos[3] = { 10.0f, 20.0f, 30.0f };
        float out_rot[3][3], out_pos[3];
        vr_camera_compose(V, base_pos, 0, 0, 0, 1, 0, 0, 0, 100.0f, out_rot, out_pos);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) {
                char buf[64]; snprintf(buf, sizeof(buf), "(a) out_rot[%d][%d]", r, c);
                check_close(buf, out_rot[r][c], (r == c) ? 1.0f : 0.0f);
            }
        check_close("(a) out_pos.x", out_pos[0], 10.0f);
        check_close("(a) out_pos.y", out_pos[1], 20.0f);
        check_close("(a) out_pos.z", out_pos[2], 30.0f);
        check_matches_old_combined_rot("(a)", V, 0, 0, 0, 1);
    }

    /* (b) Yaw-left-30deg head pose, identity cockpit, turned-90deg cockpit:
     * cross-check against the old combined_rot formula in both cases. */
    {
        float V[3][3]; mat3_identity(V);
        float theta = 30.0f * (float)M_PI / 180.0f;
        float qy = sinf(theta / 2.0f), qw = cosf(theta / 2.0f);
        check_matches_old_combined_rot("(b) yaw30/identity-cockpit", V, 0, qy, 0, qw);

        float V2[3][3]; mat3_rot_y(90.0f * (float)M_PI / 180.0f, V2);
        check_matches_old_combined_rot("(b) yaw30/turned-cockpit", V2, 0, qy, 0, qw);
    }

    /* (b2) Pitch-up-30deg head pose, identity cockpit: pure pitch rotation
     * (X-axis) exercises the row/column sign-flip interactions that yaw alone
     * doesn't touch -- cross-check against the old combined_rot formula. */
    {
        float V[3][3]; mat3_identity(V);
        float theta = 30.0f * (float)M_PI / 180.0f;
        float qx = sinf(theta / 2.0f), qw = cosf(theta / 2.0f);
        check_matches_old_combined_rot("(b2) pitch30/identity-cockpit", V, qx, 0, 0, qw);
    }

    /* (c) Eye separation: two eyes at +-0.0315m along local X (63mm IPD),
     * identity orientation, cockpit turned 90deg -- world positions must
     * differ by exactly IPD * meters_to_world along the cockpit's right
     * axis (V column 0), not double-counted. */
    {
        float V[3][3]; mat3_rot_y(90.0f * (float)M_PI / 180.0f, V);
        float zero_pos[3] = {0, 0, 0};
        float left_rot[3][3], left_pos[3], right_rot[3][3], right_pos[3];
        vr_camera_compose(V, zero_pos, 0, 0, 0, 1, -0.0315f, 0, 0, 100.0f, left_rot, left_pos);
        vr_camera_compose(V, zero_pos, 0, 0, 0, 1,  0.0315f, 0, 0, 100.0f, right_rot, right_pos);
        float d[3] = { right_pos[0]-left_pos[0], right_pos[1]-left_pos[1], right_pos[2]-left_pos[2] };
        float dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        check_close("(c) eye separation == IPD * meters_to_world", dist, 0.063f * 100.0f);
        check_close("(c) direction.x == cockpit right axis", d[0]/dist, V[0][0]);
        check_close("(c) direction.y == cockpit right axis", d[1]/dist, V[1][0]);
        check_close("(c) direction.z == cockpit right axis", d[2]/dist, V[2][0]);
    }

    /* (c2) Genuine no-double-count check for the REAL shared
     * vr_camera_view_basis() (vr_camera_compose.h/.c) -- the exact function
     * vr_runtime.c's build_mvp_vr_eye_projection() calls to turn an ALREADY
     * fully eye-composed camera into the rotation/position its projection
     * needs. This calls that production function directly below, not a
     * reimplementation of its math.
     *
     * Reuses case (c)'s left/right composed cameras (identity head
     * orientation, cockpit turned 90deg, eyes at +-0.0315m local X,
     * meters_to_world=100) -- exactly what Task 2's calculateview()
     * post-step would write into vk1..vk9/viewx,y,z for each eye, which is
     * what base_proj->view/base_cam->viewX,Y,Z snapshot in
     * vr_present_stereo_frame().
     *
     * The two assertions below are each checked against a value computed
     * INDEPENDENTLY of vr_camera_view_basis() itself -- neither is a
     * round-trip identity that would hold regardless of correctness
     * (contrast the old (c2), which recovered a position FROM the view
     * matrix it had just built from that same position -- true for any
     * orthogonal matrix, unconditionally, which is what code review
     * flagged):
     *
     * 1. out_eye_pos must come back EXACTLY equal to the pos fed in
     *    (left_pos/right_pos itself, not a value derived from it). A
     *    reintroduced double-composition bug -- adding a head-pose
     *    position delta back into vr_camera_view_basis() or around its
     *    call in build_mvp_vr_eye_projection(), the exact bug class Phase
     *    2 shipped for IPD -- would move out_eye_pos away from the input,
     *    and this assertion would catch it directly.
     *
     * 2. out_combined_rot must equal a HAND-COMPUTED literal, not a value
     *    obtained by calling vr_mat3_transpose()/vr_mat3_flip_row2() again
     *    in this test (which would just compare the function to itself).
     *    Identity head orientation makes left_rot == right_rot == V
     *    exactly (H_game = I when q = identity -- see vr_camera_compose()'s
     *    own derivation), and V = mat3_rot_y(90deg) = {{0,0,1},{0,1,0},
     *    {-1,0,0}} (c=cos90=0, s=sin90=1), so the expected
     *    combined_rot = S*transpose(V) can be derived by hand:
     *      r_true = transpose(V) = {{0,0,-1},{0,1,0},{1,0,0}}
     *        (r_true[0][2] = V[2][0] = -1; r_true[2][0] = V[0][2] = 1)
     *      combined_rot = flip_row2(r_true) = {{0,0,-1},{0,1,0},{-1,0,0}}
     *        (only row 2 negates: combined_rot[2][0] = -r_true[2][0] = -1)
     *    If vr_camera_view_basis() (or build_mvp_vr_eye_projection() around
     *    it) ever folded a live head rotation on top of r_true -- the
     *    rotation half of the same double-composition bug class --
     *    out_combined_rot would no longer match this literal. */
    {
        float V[3][3]; mat3_rot_y(90.0f * (float)M_PI / 180.0f, V);
        float zero_pos[3] = {0, 0, 0};
        float left_rot[3][3], left_pos[3], right_rot[3][3], right_pos[3];
        vr_camera_compose(V, zero_pos, 0, 0, 0, 1, -0.0315f, 0, 0, 100.0f, left_rot, left_pos);
        vr_camera_compose(V, zero_pos, 0, 0, 0, 1,  0.0315f, 0, 0, 100.0f, right_rot, right_pos);

        /* Self-contained sanity check that this case's own setup still
         * produces the 1x-IPD separation case (c) already proves in
         * general. */
        {
            float d[3] = { right_pos[0]-left_pos[0], right_pos[1]-left_pos[1], right_pos[2]-left_pos[2] };
            float dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
            check_close("(c2) fed-in eye separation == 1x IPD * meters_to_world", dist, 0.063f * 100.0f);
        }

        const float expected_combined_rot[3][3] = {
            {  0.0f, 0.0f, -1.0f },
            {  0.0f, 1.0f,  0.0f },
            { -1.0f, 0.0f,  0.0f },
        };
        const char *labels[2]        = { "left", "right" };
        float (*eye_rots[2])[3]      = { left_rot, right_rot };
        float *eye_poss[2]           = { left_pos, right_pos };

        for (int eye = 0; eye < 2; eye++) {
            float out_combined_rot[3][3], out_eye_pos[3];

            /* Calls the REAL, shared production function. */
            vr_camera_view_basis(eye_rots[eye], eye_poss[eye], out_combined_rot, out_eye_pos);

            char buf[96];
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) {
                    snprintf(buf, sizeof(buf), "(c2) %s out_combined_rot[%d][%d]", labels[eye], r, c);
                    check_close(buf, out_combined_rot[r][c], expected_combined_rot[r][c]);
                }

            snprintf(buf, sizeof(buf), "(c2) %s out_eye_pos.x == fed-in pos (unchanged, not recomposed)", labels[eye]);
            check_close(buf, out_eye_pos[0], eye_poss[eye][0]);
            snprintf(buf, sizeof(buf), "(c2) %s out_eye_pos.y == fed-in pos (unchanged, not recomposed)", labels[eye]);
            check_close(buf, out_eye_pos[1], eye_poss[eye][1]);
            snprintf(buf, sizeof(buf), "(c2) %s out_eye_pos.z == fed-in pos (unchanged, not recomposed)", labels[eye]);
            check_close(buf, out_eye_pos[2], eye_poss[eye][2]);
        }
    }

    /* (d) Angle round-trip: build a known rotation via the SAME formula
     * calculatetransform() uses (transfrm.c:43-57, with iTilt=0 for
     * simplicity -- yaw/pitch only), extract angles back, confirm they
     * match (mod the 14-bit quantization epsilon). */
    {
        int yawIn = 1234, pitchIn = 567; /* arbitrary non-trivial 14-bit angles */
        float yawRad = (float)yawIn * 2.0f * (float)M_PI / 16384.0f;
        float pitchRad = (float)pitchIn * 2.0f * (float)M_PI / 16384.0f;
        float sy = sinf(yawRad), cy = cosf(yawRad);
        float sp = sinf(pitchRad), cp = cosf(pitchRad);
        /* calculatetransform() with iTilt=0: vk1=sy, vk2=-cy*sp, vk3=cy*cp...
         * see transfrm.c:43-57 -- reconstructing the [row][col] = V convention
         * (V = transpose of the vk-storage "R_true" relationship), this is
         * most directly checked via the forward/up columns our extractor
         * actually reads, so build V with the KNOWN forward=(cy*cp,sy*cp,sp)
         * directly (that's vk3, vk6, vk9), and up column from the
         * vk2/vk5/vk8 derivation. */
        float V[3][3] = {
            {  sy, -cy*sp, cy*cp },
            { -cy, -sy*sp, sy*cp },
            {  0,   cp,    sp    },
        };
        int outYaw, outPitch, outRoll;
        vr_camera_angles_from_rotation(V, &outYaw, &outPitch, &outRoll);
        /* Allow +-2 for 14-bit table quantization vs. direct float trig. */
        if (abs(outYaw - yawIn) > 2 && abs(outYaw - yawIn) < 16382)
            { printf("FAIL: (d) yaw round-trip got %d expected ~%d\n", outYaw, yawIn); g_failures++; }
        if (abs(outPitch - pitchIn) > 2 && abs(outPitch - pitchIn) < 16382)
            { printf("FAIL: (d) pitch round-trip got %d expected ~%d\n", outPitch, pitchIn); g_failures++; }
    }

    if (g_failures == 0) { printf("ALL PASS\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
