/* tests/vr_input_map_test.c -- unit tests for PROJECTS/ROLLER/vr_input_map.c (pure C, no SDL/OpenXR). */
#include "vr_input_map.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TL_IN "/user/hand/left/input/"
#define TR_IN "/user/hand/right/input/"

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static VrInputEvent step(VrInputMapState *s, VrInputActions a, bool focused, bool has_back,
                         uint32_t dt, VrPadState *out)
{
    return vr_input_map_step(s, &a, focused, has_back, dt, out);
}

/* Exactly one button set (idx), or none when idx is -1. */
static void expect_only_button(const VrPadState *p, int idx)
{
    for (int i = 0; i < VR_PAD_BUTTON_COUNT; ++i)
        CHECK(p->buttons[i] == (i == idx));
}

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool path_ends(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

static bool name_ok(const char *n)
{
    if (!n || !*n)
        return false;
    for (const char *p = n; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
            return false;
    }
    return true;
}

static bool has_binding(const VrProfileBindings *p, VrActionId id, const char *path)
{
    for (int i = 0; i < p->count; ++i) {
        if (p->bindings[i].action == id && strcmp(p->bindings[i].path, path) == 0)
            return true;
    }
    return false;
}

static bool in_list(const char *s, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(s, list[i]) == 0)
            return true;
    }
    return false;
}

/* --- Per-frame mapping ------------------------------------------------------ */

static void test_rest(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions none;
    memset(&none, 0, sizeof none);
    vr_input_map_init(&s);

    CHECK(step(&s, none, true, true, 10, &out) == VR_INPUT_EVENT_NONE);
    CHECK(out.axes[VR_PAD_AXIS_LEFTX] == 0);
    CHECK(out.axes[VR_PAD_AXIS_LEFTY] == 0);
    CHECK(out.axes[VR_PAD_AXIS_RIGHTX] == 0);
    CHECK(out.axes[VR_PAD_AXIS_RIGHTY] == 0);
    CHECK(out.axes[VR_PAD_AXIS_LEFT_TRIGGER] == -32768);
    CHECK(out.axes[VR_PAD_AXIS_RIGHT_TRIGGER] == -32768);
    expect_only_button(&out, -1);
}

static void test_sticks(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions a;
    vr_input_map_init(&s);

    memset(&a, 0, sizeof a);
    a.left_stick_x = 1.0f;
    step(&s, a, true, true, 10, &out);
    CHECK(out.axes[VR_PAD_AXIS_LEFTX] == 32767);

    memset(&a, 0, sizeof a);
    a.left_stick_y = 1.0f; /* OpenXR up -> SDL up (negative) */
    step(&s, a, true, true, 10, &out);
    CHECK(out.axes[VR_PAD_AXIS_LEFTY] == -32767);

    memset(&a, 0, sizeof a);
    a.left_stick_y = -1.0f;
    step(&s, a, true, true, 10, &out);
    CHECK(out.axes[VR_PAD_AXIS_LEFTY] == 32767);

    memset(&a, 0, sizeof a);
    a.right_stick_x = 1.0f;
    step(&s, a, true, true, 10, &out);
    CHECK(out.axes[VR_PAD_AXIS_RIGHTX] == 32767);

    memset(&a, 0, sizeof a);
    a.right_stick_y = 1.0f;
    step(&s, a, true, true, 10, &out);
    CHECK(out.axes[VR_PAD_AXIS_RIGHTY] == -32767);

    memset(&a, 0, sizeof a);
    a.right_stick_y = -1.0f;
    step(&s, a, true, true, 10, &out);
    CHECK(out.axes[VR_PAD_AXIS_RIGHTY] == 32767);

    memset(&a, 0, sizeof a);
    a.left_stick_x = 1.5f; /* clamps */
    step(&s, a, true, true, 10, &out);
    CHECK(out.axes[VR_PAD_AXIS_LEFTX] == 32767);

    a.left_stick_x = -1.5f;
    step(&s, a, true, true, 10, &out);
    CHECK(out.axes[VR_PAD_AXIS_LEFTX] == -32767);

    memset(&a, 0, sizeof a);
    a.left_stick_x = NAN;
    step(&s, a, true, true, 10, &out);
    CHECK(out.axes[VR_PAD_AXIS_LEFTX] == 0);
    CHECK(vr_input_axis_from_unit(NAN) == 0);
}

static void test_triggers(void)
{
    /* Task 1 (tests/vr_virtual_pad_test.c): trigger axes rest at raw -32768 and the
     * full raw range maps to 0..32767 on the SDL gamepad. The conversion here is the
     * inverse: unit 0..1 -> raw -32768..32767. */
    CHECK(vr_input_trigger_from_unit(0.0f) == -32768);
    CHECK(vr_input_trigger_from_unit(1.0f) == 32767);
    {
        int16_t half = vr_input_trigger_from_unit(0.5f);
        CHECK(half >= -1 && half <= 0);
    }
    CHECK(vr_input_trigger_from_unit(NAN) == -32768);
    CHECK(vr_input_trigger_from_unit(2.0f) == 32767);
    CHECK(vr_input_trigger_from_unit(-1.0f) == -32768);

    VrInputMapState s;
    VrPadState out;
    VrInputActions a;
    vr_input_map_init(&s);

    memset(&a, 0, sizeof a);
    step(&s, a, true, true, 10, &out);
    CHECK(out.axes[VR_PAD_AXIS_LEFT_TRIGGER] == -32768);
    CHECK(out.axes[VR_PAD_AXIS_RIGHT_TRIGGER] == -32768);

    a.left_trigger = 1.0f;
    a.right_trigger = 0.0f;
    step(&s, a, true, true, 10, &out);
    CHECK(out.axes[VR_PAD_AXIS_LEFT_TRIGGER] == 32767);
    CHECK(out.axes[VR_PAD_AXIS_RIGHT_TRIGGER] == -32768);
}

