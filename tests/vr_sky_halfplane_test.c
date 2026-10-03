/* tests/vr_sky_halfplane_test.c
 *
 * Pure-C checks for vr_eye_sky_halfplane() (vr_camera_compose.{h,c}): the
 * sky/ground split a VR eye uses instead of the flat-window horizon.
 *
 * The expected horizon is NOT taken from the function's own formula. Each
 * case builds an eye camera, projects real world points through the same
 * pipeline the GPU uses for the eye (vr_camera_view_basis() for the
 * world-to-eye rotation, then a standard OpenXR asymmetric projection from the
 * FOV tangents), and checks that:
 *   - far-away points at zero elevation (the horizon) land on the line
 *     a*x + b*y + c = 0,
 *   - points above the horizon land on the sky side (> 0), points below on
 *     the ground side (< 0).
 * Plus two closed-form cases: a level eye's horizon is the NDC row of the
 * view centre, and an eye pitched down by e sees the horizon at
 * tan(e) above centre, scaled by the eye's own (not the flat window's) FOV. */
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
 * right, column 1 = up, column 2 = forward, all in world space (+Z up).
 * yaw about world Z, then pitch (positive = look up), then roll about forward. */
static void game_camera(float yaw, float pitch, float roll, float m[3][3])
{
    float fwd[3] = { cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), sinf(pitch) };
    float up0[3] = { -sinf(pitch) * cosf(yaw), -sinf(pitch) * sinf(yaw), cosf(pitch) };
    /* right = up0 x fwd: the handedness vr_camera_view_basis() expects is
     * exercised by the projection round-trip below either way, because both
     * the expected and the tested values go through the same basis. */
    float rgt[3] = { up0[1] * fwd[2] - up0[2] * fwd[1],
                     up0[2] * fwd[0] - up0[0] * fwd[2],
                     up0[0] * fwd[1] - up0[1] * fwd[0] };
    float cr = cosf(roll), sr = sinf(roll);
    for (int i = 0; i < 3; i++) {
        float r = rgt[i] * cr + up0[i] * sr;
        float u = -rgt[i] * sr + up0[i] * cr;
        m[i][0] = r; m[i][1] = u; m[i][2] = fwd[i];
    }
}

/* Projects a world direction (a point at infinity) into eye NDC. Returns 0 if
 * it is behind the eye. */
static int project_dir(const float base_rot[3][3], const float t[4] /* l r u d */,
                       const float dir[3], float *x, float *y)
{
    float R[3][3], eye[3];
    const float zero[3] = { 0, 0, 0 };
    vr_camera_view_basis(base_rot, zero, R, eye);
    float e[3];
    for (int r = 0; r < 3; r++) e[r] = R[r][0] * dir[0] + R[r][1] * dir[1] + R[r][2] * dir[2];
    if (e[2] >= -1e-4f) return 0;  /* OpenXR eye space looks down -Z */
    float X = e[0] / -e[2], Y = e[1] / -e[2];
    *x = (2.0f * X - (t[1] + t[0])) / (t[1] - t[0]);
    *y = (2.0f * Y - (t[2] + t[3])) / (t[2] - t[3]);
    return 1;
}

static void run_case(const char *name, float yaw, float pitch, float roll, const float t[4])
{
    float rot[3][3];
    game_camera(yaw, pitch, roll, rot);
    float a, b, c;
    vr_eye_sky_halfplane(rot, t[0], t[1], t[2], t[3], &a, &b, &c);
    float norm = sqrtf(a * a + b * b);
    char label[160];
    int on_screen = 0;
    for (int k = -60; k <= 60; k += 5) {
        float az = yaw + k * PI / 180.0f;
        for (int el = -20; el <= 20; el += 10) {
            float e = el * PI / 180.0f;
            float dir[3] = { cosf(e) * cosf(az), cosf(e) * sinf(az), sinf(e) };
            float x, y;
            if (!project_dir(rot, t, dir, &x, &y) || fabsf(x) > 1.5f || fabsf(y) > 1.5f)
                continue;
            on_screen++;
            float d = (a * x + b * y + c) / norm;
            if (el == 0) {
                snprintf(label, sizeof label, "%s: horizon point az%+d lies on the line (d=%g)", name, k, d);
                check(label, fabsf(d) < 1e-3f);
            } else if (el > 0) {
                snprintf(label, sizeof label, "%s: point az%+d el%+d is sky", name, k, el);
                check(label, d > 0.0f);
            } else {
                snprintf(label, sizeof label, "%s: point az%+d el%+d is ground", name, k, el);
                check(label, d < 0.0f);
            }
        }
    }
    snprintf(label, sizeof label, "%s: some sample points are on screen", name);
    check(label, on_screen > 10);
}

int main(void)
{
    /* Symmetric stand-in FOV (--vr-fake-head) and an asymmetric, HMD-like one. */
    float ts = tanf(0.733f);
    const float sym[4]  = { -ts, ts, ts, -ts };
    const float asym[4] = { tanf(-0.87f), tanf(0.75f), tanf(0.82f), tanf(-0.95f) };

    run_case("level, symmetric", 0.3f, 0.0f, 0.0f, sym);
    run_case("pitch down 15, symmetric", 1.1f, -15.0f * PI / 180.0f, 0.0f, sym);
    run_case("pitch up 20, roll 25, asymmetric", -2.0f, 20.0f * PI / 180.0f, 25.0f * PI / 180.0f, asym);
    run_case("roll -40, asymmetric", 2.5f, -5.0f * PI / 180.0f, -40.0f * PI / 180.0f, asym);

    /* Closed form: level eye, symmetric FOV -> horizon is the centre row, sky above. */
    {
        float rot[3][3], a, b, c;
        game_camera(0.0f, 0.0f, 0.0f, rot);
        vr_eye_sky_halfplane(rot, sym[0], sym[1], sym[2], sym[3], &a, &b, &c);
        check("level: horizon is horizontal", fabsf(a) < 1e-5f);
        check("level: horizon through the centre", fabsf(c) < 1e-5f);
        check("level: sky is up", b > 0.0f);
    }
    /* Closed form: pitched down 10 deg -> horizon at NDC y = tan(10)/tan(fov_up),
     * i.e. scaled by the eye FOV. The flat-window formula would put it at
     * VIEWDIST*tan(10)/100 = 2.7*tan(10) instead, about 2.4x too high. */
    {
        float rot[3][3], a, b, c;
        float e = 10.0f * PI / 180.0f;
        game_camera(0.0f, -e, 0.0f, rot);
        vr_eye_sky_halfplane(rot, sym[0], sym[1], sym[2], sym[3], &a, &b, &c);
        float y_h = -c / b;
        check("pitch down 10: horizon at tan(10)/tan(fov_up) above centre",
              fabsf(y_h - tanf(e) / ts) < 1e-4f);
    }

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
