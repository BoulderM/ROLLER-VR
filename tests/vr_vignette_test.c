/* tests/vr_vignette_test.c -- pure-C checks of the comfort vignette maths
 * (PROJECTS/ROLLER/vr_vignette.c). No SDL, OpenXR or GPU.
 *
 * Optional: pass a file name to also write the ring texture's alpha channel there as
 * VR_VIGNETTE_TEX_SIZE^2 raw bytes (used to preview the real texture). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../PROJECTS/ROLLER/vr_vignette.h"

static int g_failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)
#define NEAR(a, b, eps) (fabsf((float)(a) - (float)(b)) <= (eps))

static void test_names(void)
{
    VrVignetteLevel l = VR_VIGNETTE_STRONG;
    CHECK(strcmp(vr_vignette_level_name(VR_VIGNETTE_OFF), "off") == 0);
    CHECK(strcmp(vr_vignette_level_name(VR_VIGNETTE_MILD), "mild") == 0);
    CHECK(strcmp(vr_vignette_level_name(VR_VIGNETTE_STRONG), "strong") == 0);
    CHECK(strcmp(vr_vignette_level_name((VrVignetteLevel)99), "off") == 0);

    CHECK(vr_vignette_level_parse("off", &l) && l == VR_VIGNETTE_OFF);
    CHECK(vr_vignette_level_parse("Mild", &l) && l == VR_VIGNETTE_MILD);
    CHECK(vr_vignette_level_parse("  STRONG\t", &l) && l == VR_VIGNETTE_STRONG);
    l = VR_VIGNETTE_MILD;
    CHECK(!vr_vignette_level_parse("medium", &l) && l == VR_VIGNETTE_MILD); /* untouched on failure */
    CHECK(!vr_vignette_level_parse("", &l));
    CHECK(!vr_vignette_level_parse("mildx", &l));
    CHECK(!vr_vignette_level_parse(NULL, &l));

    CHECK(vr_vignette_level_next(VR_VIGNETTE_OFF) == VR_VIGNETTE_MILD);
    CHECK(vr_vignette_level_next(VR_VIGNETTE_MILD) == VR_VIGNETTE_STRONG);
    CHECK(vr_vignette_level_next(VR_VIGNETTE_STRONG) == VR_VIGNETTE_OFF);

    CHECK(vr_vignette_level_peak(VR_VIGNETTE_OFF) == 0.0f);
    CHECK(vr_vignette_level_peak(VR_VIGNETTE_MILD) > 0.0f);
    CHECK(vr_vignette_level_peak(VR_VIGNETTE_MILD) < vr_vignette_level_peak(VR_VIGNETTE_STRONG));
    CHECK(vr_vignette_level_peak(VR_VIGNETTE_STRONG) == 1.0f);
}

static void rot_axis(char axis, float deg, float m[3][3])
{
    float c = cosf(deg * (float)M_PI / 180.0f), s = sinf(deg * (float)M_PI / 180.0f);
    memset(m, 0, 9 * sizeof(float));
    if (axis == 'x')      { m[0][0] = 1; m[1][1] = c; m[1][2] = -s; m[2][1] = s; m[2][2] = c; }
    else if (axis == 'y') { m[1][1] = 1; m[0][0] = c; m[0][2] = s;  m[2][0] = -s; m[2][2] = c; }
    else                  { m[2][2] = 1; m[0][0] = c; m[0][1] = -s; m[1][0] = s;  m[1][1] = c; }
}

static void test_rotation(void)
{
    float I[3][3], a[3][3], b[3][3];
    rot_axis('z', 0, I);
    CHECK(NEAR(vr_vignette_rotation_deg(I, I), 0.0f, 0.01f));
    rot_axis('z', 90, a);
    CHECK(NEAR(vr_vignette_rotation_deg(I, a), 90.0f, 0.05f));
    CHECK(NEAR(vr_vignette_rotation_deg(a, I), 90.0f, 0.05f));     /* symmetric */
    rot_axis('x', 30, a);
    CHECK(NEAR(vr_vignette_rotation_deg(I, a), 30.0f, 0.05f));
    rot_axis('y', 0.5f, a);                                       /* small steps keep their precision */
    CHECK(NEAR(vr_vignette_rotation_deg(I, a), 0.5f, 0.05f));
    rot_axis('y', 170, a);
    rot_axis('y', -10, b);
    CHECK(NEAR(vr_vignette_rotation_deg(a, b), 180.0f, 0.1f));    /* wraps through 180 */
    rot_axis('z', 180, a);
    CHECK(NEAR(vr_vignette_rotation_deg(I, a), 180.0f, 0.1f));
    /* Same rotation about the other axis, composed: 10 deg about z after 40 deg about z is 10. */
    rot_axis('z', 40, a);
    rot_axis('z', 50, b);
    CHECK(NEAR(vr_vignette_rotation_deg(a, b), 10.0f, 0.05f));
}