static void test_face_buttons(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions a;
    vr_input_map_init(&s);

    memset(&a, 0, sizeof a);
    a.button_a = true;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_SOUTH);

    memset(&a, 0, sizeof a);
    a.button_b = true;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_EAST);

    memset(&a, 0, sizeof a);
    a.button_x = true;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_WEST);

    memset(&a, 0, sizeof a);
    a.button_y = true;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_NORTH);

    memset(&a, 0, sizeof a);
    a.select = true;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_SOUTH);
}

static void test_dpad_clicks_shoulders(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions a;
    vr_input_map_init(&s);

    memset(&a, 0, sizeof a);
    a.dpad_up = true;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_DPAD_UP);

    memset(&a, 0, sizeof a);
    a.dpad_down = true;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_DPAD_DOWN);

    memset(&a, 0, sizeof a);
    a.dpad_left = true;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_DPAD_LEFT);

    memset(&a, 0, sizeof a);
    a.dpad_right = true;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_DPAD_RIGHT);

    memset(&a, 0, sizeof a);
    a.left_stick_click = true;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_LEFT_STICK);

    memset(&a, 0, sizeof a);
    a.right_stick_click = true;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_RIGHT_STICK);

    memset(&a, 0, sizeof a);
    a.left_shoulder = true;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_LEFT_SHOULDER);

    memset(&a, 0, sizeof a);
    a.right_shoulder = true;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_RIGHT_SHOULDER);

    /* Everything at once (view and menu make a combo, which holds BACK/START back):
     * GUIDE must still never be set. */
    memset(&a, 0, sizeof a);
    a.button_a = a.button_b = a.button_x = a.button_y = true;
    a.left_stick_click = a.right_stick_click = true;
    a.left_shoulder = a.right_shoulder = true;
    a.dpad_up = a.dpad_down = a.dpad_left = a.dpad_right = true;
    a.select = true;
    step(&s, a, true, true, 10, &out);
    CHECK(!out.buttons[VR_PAD_BUTTON_GUIDE]);
}

static void test_squeeze_hysteresis(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions a;
    vr_input_map_init(&s);

    memset(&a, 0, sizeof a);
    a.left_squeeze = 0.5f;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, -1);

    a.left_squeeze = 0.6f;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_LEFT_SHOULDER);

    a.left_squeeze = 0.5f;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_LEFT_SHOULDER);

    /* Release band: VR_SQUEEZE_RELEASE is 0.4f and the release test is strictly
     * below it (v < 0.4f), so everything from 0.4 up stays pressed. */
    a.left_squeeze = 0.45f;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_LEFT_SHOULDER);

    a.left_squeeze = 0.41f;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_LEFT_SHOULDER);

    a.left_squeeze = 0.4f;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, VR_PAD_BUTTON_LEFT_SHOULDER);

    a.left_squeeze = 0.39f;
    step(&s, a, true, true, 10, &out);
    expect_only_button(&out, -1);
}

static void test_not_focused(void)
{
    VrInputMapState s;
    VrPadState out;
    VrPadState rest;
    VrInputActions all;
    VrInputActions both;
    int quits = 0;
    vr_input_map_init(&s);
    vr_input_pad_rest(&rest);

    /* Every input set, not focused: output is exactly the rest state, no event. */
    memset(&all, 0, sizeof all);
    all.left_stick_x = all.left_stick_y = all.right_stick_x = all.right_stick_y = 0.7f;
    all.left_trigger = all.right_trigger = all.left_squeeze = all.right_squeeze = 0.7f;
    all.button_a = all.button_b = all.button_x = all.button_y = true;
    all.menu = all.view = true;
    all.left_shoulder = all.right_shoulder = true;
    all.left_stick_click = all.right_stick_click = true;
    all.dpad_up = all.dpad_down = all.dpad_left = all.dpad_right = true;
    all.select = true;
    CHECK(step(&s, all, false, true, 10, &out) == VR_INPUT_EVENT_NONE);
    CHECK(memcmp(&out, &rest, sizeof out) == 0);

    /* A combo in progress loses focus: no event, and the count starts again from 0
     * once focus returns with both buttons still held. */
    memset(&both, 0, sizeof both);
    both.view = both.menu = true;
    step(&s, both, true, true, 10, &out); /* combo starts */
    for (int i = 0; i < 150; ++i) { /* 1500 ms, short of the hold */
        if (step(&s, both, true, true, 10, &out) == VR_INPUT_EVENT_QUIT)
            quits++;
    }
    CHECK(step(&s, both, false, true, 10, &out) == VR_INPUT_EVENT_NONE);
    CHECK(memcmp(&out, &rest, sizeof out) == 0);

    CHECK(step(&s, both, true, true, 10, &out) == VR_INPUT_EVENT_NONE); /* combo restarts */
    for (int i = 1; i <= 199; ++i) { /* 1990 ms since the restart */
        if (step(&s, both, true, true, 10, &out) == VR_INPUT_EVENT_QUIT)
            quits++;
    }
    CHECK(quits == 0);
    CHECK(step(&s, both, true, true, 10, &out) == VR_INPUT_EVENT_QUIT); /* 2000 ms */
}

