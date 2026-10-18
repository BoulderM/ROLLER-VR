/* tests/vr_hud_layout_test.c -- pure-C checks of the HUD layout geometry
 * (PROJECTS/ROLLER/vr_hud_layout.c). No SDL, OpenXR or GPU. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../PROJECTS/ROLLER/vr_hud_layout.h"

static int g_failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)
#define NEAR(a, b, eps) (fabsf((float)(a) - (float)(b)) <= (eps))

static void test_names(void)
{
    VrHudLayout l = VR_HUD_WIDE;
    CHECK(strcmp(vr_hud_layout_name(VR_HUD_ORIGINAL), "original") == 0);
    CHECK(strcmp(vr_hud_layout_name(VR_HUD_SPREAD), "spread") == 0);
    CHECK(strcmp(vr_hud_layout_name(VR_HUD_WIDE), "wide") == 0);
    CHECK(strcmp(vr_hud_layout_label(VR_HUD_ORIGINAL), "ORIGINAL") == 0);
    CHECK(strcmp(vr_hud_layout_label(VR_HUD_SPREAD), "SPREAD") == 0);
    CHECK(strcmp(vr_hud_layout_label(VR_HUD_WIDE), "WIDE") == 0);
    CHECK(strcmp(vr_hud_layout_name((VrHudLayout)99), "original") == 0);
    CHECK(strcmp(vr_hud_layout_label((VrHudLayout)-1), "ORIGINAL") == 0);

    CHECK(vr_hud_layout_parse("original", &l) && l == VR_HUD_ORIGINAL);
    CHECK(vr_hud_layout_parse(" Spread\t", &l) && l == VR_HUD_SPREAD);
    CHECK(vr_hud_layout_parse("WIDE", &l) && l == VR_HUD_WIDE);
    l = VR_HUD_SPREAD;
    CHECK(!vr_hud_layout_parse("wider", &l) && l == VR_HUD_SPREAD);   /* untouched on failure */
    CHECK(!vr_hud_layout_parse("", &l));
    CHECK(!vr_hud_layout_parse(NULL, &l));
    CHECK(!vr_hud_layout_parse("wi de", &l));

    CHECK(vr_hud_layout_next(VR_HUD_ORIGINAL) == VR_HUD_SPREAD);
    CHECK(vr_hud_layout_next(VR_HUD_SPREAD) == VR_HUD_WIDE);
    CHECK(vr_hud_layout_next(VR_HUD_WIDE) == VR_HUD_ORIGINAL);
    CHECK(vr_hud_layout_prev(VR_HUD_ORIGINAL) == VR_HUD_WIDE);
    CHECK(vr_hud_layout_prev(VR_HUD_SPREAD) == VR_HUD_ORIGINAL);
    CHECK(vr_hud_layout_prev(VR_HUD_WIDE) == VR_HUD_SPREAD);
    for (int i = 0; i < VR_HUD_LAYOUT_COUNT; ++i)
        CHECK(vr_hud_layout_prev(vr_hud_layout_next((VrHudLayout)i)) == (VrHudLayout)i);
}

static void test_original(void)
{
    VrHudPanel p[VR_HUD_MAX_PANELS];
    int n = vr_hud_panels(VR_HUD_ORIGINAL, 640, 400, 1.2f, p);
    CHECK(n == 1);
    CHECK(p[0].x == 0 && p[0].y == 0 && p[0].w == 640 && p[0].h == 400);
    CHECK(NEAR(p[0].right_m, 0.0f, 1e-6f) && NEAR(p[0].up_m, 0.0f, 1e-6f));
    CHECK(NEAR(p[0].width_m, 1.2f, 1e-5f) && NEAR(p[0].height_m, 0.75f, 1e-5f));
}

/* Every pixel of the texture is in exactly one panel. */
static void check_tiling(int w, int h, VrHudLayout layout)
{
    VrHudPanel p[VR_HUD_MAX_PANELS];
    int n = vr_hud_panels(layout, w, h, 1.2f, p);
    CHECK(n == VR_HUD_MAX_PANELS);
    unsigned char *cover = (unsigned char *)calloc((size_t)w * h, 1);
    CHECK(cover != NULL);
    if (!cover) return;
    for (int i = 0; i < n; ++i) {
        CHECK(p[i].w > 0 && p[i].h > 0);
        CHECK(p[i].x >= 0 && p[i].y >= 0 && p[i].x + p[i].w <= w && p[i].y + p[i].h <= h);
        for (int y = p[i].y; y < p[i].y + p[i].h && y < h; ++y)
            for (int x = p[i].x; x < p[i].x + p[i].w && x < w; ++x)
                if (x >= 0 && y >= 0 && cover[(size_t)y * w + x] < 255) cover[(size_t)y * w + x]++;
    }
    int bad = 0;
    for (size_t i = 0; i < (size_t)w * h; ++i)
        if (cover[i] != 1) ++bad;
    CHECK(bad == 0);
    free(cover);
}