static float run(VrVignetteState *s, VrVignetteLevel level, float rate, float seconds, bool preview)
{
    const float dt = 1.0f / 90.0f;
    float v = s->strength;
    for (float t = 0; t < seconds; t += dt)
        v = vr_vignette_step(s, level, rate, dt, preview);
    return v;
}

static void test_step(void)
{
    VrVignetteState s = { 0 };
    const float mild = vr_vignette_level_peak(VR_VIGNETTE_MILD);

    /* Off is always 0, whatever the motion. */
    CHECK(run(&s, VR_VIGNETTE_OFF, 500.0f, 2.0f, false) == 0.0f);
    CHECK(run(&s, VR_VIGNETTE_OFF, 500.0f, 2.0f, true) == 0.0f);

    /* A straight road: no turning, no vignette. */
    s.strength = 0;
    CHECK(run(&s, VR_VIGNETTE_MILD, 0.0f, 3.0f, false) == 0.0f);
    CHECK(run(&s, VR_VIGNETTE_STRONG, 5.0f, 3.0f, false) == 0.0f); /* under the dead zone */

    /* Hard turning drives mild to its peak and no further. */
    float v = run(&s, VR_VIGNETTE_MILD, 400.0f, 2.0f, false);
    CHECK(NEAR(v, mild, 0.01f));
    CHECK(v <= mild + 1e-6f);

    /* Release is slow: still well on after 0.3 s, gone after 4 s. Attack is quicker than release. */
    float after = run(&s, VR_VIGNETTE_MILD, 0.0f, 0.3f, false);
    CHECK(after > 0.5f * mild);
    CHECK(run(&s, VR_VIGNETTE_MILD, 0.0f, 4.0f, false) < VR_VIGNETTE_MIN_VISIBLE);
    VrVignetteState up = { 0 }, down = { mild };
    float rise = run(&up, VR_VIGNETTE_MILD, 400.0f, 0.2f, false);
    float fall = mild - run(&down, VR_VIGNETTE_MILD, 0.0f, 0.2f, false);
    CHECK(rise > fall);

    /* Strong reacts to gentler turning than mild does, and gets darker. */
    VrVignetteState m = { 0 }, st = { 0 };
    float mv = run(&m, VR_VIGNETTE_MILD, 60.0f, 3.0f, false);
    float sv = run(&st, VR_VIGNETTE_STRONG, 60.0f, 3.0f, false);
    CHECK(sv > mv);
    VrVignetteState st2 = { 0 };
    CHECK(NEAR(run(&st2, VR_VIGNETTE_STRONG, 400.0f, 2.0f, false), 1.0f, 0.01f));

    /* Switching to a milder level cannot leave it above the new peak. */
    VrVignetteState sw = { 1.0f };
    CHECK(vr_vignette_step(&sw, VR_VIGNETTE_MILD, 0.0f, 1.0f / 90.0f, false) <= mild + 1e-6f);
    CHECK(vr_vignette_step(&sw, VR_VIGNETTE_OFF, 0.0f, 1.0f / 90.0f, false) == 0.0f);

    /* Preview (pause menu) shows the peak even with no motion, then lets go. */
    VrVignetteState pv = { 0 };
    CHECK(run(&pv, VR_VIGNETTE_STRONG, 0.0f, 1.0f, true) > 0.95f);
    CHECK(run(&pv, VR_VIGNETTE_STRONG, 0.0f, 5.0f, false) < VR_VIGNETTE_MIN_VISIBLE);

    /* A zero or negative frame time changes nothing. */
    VrVignetteState z = { 0.4f };
    CHECK(vr_vignette_step(&z, VR_VIGNETTE_STRONG, 400.0f, 0.0f, false) == 0.4f);
    CHECK(vr_vignette_step(&z, VR_VIGNETTE_STRONG, 400.0f, -1.0f, false) == 0.4f);
}