/* Holds view+menu for 200 further steps of 10 ms after the starting step. Returns the
 * 1-based index of the step that quit, 0 if none. *quits counts QUIT events. Back and
 * Start must stay released throughout. */
static int run_clean_combo_hold(VrInputMapState *s, int *quits)
{
    VrInputActions both;
    VrPadState out;
    int quit_at = 0;
    memset(&both, 0, sizeof both);
    both.view = both.menu = true;
    CHECK(step(s, both, true, true, 10, &out) == VR_INPUT_EVENT_NONE);
    expect_only_button(&out, -1);
    for (int i = 1; i <= 200; ++i) {
        VrInputEvent ev = step(s, both, true, true, 10, &out);
        CHECK(!out.buttons[VR_PAD_BUTTON_BACK]);
        CHECK(!out.buttons[VR_PAD_BUTTON_START]);
        if (ev == VR_INPUT_EVENT_QUIT) {
            (*quits)++;
            if (!quit_at)
                quit_at = i;
        }
    }
    return quit_at;
}

static void test_clean_combo_quits_once(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions none;
    int quits = 0;
    vr_input_map_init(&s);
    memset(&none, 0, sizeof none);

    CHECK(run_clean_combo_hold(&s, &quits) == 200);
    CHECK(quits == 1);

    /* Holding 500 ms longer does not quit again. */
    for (int i = 0; i < 50; ++i) {
        if (step(&s, (VrInputActions){ .view = true, .menu = true }, true, true, 10, &out) != VR_INPUT_EVENT_NONE)
            quits++;
    }
    CHECK(quits == 1);
    CHECK(step(&s, none, true, true, 10, &out) == VR_INPUT_EVENT_NONE);
}

static void test_latch_after_combo(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions a;
    int quits = 0;
    vr_input_map_init(&s);
    run_clean_combo_hold(&s, &quits);
    CHECK(quits == 1);

    /* Release view only: no event, START stays suppressed while menu is still held. */
    memset(&a, 0, sizeof a);
    a.menu = true;
    for (int i = 0; i < 10; ++i) {
        CHECK(step(&s, a, true, true, 10, &out) == VR_INPUT_EVENT_NONE);
        CHECK(!out.buttons[VR_PAD_BUTTON_START]);
    }

    /* Release menu, then press it alone: START only after the 250 ms grace. */
    memset(&a, 0, sizeof a);
    CHECK(step(&s, a, true, true, 10, &out) == VR_INPUT_EVENT_NONE);
    memset(&a, 0, sizeof a);
    a.menu = true;
    for (int i = 1; i <= 24; ++i) {
        step(&s, a, true, true, 10, &out);
        CHECK(!out.buttons[VR_PAD_BUTTON_START]);
    }
    step(&s, a, true, true, 10, &out);
    CHECK(out.buttons[VR_PAD_BUTTON_START]);
    CHECK(quits == 1);
}

static void test_recenter(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions both;
    VrInputActions none;
    int recenters = 0;
    int quits = 0;
    vr_input_map_init(&s);
    memset(&both, 0, sizeof both);
    memset(&none, 0, sizeof none);
    both.view = both.menu = true;

    step(&s, both, true, true, 10, &out); /* combo starts */
    for (int i = 0; i < 50; ++i) {        /* held to 500 ms */
        if (step(&s, both, true, true, 10, &out) == VR_INPUT_EVENT_QUIT)
            quits++;
        CHECK(!out.buttons[VR_PAD_BUTTON_BACK]);
        CHECK(!out.buttons[VR_PAD_BUTTON_START]);
    }
    if (step(&s, none, true, true, 10, &out) == VR_INPUT_EVENT_RECENTER)
        recenters++;
    CHECK(!out.buttons[VR_PAD_BUTTON_BACK]);
    CHECK(!out.buttons[VR_PAD_BUTTON_START]);
    for (int i = 0; i < 20; ++i) {
        if (step(&s, none, true, true, 10, &out) != VR_INPUT_EVENT_NONE)
            recenters++;
    }
    CHECK(recenters == 1);
    CHECK(quits == 0);
}

static void test_unclean_combo(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions view;
    VrInputActions both;
    VrInputActions none;
    int recenters = 0;
    int quits = 0;
    int quit_at = 0;
    VrInputEvent ev;
    memset(&view, 0, sizeof view);
    memset(&both, 0, sizeof both);
    memset(&none, 0, sizeof none);
    view.view = true;
    both.view = both.menu = true;

    /* (a) view held 400 ms (BACK from the 250 ms step), then menu; released after 1000 ms: no RECENTER. */
    vr_input_map_init(&s);
    for (int i = 1; i <= 40; ++i) {
        step(&s, view, true, true, 10, &out);
        CHECK(out.buttons[VR_PAD_BUTTON_BACK] == (i >= 25));
    }
    ev = step(&s, both, true, true, 10, &out); /* combo starts, not clean */
    CHECK(ev == VR_INPUT_EVENT_NONE);
    CHECK(!out.buttons[VR_PAD_BUTTON_BACK]);
    CHECK(!out.buttons[VR_PAD_BUTTON_START]);
    for (int i = 1; i <= 100; ++i) { /* 1000 ms */
        ev = step(&s, both, true, true, 10, &out);
        CHECK(ev == VR_INPUT_EVENT_NONE);
        CHECK(!out.buttons[VR_PAD_BUTTON_BACK]);
        CHECK(!out.buttons[VR_PAD_BUTTON_START]);
    }
    ev = step(&s, none, true, true, 10, &out);
    CHECK(ev == VR_INPUT_EVENT_NONE);

    /* (b) the same, but held 2000 ms: QUIT, once, and no RECENTER on release. */
    vr_input_map_init(&s);
    for (int i = 1; i <= 40; ++i)
        step(&s, view, true, true, 10, &out);
    step(&s, both, true, true, 10, &out); /* combo starts */
    for (int i = 1; i <= 200; ++i) {
        ev = step(&s, both, true, true, 10, &out);
        CHECK(!out.buttons[VR_PAD_BUTTON_BACK]);
        CHECK(!out.buttons[VR_PAD_BUTTON_START]);
        if (ev == VR_INPUT_EVENT_QUIT) {
            quits++;
            if (!quit_at)
                quit_at = i;
        }
        if (ev == VR_INPUT_EVENT_RECENTER)
            recenters++;
    }
    CHECK(quits == 1);
    CHECK(quit_at == 200);
    ev = step(&s, none, true, true, 10, &out);
    CHECK(ev == VR_INPUT_EVENT_NONE);
    CHECK(recenters == 0);
}

