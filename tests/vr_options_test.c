/* tests/vr_options_test.c -- pure-C checks of the VR options: the roller-vr.ini file and the pause
 * menu page's rows (PROJECTS/ROLLER/vr_options.c). No SDL, OpenXR or GPU. */
#include <stdio.h>
#include <string.h>

#include "../PROJECTS/ROLLER/vr_options.h"

static int g_failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void test_default(void)
{
    VrOptions o = vr_options_default();
    CHECK(o.vignette == VR_VIGNETTE_OFF);
    CHECK(o.hud == VR_HUD_ORIGINAL);
}

static void test_parse(void)
{
    VrOptions o = vr_options_default();
    CHECK(vr_options_parse("vignette=mild\n", &o) == 1 && o.vignette == VR_VIGNETTE_MILD && o.hud == VR_HUD_ORIGINAL);
    o = vr_options_default();
    CHECK(vr_options_parse("hud=wide\n", &o) == 1 && o.vignette == VR_VIGNETTE_OFF && o.hud == VR_HUD_WIDE);
    o = vr_options_default();
    CHECK(vr_options_parse("; mine\r\n  Vignette = STRONG  \r\nHUD=Spread\r\nother=1", &o) == 2);
    CHECK(o.vignette == VR_VIGNETTE_STRONG && o.hud == VR_HUD_SPREAD);

    /* The last valid line wins; an invalid value is ignored and leaves what was there. */
    o = vr_options_default();
    CHECK(vr_options_parse("vignette=mild\nvignette=strong\nhud=wide\nhud=bogus\n", &o) == 2); /* two keys, four lines */
    CHECK(o.vignette == VR_VIGNETTE_STRONG && o.hud == VR_HUD_WIDE);

    /* Nothing found: untouched, and 0. Comments and look-alike keys are not settings. */
    o.vignette = VR_VIGNETTE_MILD;
    o.hud = VR_HUD_SPREAD;
    CHECK(vr_options_parse("", &o) == 0);
    CHECK(vr_options_parse(NULL, &o) == 0);
    CHECK(vr_options_parse("; vignette=strong\n# hud=wide\nmyhud=wide\nvignette2=off\nvignette=medium\nhud=\n", &o) == 0);
    CHECK(o.vignette == VR_VIGNETTE_MILD && o.hud == VR_HUD_SPREAD);
    CHECK(vr_options_parse("vignette=off\n", NULL) == 0);
}

static void test_format(void)
{
    char out[1024];
    VrOptions o = { VR_VIGNETTE_STRONG, VR_HUD_WIDE };

    size_t n = vr_options_format(NULL, &o, out, sizeof out);
    CHECK(n > 0 && n == strlen(out));
    VrOptions back = vr_options_default();
    CHECK(vr_options_parse(out, &back) == 2 && back.vignette == o.vignette && back.hud == o.hud);

    /* Existing text: other lines kept in place, our lines replaced in place, duplicates dropped,
     * missing keys appended. */
    const char *old = "; mine\nother=1\nvignette=off\nmore=2\nvignette=mild\n";
    n = vr_options_format(old, &o, out, sizeof out);
    CHECK(n > 0);
    CHECK(strstr(out, "; mine\n") && strstr(out, "other=1\n") && strstr(out, "more=2\n"));
    CHECK(strstr(out, "vignette=strong\n") != NULL && strstr(out, "hud=wide\n") != NULL);
    CHECK(strstr(out, "vignette=off") == NULL && strstr(out, "vignette=mild") == NULL);
    CHECK(strstr(strstr(out, "vignette=strong") + 1, "vignette=") == NULL);       /* exactly one */
    CHECK(strstr(out, "other=1") < strstr(out, "vignette=strong") && strstr(out, "vignette=strong") < strstr(out, "more=2"));

    /* No trailing newline on the old text: our lines go on their own lines. */
    n = vr_options_format("other=1", &o, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "other=1\nvignette=strong\nhud=wide\n") == 0);

    /* Replacing one key keeps the other where it is. */
    VrOptions m = { VR_VIGNETTE_MILD, VR_HUD_SPREAD };
    n = vr_options_format("hud=wide\nx=1\nvignette=off\n", &m, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "hud=spread\nx=1\nvignette=mild\n") == 0);

    /* Round trip for every combination. */
    for (int v = 0; v < VR_VIGNETTE_LEVEL_COUNT; ++v)
        for (int h = 0; h < VR_HUD_LAYOUT_COUNT; ++h) {
            VrOptions w = { (VrVignetteLevel)v, (VrHudLayout)h }, r = { (VrVignetteLevel)9, (VrHudLayout)9 };
            n = vr_options_format("x=1\n", &w, out, sizeof out);
            CHECK(n > 0 && vr_options_parse(out, &r) == 2 && r.vignette == w.vignette && r.hud == w.hud);
        }

    /* A too-small buffer: nothing is claimed to be written. */
    CHECK(vr_options_format(old, &o, out, 8) == 0);
    CHECK(vr_options_format(old, &o, NULL, 0) == 0);
    CHECK(vr_options_format(old, NULL, out, sizeof out) == 0);
}