static void test_geometry(void)
{
    CHECK(NEAR(vr_vignette_clear_half_angle_deg(0.0f), VR_VIGNETTE_OPEN_CLEAR_DEG, 0.01f));
    CHECK(NEAR(vr_vignette_clear_half_angle_deg(1.0f), VR_VIGNETTE_STRONG_CLEAR_DEG, 0.01f));
    CHECK(NEAR(vr_vignette_clear_half_angle_deg(-3.0f), VR_VIGNETTE_OPEN_CLEAR_DEG, 0.01f)); /* clamped */
    CHECK(NEAR(vr_vignette_clear_half_angle_deg(7.0f), VR_VIGNETTE_STRONG_CLEAR_DEG, 0.01f));
    float prev_a = 1000.0f, prev_k = 0.0f;
    for (float s = 0.0f; s <= 1.0f; s += 0.05f) {
        float a = vr_vignette_clear_half_angle_deg(s), k = vr_vignette_quad_scale(s);
        CHECK(a < prev_a);          /* closing in as strength rises */
        CHECK(k >= 1.0f - 1e-4f);
        if (s > 0.0f) CHECK(k < prev_k); /* scale shrinks towards 1 */
        prev_a = a;
        prev_k = k;
    }
    CHECK(NEAR(vr_vignette_quad_scale(1.0f), 1.0f, 1e-3f));
    CHECK(vr_vignette_quad_scale(0.0f) > 5.0f);
    /* The mild peak still reaches into a headset's view: its clear centre is well under 50 deg. */
    CHECK(vr_vignette_clear_half_angle_deg(vr_vignette_level_peak(VR_VIGNETTE_MILD)) < 45.0f);
}

static void test_texture(const char *dump_path)
{
    const int N = VR_VIGNETTE_TEX_SIZE;
    uint8_t *px = (uint8_t *)malloc((size_t)N * N * 4);
    CHECK(px != NULL);
    if (!px) return;
    memset(px, 0x55, (size_t)N * N * 4);
    vr_vignette_fill_texture(px);
    #define A(x, y) px[((size_t)(y) * N + (x)) * 4 + 3]

    int colour_ok = 1;
    for (size_t i = 0; i < (size_t)N * N; ++i)
        if (px[i * 4] || px[i * 4 + 1] || px[i * 4 + 2]) colour_ok = 0;
    CHECK(colour_ok);                          /* black everywhere; only alpha varies */

    CHECK(A(N / 2, N / 2) == 0);               /* clear centre */
    CHECK(A(0, 0) == 255 && A(N - 1, 0) == 255 && A(0, N - 1) == 255 && A(N - 1, N - 1) == 255);

    int sym = 1, mono = 1;
    for (int y = 0; y < N; y += 7)
        for (int x = 0; x < N; x += 5)
            if (A(x, y) != A(N - 1 - x, y) || A(x, y) != A(x, N - 1 - y) || A(x, y) != A(y, x)) sym = 0;
    for (int x = N / 2; x < N - 1; ++x)
        if (A(x + 1, N / 2) < A(x, N / 2)) mono = 0;
    CHECK(sym);                                /* round, not stretched */
    CHECK(mono);                               /* never lighter further out */

    /* The clear centre ends at STRONG_CLEAR_DEG and the fade is FADE_WIDTH wide (tangent units). */
    float texel = 2.0f * VR_VIGNETTE_TEX_EXTENT / (float)N;
    float t0 = tanf(VR_VIGNETTE_STRONG_CLEAR_DEG * (float)M_PI / 180.0f);
    int x_clear = N / 2 + (int)((t0 - 0.05f) / texel);
    int x_full = N / 2 + (int)((t0 + VR_VIGNETTE_FADE_WIDTH + 0.05f) / texel);
    int x_mid = N / 2 + (int)((t0 + 0.5f * VR_VIGNETTE_FADE_WIDTH) / texel);
    CHECK(A(x_clear, N / 2) == 0);
    CHECK(A(x_full, N / 2) == 255);
    CHECK(A(x_mid, N / 2) > 90 && A(x_mid, N / 2) < 165);   /* about half way */
    #undef A

    if (dump_path) {
        FILE *f = fopen(dump_path, "wb");
        if (f) {
            for (size_t i = 0; i < (size_t)N * N; ++i) fputc(px[i * 4 + 3], f);
            fclose(f);
        }
    }
    free(px);
}