static void test_lone_tap(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions a;
    VrInputActions none;
    vr_input_map_init(&s);
    memset(&a, 0, sizeof a);
    memset(&none, 0, sizeof none);
    a.menu = true;

    for (int i = 0; i < 10; ++i) { /* 100 ms down */
        step(&s, a, true, true, 10, &out);
        CHECK(!out.buttons[VR_PAD_BUTTON_START]);
    }
    CHECK(step(&s, none, true, true, 10, &out) == VR_INPUT_EVENT_NONE);
    CHECK(out.buttons[VR_PAD_BUTTON_START]); /* release step delivers the tap */
    for (int i = 1; i <= 4; ++i) { /* the pulse lasts VR_TAP_PULSE_MS = 50 ms */
        step(&s, none, true, true, 10, &out);
        CHECK(out.buttons[VR_PAD_BUTTON_START]);
    }
    for (int i = 0; i < 5; ++i) { /* gone from 50 ms on */
        step(&s, none, true, true, 10, &out);
        CHECK(!out.buttons[VR_PAD_BUTTON_START]);
    }
}

static void test_lone_hold(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions a;
    VrInputActions none;
    vr_input_map_init(&s);
    memset(&a, 0, sizeof a);
    memset(&none, 0, sizeof none);
    a.menu = true;

    for (int i = 1; i <= 24; ++i) { /* 240 ms */
        step(&s, a, true, true, 10, &out);
        CHECK(!out.buttons[VR_PAD_BUTTON_START]);
    }
    step(&s, a, true, true, 10, &out); /* 250 ms */
    CHECK(out.buttons[VR_PAD_BUTTON_START]);
    for (int i = 0; i < 5; ++i) {
        step(&s, a, true, true, 10, &out);
        CHECK(out.buttons[VR_PAD_BUTTON_START]);
    }
    CHECK(step(&s, none, true, true, 10, &out) == VR_INPUT_EVENT_NONE);
    CHECK(!out.buttons[VR_PAD_BUTTON_START]); /* release: no extra pulse */
    step(&s, none, true, true, 10, &out);
    CHECK(!out.buttons[VR_PAD_BUTTON_START]);
}

static void test_no_back_profile(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions a;
    VrInputActions none;
    int quits = 0;
    int recenters = 0;
    vr_input_map_init(&s);
    memset(&a, 0, sizeof a);
    memset(&none, 0, sizeof none);
    a.menu = true;

    CHECK(step(&s, a, true, false, 10, &out) == VR_INPUT_EVENT_NONE);
    CHECK(out.buttons[VR_PAD_BUTTON_START]); /* immediate, no grace without Back */
    for (int i = 2; i <= 200; ++i) {         /* 2000 ms */
        VrInputEvent ev = step(&s, a, true, false, 10, &out);
        CHECK(out.buttons[VR_PAD_BUTTON_START]);
        if (ev == VR_INPUT_EVENT_QUIT) {
            quits++;
            CHECK(i == 200);
        }
        if (ev == VR_INPUT_EVENT_RECENTER)
            recenters++;
    }
    for (int i = 0; i < 100; ++i) { /* still held: no second QUIT */
        VrInputEvent ev = step(&s, a, true, false, 10, &out);
        if (ev == VR_INPUT_EVENT_QUIT)
            quits++;
    }
    CHECK(quits == 1);
    CHECK(recenters == 0);

    /* View passes straight through to BACK on a profile without Back. */
    memset(&a, 0, sizeof a);
    a.view = true;
    CHECK(step(&s, a, true, false, 10, &out) == VR_INPUT_EVENT_NONE);
    CHECK(out.buttons[VR_PAD_BUTTON_BACK]);
    CHECK(!out.buttons[VR_PAD_BUTTON_START]);
}

static void test_dt_clamp(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions both;
    int quits = 0;
    vr_input_map_init(&s);
    memset(&both, 0, sizeof both);
    both.view = both.menu = true;

    step(&s, both, true, true, 10, &out); /* combo starts */
    CHECK(step(&s, both, true, true, 5000, &out) == VR_INPUT_EVENT_NONE); /* counts 100 ms */
    for (int i = 1; i <= 189; ++i) { /* 100 + 1890 = 1990 ms */
        if (step(&s, both, true, true, 10, &out) == VR_INPUT_EVENT_QUIT)
            quits++;
    }
    CHECK(quits == 0);
    CHECK(step(&s, both, true, true, 10, &out) == VR_INPUT_EVENT_QUIT); /* 2000 ms */
}