static void test_rows(void)
{
    /* Down goes vignette -> HUD layout -> exit and stops there; up comes back, and from the exit
     * row up goes to the last option: the same way the SOUND page moves. */
    CHECK(vr_options_row_down(VR_OPTIONS_ROW_VIGNETTE) == VR_OPTIONS_ROW_HUD);
    CHECK(vr_options_row_down(VR_OPTIONS_ROW_HUD) == VR_OPTIONS_ROW_EXIT);
    CHECK(vr_options_row_down(VR_OPTIONS_ROW_EXIT) == VR_OPTIONS_ROW_EXIT);
    CHECK(vr_options_row_up(VR_OPTIONS_ROW_EXIT) == VR_OPTIONS_ROW_HUD);
    CHECK(vr_options_row_up(VR_OPTIONS_ROW_HUD) == VR_OPTIONS_ROW_VIGNETTE);
    CHECK(vr_options_row_up(VR_OPTIONS_ROW_VIGNETTE) == VR_OPTIONS_ROW_VIGNETTE);
    CHECK(vr_options_row_down(77) == VR_OPTIONS_ROW_VIGNETTE && vr_options_row_up(-3) == VR_OPTIONS_ROW_VIGNETTE);

    CHECK(strcmp(vr_options_row_label(VR_OPTIONS_ROW_VIGNETTE), "VIGNETTE") == 0);
    CHECK(strcmp(vr_options_row_label(VR_OPTIONS_ROW_HUD), "HUD LAYOUT") == 0);
    CHECK(vr_options_row_label(VR_OPTIONS_ROW_EXIT) == NULL && vr_options_row_label(9) == NULL);

    VrOptions o = { VR_VIGNETTE_MILD, VR_HUD_WIDE };
    CHECK(strcmp(vr_options_row_value(&o, VR_OPTIONS_ROW_VIGNETTE), "MILD") == 0);
    CHECK(strcmp(vr_options_row_value(&o, VR_OPTIONS_ROW_HUD), "WIDE") == 0);
    o.vignette = VR_VIGNETTE_OFF;
    CHECK(strcmp(vr_options_row_value(&o, VR_OPTIONS_ROW_VIGNETTE), "OFF") == 0);
    o.vignette = VR_VIGNETTE_STRONG;
    CHECK(strcmp(vr_options_row_value(&o, VR_OPTIONS_ROW_VIGNETTE), "STRONG") == 0);
    CHECK(vr_options_row_value(&o, VR_OPTIONS_ROW_EXIT) == NULL && vr_options_row_value(NULL, 1) == NULL);
}

static void test_adjust(void)
{
    VrOptions o = vr_options_default();
    CHECK(vr_options_adjust(&o, VR_OPTIONS_ROW_VIGNETTE, +1) && o.vignette == VR_VIGNETTE_MILD && o.hud == VR_HUD_ORIGINAL);
    CHECK(vr_options_adjust(&o, VR_OPTIONS_ROW_VIGNETTE, +1) && o.vignette == VR_VIGNETTE_STRONG);
    CHECK(vr_options_adjust(&o, VR_OPTIONS_ROW_VIGNETTE, +1) && o.vignette == VR_VIGNETTE_OFF);   /* wraps */
    CHECK(vr_options_adjust(&o, VR_OPTIONS_ROW_VIGNETTE, -1) && o.vignette == VR_VIGNETTE_STRONG);
    CHECK(vr_options_adjust(&o, VR_OPTIONS_ROW_HUD, +1) && o.hud == VR_HUD_SPREAD && o.vignette == VR_VIGNETTE_STRONG);
    CHECK(vr_options_adjust(&o, VR_OPTIONS_ROW_HUD, +1) && o.hud == VR_HUD_WIDE);
    CHECK(vr_options_adjust(&o, VR_OPTIONS_ROW_HUD, +1) && o.hud == VR_HUD_ORIGINAL);
    CHECK(vr_options_adjust(&o, VR_OPTIONS_ROW_HUD, -1) && o.hud == VR_HUD_WIDE);

    /* The exit row, a bad row and a zero step change nothing. */
    VrOptions before = o;
    CHECK(!vr_options_adjust(&o, VR_OPTIONS_ROW_EXIT, +1));
    CHECK(!vr_options_adjust(&o, 7, +1));
    CHECK(!vr_options_adjust(&o, VR_OPTIONS_ROW_HUD, 0));
    CHECK(!vr_options_adjust(NULL, VR_OPTIONS_ROW_HUD, +1));
    CHECK(o.vignette == before.vignette && o.hud == before.hud);
}

int main(void)
{
    test_default();
    test_parse();
    test_format();
    test_rows();
    test_adjust();

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("vr_options_test: ALL PASS\n");
    return 0;
}