static void test_settings(void)
{
    VrVignetteLevel l = VR_VIGNETTE_OFF;
    CHECK(vr_vignette_settings_parse("vignette=mild\n", &l) && l == VR_VIGNETTE_MILD);
    CHECK(vr_vignette_settings_parse("  Vignette = STRONG  \r\n", &l) && l == VR_VIGNETTE_STRONG);
    CHECK(vr_vignette_settings_parse("; roller-vr\n# note\nother=1\nvignette=off", &l) && l == VR_VIGNETTE_OFF);
    CHECK(vr_vignette_settings_parse("vignette=mild\nvignette=strong\n", &l) && l == VR_VIGNETTE_STRONG); /* last wins */
    l = VR_VIGNETTE_MILD;
    CHECK(!vr_vignette_settings_parse("vignette=medium\n", &l) && l == VR_VIGNETTE_MILD); /* untouched */
    CHECK(vr_vignette_settings_parse("vignette=strong\nvignette=bogus\n", &l) && l == VR_VIGNETTE_STRONG); /* bad line ignored */
    CHECK(!vr_vignette_settings_parse("", &l));
    CHECK(!vr_vignette_settings_parse(NULL, &l));
    CHECK(!vr_vignette_settings_parse("; vignette=strong\n", &l));      /* a comment is not a setting */
    CHECK(!vr_vignette_settings_parse("myvignette=strong\n", &l));      /* whole key only */

    char out[512];
    size_t n = vr_vignette_settings_format(NULL, VR_VIGNETTE_MILD, out, sizeof out);
    CHECK(n > 0 && n == strlen(out));
    CHECK(strstr(out, "vignette=mild\n") != NULL);
    CHECK(vr_vignette_settings_parse(out, &l) && l == VR_VIGNETTE_MILD);

    /* Existing text: other lines kept, the vignette line replaced in place, extras dropped. */
    const char *old = "; mine\nother=1\nvignette=off\nmore=2\nvignette=mild\n";
    n = vr_vignette_settings_format(old, VR_VIGNETTE_STRONG, out, sizeof out);
    CHECK(n > 0);
    CHECK(strstr(out, "; mine\n") && strstr(out, "other=1\n") && strstr(out, "more=2\n"));
    CHECK(strstr(out, "vignette=strong\n") != NULL);
    CHECK(strstr(out, "vignette=off") == NULL && strstr(out, "vignette=mild") == NULL);
    CHECK(strstr(strstr(out, "vignette=strong") + 1, "vignette=") == NULL); /* exactly one */
    CHECK(strstr(out, "other=1") < strstr(out, "vignette=strong") && strstr(out, "vignette=strong") < strstr(out, "more=2"));

    /* No key yet, no trailing newline: appended on its own line. */
    n = vr_vignette_settings_format("other=1", VR_VIGNETTE_OFF, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "other=1\nvignette=off\n") == 0);
    /* Round trip for every level. */
    for (int lv = 0; lv < VR_VIGNETTE_LEVEL_COUNT; ++lv) {
        VrVignetteLevel back = (VrVignetteLevel)99;
        n = vr_vignette_settings_format("x=1\n", (VrVignetteLevel)lv, out, sizeof out);
        CHECK(n > 0 && vr_vignette_settings_parse(out, &back) && back == (VrVignetteLevel)lv);
    }
    /* Too small a buffer: nothing is claimed to be written. */
    CHECK(vr_vignette_settings_format(old, VR_VIGNETTE_STRONG, out, 8) == 0);
    CHECK(vr_vignette_settings_format(old, VR_VIGNETTE_STRONG, NULL, 0) == 0);
}

int main(int argc, char **argv)
{
    test_names();
    test_rotation();
    test_step();
    test_geometry();
    test_texture(argc > 1 ? argv[1] : NULL);
    test_settings();

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("vr_vignette_test: ALL PASS\n");
    return 0;
}