/* --- Binding table ----------------------------------------------------------- */

/* Valve Frame components, copied from SteamFrameControllerProfile.cs (the plan's
 * Task 2 list). dpad_* and view are left-hand only; menu is right-hand only. */
static const char *const k_valve_left[] = {
    TL_IN "thumbstick", TL_IN "thumbstick/click", TL_IN "trigger/value", TL_IN "squeeze/value",
    TL_IN "shoulder/click", TL_IN "dpad_up/click", TL_IN "dpad_down/click",
    TL_IN "dpad_left/click", TL_IN "dpad_right/click", TL_IN "view/click",
};
static const char *const k_valve_right[] = {
    TR_IN "thumbstick", TR_IN "thumbstick/click", TR_IN "trigger/value", TR_IN "squeeze/value",
    TR_IN "shoulder/click", TR_IN "a/click", TR_IN "b/click", TR_IN "x/click", TR_IN "y/click",
    TR_IN "menu/click",
};

static void test_binding_names(void)
{
    for (int i = 0; i < VR_ACT_COUNT; ++i) {
        const char *ni = vr_input_action_descs[i].name;
        CHECK(name_ok(ni));
        CHECK(vr_input_action_descs[i].localized != NULL && vr_input_action_descs[i].localized[0] != '\0');
        for (int j = i + 1; j < VR_ACT_COUNT; ++j) {
            const char *nj = vr_input_action_descs[j].name;
            CHECK(ni && nj && strcmp(ni, nj) != 0);
        }
    }
}

static void test_binding_profiles(void)
{
    for (int p = 0; p < VR_PROFILE_COUNT; ++p) {
        const VrProfileBindings *pr = &vr_input_profiles[p];
        CHECK(pr->profile != NULL);
        CHECK(pr->count > 0);
        for (int i = 0; i < pr->count; ++i) {
            const VrBinding *b = &pr->bindings[i];
            CHECK((int)b->action < VR_ACT_COUNT);
            if ((int)b->action >= VR_ACT_COUNT)
                continue;
            const char *path = b->path;
            VrActionKind want;
            if (path_ends(path, "/value"))
                want = VR_ACT_KIND_FLOAT;
            else if (path_ends(path, "/click"))
                want = VR_ACT_KIND_BOOL;
            else if (path_ends(path, "thumbstick"))
                want = VR_ACT_KIND_VEC2;
            else {
                CHECK(false); /* unrecognised binding path suffix */
                continue;
            }
            CHECK(vr_input_action_descs[b->action].kind == want);

            for (int j = i + 1; j < pr->count; ++j) {
                const VrBinding *c = &pr->bindings[j];
                CHECK(!(c->action == b->action && strcmp(c->path, path) == 0));
                CHECK(strcmp(c->path, path) != 0);
            }
        }
    }
}

static void test_valve_profile(void)
{
    const VrProfileBindings *v = &vr_input_profiles[0];
    bool bound[VR_ACT_COUNT];
    memset(bound, 0, sizeof bound);

    CHECK(strcmp(v->profile, VR_XR_VALVE_FRAME_PROFILE) == 0);
    CHECK(v->required_extension != NULL);
    CHECK(v->required_extension && strcmp(v->required_extension, VR_XR_VALVE_FRAME_EXTENSION_NAME) == 0);
    CHECK(v->has_back);
    CHECK(v->count == 20);

    for (int i = 0; i < v->count; ++i) {
        const VrBinding *b = &v->bindings[i];
        const char *path = b->path;
        if (starts_with(path, TL_IN))
            CHECK(in_list(path, k_valve_left, sizeof k_valve_left / sizeof k_valve_left[0]));
        else if (starts_with(path, TR_IN))
            CHECK(in_list(path, k_valve_right, sizeof k_valve_right / sizeof k_valve_right[0]));
        else
            CHECK(false); /* valve binding on an unexpected hand */
        bound[b->action] = true;
    }
    for (int a = 0; a < VR_ACT_COUNT; ++a) {
        if (a == VR_ACT_SELECT)
            continue;
        CHECK(bound[a]);
    }

    /* Each (action, component) pair is written out here, not read back from the
     * table. Components are the SteamFrameControllerProfile.cs list (the k_valve_*
     * arrays above); the action on each is the one its name says. */
    /* Left hand. */
    CHECK(has_binding(v, VR_ACT_LEFT_STICK, TL_IN "thumbstick"));
    CHECK(has_binding(v, VR_ACT_LEFT_TRIGGER, TL_IN "trigger/value"));
    CHECK(has_binding(v, VR_ACT_LEFT_SQUEEZE, TL_IN "squeeze/value"));
    CHECK(has_binding(v, VR_ACT_LEFT_SHOULDER, TL_IN "shoulder/click"));
    CHECK(has_binding(v, VR_ACT_LEFT_STICK_CLICK, TL_IN "thumbstick/click"));
    CHECK(has_binding(v, VR_ACT_VIEW, TL_IN "view/click"));
    CHECK(has_binding(v, VR_ACT_DPAD_UP, TL_IN "dpad_up/click"));
    CHECK(has_binding(v, VR_ACT_DPAD_DOWN, TL_IN "dpad_down/click"));
    CHECK(has_binding(v, VR_ACT_DPAD_LEFT, TL_IN "dpad_left/click"));
    CHECK(has_binding(v, VR_ACT_DPAD_RIGHT, TL_IN "dpad_right/click"));
    /* Right hand. */
    CHECK(has_binding(v, VR_ACT_RIGHT_STICK, TR_IN "thumbstick"));
    CHECK(has_binding(v, VR_ACT_RIGHT_TRIGGER, TR_IN "trigger/value"));
    CHECK(has_binding(v, VR_ACT_RIGHT_SQUEEZE, TR_IN "squeeze/value"));
    CHECK(has_binding(v, VR_ACT_RIGHT_SHOULDER, TR_IN "shoulder/click"));
    CHECK(has_binding(v, VR_ACT_RIGHT_STICK_CLICK, TR_IN "thumbstick/click"));
    CHECK(has_binding(v, VR_ACT_A, TR_IN "a/click"));
    CHECK(has_binding(v, VR_ACT_B, TR_IN "b/click"));
    CHECK(has_binding(v, VR_ACT_X, TR_IN "x/click"));
    CHECK(has_binding(v, VR_ACT_Y, TR_IN "y/click"));
    CHECK(has_binding(v, VR_ACT_MENU, TR_IN "menu/click"));
}