static void test_tiling(void)
{
    const int sizes[][2] = { { 640, 400 }, { 320, 200 }, { 1280, 800 }, { 800, 500 }, { 321, 199 }, { 100, 64 } };
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        check_tiling(sizes[s][0], sizes[s][1], VR_HUD_SPREAD);
        check_tiling(sizes[s][0], sizes[s][1], VR_HUD_WIDE);
    }
}

static float dist(float x, float y) { return sqrtf(x * x + y * y); }

static void test_spread(void)
{
    VrHudPanel o[VR_HUD_MAX_PANELS], s[VR_HUD_MAX_PANELS], w[VR_HUD_MAX_PANELS];
    CHECK(vr_hud_panels(VR_HUD_SPREAD, 640, 400, 1.2f, s) == 6);
    CHECK(vr_hud_panels(VR_HUD_WIDE, 640, 400, 1.2f, w) == 6);
    /* Compare with the same panels left where they are: ORIGINAL cut the same way would put each
     * panel's centre at its rectangle's centre. */
    const float mpp = 1.2f / 640.0f;
    for (int i = 0; i < 6; ++i) {
        o[i] = s[i];
        o[i].right_m = ((float)s[i].x + (float)s[i].w * 0.5f - 320.0f) * mpp;
        o[i].up_m = -((float)s[i].y + (float)s[i].h * 0.5f - 200.0f) * mpp;
    }
    int moved = 0, still = 0;
    for (int i = 0; i < 6; ++i) {
        CHECK(NEAR(s[i].width_m, (float)s[i].w * mpp, 1e-5f) && NEAR(s[i].height_m, (float)s[i].h * mpp, 1e-5f));
        CHECK(s[i].w == w[i].w && s[i].h == w[i].h && s[i].x == w[i].x && s[i].y == w[i].y); /* same cuts */
        CHECK(NEAR(s[i].width_m, w[i].width_m, 1e-6f));                                    /* same size */
        float dsx = s[i].right_m - o[i].right_m, dsy = s[i].up_m - o[i].up_m;
        float dwx = w[i].right_m - o[i].right_m, dwy = w[i].up_m - o[i].up_m;
        if (dist(dsx, dsy) < 1e-6f) {
            ++still;
            CHECK(dist(dwx, dwy) < 1e-6f);                /* a panel that stays, stays in both */
        } else {
            ++moved;
            CHECK(NEAR(dwx, 2.0f * dsx, 1e-5f) && NEAR(dwy, 2.0f * dsy, 1e-5f)); /* (2-1) vs (1.5-1) */
            /* Outward: the move points away from the centre of the HUD (a positive dot product
             * with the panel's own position there), and the panel keeps its size. */
            CHECK(dsx * o[i].right_m + dsy * o[i].up_m > 0.0f);
        }
    }
    CHECK(moved == 5 && still == 1);                      /* five clusters move, the middle stays */

    /* The clusters keep their corners: a panel left of the centre stays left, one above stays above,
     * however far it is spread (a panel within 2 cm of the middle axis may not cross it either). */
    for (int i = 0; i < 6; ++i) {
        CHECK((o[i].right_m > 0.02f) == (w[i].right_m > 0.02f));
        CHECK((o[i].right_m < -0.02f) == (w[i].right_m < -0.02f));
        CHECK((o[i].up_m > 0.02f) == (w[i].up_m > 0.02f));
        CHECK((o[i].up_m < -0.02f) == (w[i].up_m < -0.02f));
    }

    /* Even the widest spread keeps every panel inside a 2.6 m x 1.5 m window around the centre
     * (about +-41 degrees across and +-27 up and down at 1.5 m), i.e. inside a headset's view. */
    for (int i = 0; i < 6; ++i) {
        CHECK(fabsf(w[i].right_m) + w[i].width_m * 0.5f < 1.3f);
        CHECK(fabsf(w[i].up_m) + w[i].height_m * 0.5f < 0.75f);
    }
}

static void test_degenerate(void)
{
    VrHudPanel p[VR_HUD_MAX_PANELS];
    CHECK(vr_hud_panels(VR_HUD_SPREAD, 0, 0, 1.2f, p) == 0);
    CHECK(vr_hud_panels(VR_HUD_SPREAD, 640, 400, 0.0f, p) == 0);
    CHECK(vr_hud_panels(VR_HUD_WIDE, 32, 20, 1.2f, p) == 0);
    CHECK(vr_hud_panels(VR_HUD_ORIGINAL, 32, 20, 1.2f, p) == 0);
    CHECK(vr_hud_panels(VR_HUD_ORIGINAL, 640, 400, 1.2f, NULL) == 0);
}

int main(void)
{
    test_names();
    test_original();
    test_tiling();
    test_spread();
    test_degenerate();

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("vr_hud_layout_test: ALL PASS\n");
    return 0;
}