static void test_touch_profile(void)
{
    const VrProfileBindings *t = &vr_input_profiles[1];
    CHECK(strcmp(t->profile, VR_XR_TOUCH_PROFILE) == 0);
    CHECK(t->required_extension == NULL);
    CHECK(!t->has_back);

    for (int i = 0; i < t->count; ++i) {
        const VrBinding *b = &t->bindings[i];
        CHECK(b->action != VR_ACT_VIEW);
        CHECK(b->action != VR_ACT_DPAD_UP);
        CHECK(b->action != VR_ACT_DPAD_DOWN);
        CHECK(b->action != VR_ACT_DPAD_LEFT);
        CHECK(b->action != VR_ACT_DPAD_RIGHT);
        if (b->action == VR_ACT_X || b->action == VR_ACT_Y || b->action == VR_ACT_MENU)
            CHECK(starts_with(b->path, TL_IN));
        if (b->action == VR_ACT_A || b->action == VR_ACT_B)
            CHECK(starts_with(b->path, TR_IN));
    }
    CHECK(has_binding(t, VR_ACT_X, TL_IN "x/click"));
    CHECK(has_binding(t, VR_ACT_Y, TL_IN "y/click"));
    CHECK(has_binding(t, VR_ACT_MENU, TL_IN "menu/click"));
    CHECK(has_binding(t, VR_ACT_A, TR_IN "a/click"));
    CHECK(has_binding(t, VR_ACT_B, TR_IN "b/click"));
}

static void test_simple_profile(void)
{
    const VrProfileBindings *m = &vr_input_profiles[2];
    CHECK(strcmp(m->profile, VR_XR_SIMPLE_PROFILE) == 0);
    CHECK(m->required_extension == NULL);
    CHECK(!m->has_back);
    CHECK(m->count == 4);
    for (int i = 0; i < m->count; ++i) {
        const VrBinding *b = &m->bindings[i];
        CHECK((b->action == VR_ACT_SELECT && path_ends(b->path, "select/click")) ||
              (b->action == VR_ACT_MENU && path_ends(b->path, "menu/click")));
    }
}

static void test_actions_set(void)
{
    VrInputActions a;
    VrInputActions before;
    memset(&a, 0, sizeof a);

    vr_input_actions_set(&a, VR_ACT_LEFT_STICK, 0.25f, -0.5f);
    CHECK(a.left_stick_x == 0.25f && a.left_stick_y == -0.5f);
    vr_input_actions_set(&a, VR_ACT_RIGHT_STICK, -1.0f, 1.0f);
    CHECK(a.right_stick_x == -1.0f && a.right_stick_y == 1.0f);
    vr_input_actions_set(&a, VR_ACT_LEFT_TRIGGER, 0.7f, 0.0f);
    CHECK(a.left_trigger == 0.7f);
    vr_input_actions_set(&a, VR_ACT_RIGHT_SQUEEZE, 0.3f, 0.0f);
    CHECK(a.right_squeeze == 0.3f);

    vr_input_actions_set(&a, VR_ACT_A, 0.49f, 0.0f);
    CHECK(!a.button_a);
    vr_input_actions_set(&a, VR_ACT_A, 0.5f, 0.0f);
    CHECK(a.button_a);
    vr_input_actions_set(&a, VR_ACT_A, 0.0f, 0.0f);
    CHECK(!a.button_a);
    vr_input_actions_set(&a, VR_ACT_MENU, 1.0f, 0.0f);
    CHECK(a.menu);
    vr_input_actions_set(&a, VR_ACT_SELECT, 1.0f, 0.0f);
    CHECK(a.select);

    /* Out-of-range ids are ignored; NULL is tolerated. */
    memset(&a, 0, sizeof a);
    before = a;
    vr_input_actions_set(&a, (VrActionId)VR_ACT_COUNT, 1.0f, 1.0f);
    vr_input_actions_set(&a, (VrActionId)999, 1.0f, 1.0f);
    CHECK(memcmp(&a, &before, sizeof a) == 0);
    vr_input_actions_set(NULL, VR_ACT_A, 1.0f, 1.0f);
}

/* Steps one frame with only `a` set and checks that exactly button `idx` is pressed. */
static void expect_pressed(VrInputMapState *s, VrInputActions a, bool has_back, int idx)
{
    VrPadState out;
    step(s, a, true, has_back, 10, &out);
    expect_only_button(&out, idx);
}

/* Pad indices as literal integers, so swapping two enum values in vr_input_map.h
 * fails here as well as on the names. The numbers are SDL3's SDL_GamepadButton and
 * SDL_GamepadAxis values (SDL_gamepad.h): buttons SOUTH 0, EAST 1, WEST 2, NORTH 3,
 * BACK 4, GUIDE 5, START 6, LEFT_STICK 7, RIGHT_STICK 8, LEFT_SHOULDER 9,
 * RIGHT_SHOULDER 10, DPAD_UP 11, DPAD_DOWN 12, DPAD_LEFT 13, DPAD_RIGHT 14; axes
 * LEFTX 0, LEFTY 1, RIGHTX 2, RIGHTY 3, LEFT_TRIGGER 4, RIGHT_TRIGGER 5. */
static void test_pad_index_literals(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions all;
    vr_input_map_init(&s);

    /* Face buttons (has_back true, so no grace window on them). select is SOUTH. */
    expect_pressed(&s, (VrInputActions){ .button_a = true }, true, 0);
    expect_pressed(&s, (VrInputActions){ .button_b = true }, true, 1);
    expect_pressed(&s, (VrInputActions){ .button_x = true }, true, 2);
    expect_pressed(&s, (VrInputActions){ .button_y = true }, true, 3);
    expect_pressed(&s, (VrInputActions){ .select = true }, true, 0);

    /* BACK and START, on a profile without Back so neither waits out the grace window. */
    expect_pressed(&s, (VrInputActions){ .view = true }, false, 4);
    expect_pressed(&s, (VrInputActions){ .menu = true }, false, 6);

    /* Stick clicks, shoulders, d-pad. */
    expect_pressed(&s, (VrInputActions){ .left_stick_click = true }, true, 7);
    expect_pressed(&s, (VrInputActions){ .right_stick_click = true }, true, 8);
    expect_pressed(&s, (VrInputActions){ .left_shoulder = true }, true, 9);
    expect_pressed(&s, (VrInputActions){ .right_shoulder = true }, true, 10);
    expect_pressed(&s, (VrInputActions){ .dpad_up = true }, true, 11);
    expect_pressed(&s, (VrInputActions){ .dpad_down = true }, true, 12);
    expect_pressed(&s, (VrInputActions){ .dpad_left = true }, true, 13);
    expect_pressed(&s, (VrInputActions){ .dpad_right = true }, true, 14);

    /* Axes. OpenXR up is SDL negative on the Y axes, as in test_sticks. */
    step(&s, (VrInputActions){ .left_stick_x = 1.0f }, true, true, 10, &out);
    CHECK(out.axes[0] == 32767);
    step(&s, (VrInputActions){ .left_stick_y = 1.0f }, true, true, 10, &out);
    CHECK(out.axes[1] == -32767);
    step(&s, (VrInputActions){ .right_stick_x = 1.0f }, true, true, 10, &out);
    CHECK(out.axes[2] == 32767);
    step(&s, (VrInputActions){ .right_stick_y = 1.0f }, true, true, 10, &out);
    CHECK(out.axes[3] == -32767);
    step(&s, (VrInputActions){ .left_trigger = 1.0f }, true, true, 10, &out);
    CHECK(out.axes[4] == 32767);
    step(&s, (VrInputActions){ .right_trigger = 1.0f }, true, true, 10, &out);
    CHECK(out.axes[5] == 32767);

    /* GUIDE (5) is never set, even with every button input held. */
    memset(&all, 0, sizeof all);
    all.button_a = all.button_b = all.button_x = all.button_y = true;
    all.left_stick_click = all.right_stick_click = true;
    all.left_shoulder = all.right_shoulder = true;
    all.dpad_up = all.dpad_down = all.dpad_left = all.dpad_right = true;
    all.select = true;
    all.menu = all.view = true;
    step(&s, all, true, true, 10, &out);
    CHECK(!out.buttons[5]);
}

/* --- Screenshot gesture (both thumbstick clicks) ------------------------------ */

static void test_screenshot_combo(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions none, left, both;
    memset(&none, 0, sizeof none);
    memset(&left, 0, sizeof left);
    memset(&both, 0, sizeof both);
    left.left_stick_click = true;
    both.left_stick_click = both.right_stick_click = true;

    for (int has_back = 0; has_back <= 1; ++has_back) { /* Touch (no Back) and Frame alike */
        vr_input_map_init(&s);

        /* One click alone is not the gesture, and still reaches the pad. */
        CHECK(step(&s, left, true, has_back, 10, &out) == VR_INPUT_EVENT_NONE);
        expect_only_button(&out, VR_PAD_BUTTON_LEFT_STICK);

        /* The second click completes it: one event, whichever click came first, and the
         * clicks stay on the pad (the game binds neither). Holding does not repeat it. */
        CHECK(step(&s, both, true, has_back, 10, &out) == VR_INPUT_EVENT_SCREENSHOT);
        CHECK(out.buttons[VR_PAD_BUTTON_LEFT_STICK] && out.buttons[VR_PAD_BUTTON_RIGHT_STICK]);
        CHECK(step(&s, both, true, has_back, 10, &out) == VR_INPUT_EVENT_NONE);
        CHECK(step(&s, both, true, has_back, 100, &out) == VR_INPUT_EVENT_NONE);

        /* Letting go of one click and pressing it again while the other is still down does
         * not fire again: the gesture re-arms only once both are released. */
        CHECK(step(&s, left, true, has_back, 10, &out) == VR_INPUT_EVENT_NONE);
        CHECK(step(&s, both, true, has_back, 10, &out) == VR_INPUT_EVENT_NONE);
        CHECK(step(&s, none, true, has_back, 10, &out) == VR_INPUT_EVENT_NONE);
        CHECK(step(&s, both, true, has_back, 10, &out) == VR_INPUT_EVENT_SCREENSHOT);
    }

    /* Not focused: nothing fires, and nothing carries over the focus loss. */
    vr_input_map_init(&s);
    CHECK(step(&s, both, false, true, 10, &out) == VR_INPUT_EVENT_NONE);
    expect_only_button(&out, -1);

    /* The Back+Start safety exit still quits, once, with both clicks held through it. */
    vr_input_map_init(&s);
    VrInputActions all = both;
    all.menu = all.view = true;
    int quits = 0, shots = 0;
    for (int i = 0; i < 250; ++i) {
        VrInputEvent ev = step(&s, all, true, true, 10, &out);
        quits += ev == VR_INPUT_EVENT_QUIT;
        shots += ev == VR_INPUT_EVENT_SCREENSHOT;
    }
    CHECK(quits == 1);
    CHECK(shots <= 1);
}

/* --- Comfort gesture (left thumbstick click alone) ------------------------------ */

static void test_comfort_gesture(void)
{
    VrInputMapState s;
    VrPadState out;
    VrInputActions none, left, right, both;
    memset(&none, 0, sizeof none);
    memset(&left, 0, sizeof left);
    memset(&right, 0, sizeof right);
    memset(&both, 0, sizeof both);
    left.left_stick_click = true;
    right.right_stick_click = true;
    both.left_stick_click = both.right_stick_click = true;

    for (int has_back = 0; has_back <= 1; ++has_back) {
        vr_input_map_init(&s);

        /* A lone left click waits out VR_COMFORT_GRACE_MS (a partner click may be coming), then
         * fires once; the click itself still reaches the pad. Holding does not repeat it. */
        CHECK(step(&s, left, true, has_back, 100, &out) == VR_INPUT_EVENT_NONE);
        CHECK(step(&s, left, true, has_back, 90, &out) == VR_INPUT_EVENT_NONE);
        CHECK(step(&s, left, true, has_back, 20, &out) == VR_INPUT_EVENT_COMFORT);
        expect_only_button(&out, VR_PAD_BUTTON_LEFT_STICK);
        for (int i = 0; i < 5; ++i)
            CHECK(step(&s, left, true, has_back, 1000, &out) == VR_INPUT_EVENT_NONE);

        /* Releasing re-arms it. */
        CHECK(step(&s, none, true, has_back, 10, &out) == VR_INPUT_EVENT_NONE);
        int fired = 0;
        for (int i = 0; i < 4; ++i)
            fired += step(&s, left, true, has_back, 100, &out) == VR_INPUT_EVENT_COMFORT;
        CHECK(fired == 1);

        /* The second click inside the grace time makes it the screenshot gesture instead, and
         * the comfort event never follows, not even once the right click is let go again. */
        vr_input_map_init(&s);
        CHECK(step(&s, left, true, has_back, 100, &out) == VR_INPUT_EVENT_NONE);
        CHECK(step(&s, both, true, has_back, 10, &out) == VR_INPUT_EVENT_SCREENSHOT);
        int comfort = 0;
        for (int i = 0; i < 6; ++i)
            comfort += step(&s, both, true, has_back, 100, &out) == VR_INPUT_EVENT_COMFORT;
        for (int i = 0; i < 6; ++i)
            comfort += step(&s, left, true, has_back, 100, &out) == VR_INPUT_EVENT_COMFORT;
        CHECK(comfort == 0);
        /* Only after both are up is a lone left click a comfort gesture again. */
        CHECK(step(&s, none, true, has_back, 10, &out) == VR_INPUT_EVENT_NONE);
        fired = 0;
        for (int i = 0; i < 4; ++i)
            fired += step(&s, left, true, has_back, 100, &out) == VR_INPUT_EVENT_COMFORT;
        CHECK(fired == 1);

        /* The right click alone is nothing. */
        vr_input_map_init(&s);
        for (int i = 0; i < 20; ++i)
            CHECK(step(&s, right, true, has_back, 100, &out) == VR_INPUT_EVENT_NONE);
    }

    /* Not focused: nothing fires and nothing builds up towards it. */
    vr_input_map_init(&s);
    for (int i = 0; i < 10; ++i)
        CHECK(step(&s, left, false, true, 100, &out) == VR_INPUT_EVENT_NONE);
    CHECK(step(&s, left, true, true, 100, &out) == VR_INPUT_EVENT_NONE);
}

int main(void)
{
    test_rest();
    test_sticks();
    test_triggers();
    test_face_buttons();
    test_dpad_clicks_shoulders();
    test_squeeze_hysteresis();
    test_not_focused();
    test_clean_combo_quits_once();
    test_latch_after_combo();
    test_recenter();
    test_unclean_combo();
    test_lone_tap();
    test_lone_hold();
    test_no_back_profile();
    test_dt_clamp();
    test_binding_names();
    test_binding_profiles();
    test_valve_profile();
    test_touch_profile();
    test_simple_profile();
    test_actions_set();
    test_pad_index_literals();
    test_screenshot_combo();
    test_comfort_gesture();

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("vr_input_map_test: ALL PASS\n");
    return 0;
}
