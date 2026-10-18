/* PROJECTS/ROLLER/vr_fake_input.c -- see vr_fake_input.h */
#include "vr_fake_input.h"

#include <string.h>

#define FAKE_PRESS_MS         60u
#define FAKE_GAP_MS          240u
#define FAKE_NAV_COUNT          8
#define FAKE_SETTLE_MS       2000u
#define FAKE_DWELL_MS        4000u /* was 1500; a quad layer can roughly halve menu fps on a slow
                                     * software compositor (measured on fatal-gpu/Monado, M4), which
                                     * made the harness's own re-entry detection miss the first
                                     * nav-down press at 1500ms; 4000ms gave 3/3 clean runs there */
#define FAKE_WAIT_TIMEOUT_MS 20000u
#define FAKE_RACE_TIMEOUT_MS 60000u
#define FAKE_COUNTDOWN_MS    12000u
#define FAKE_DRIVE_MS        20500u
#define FAKE_RELEASE_MS       1000u
#define FAKE_SHOT_PRESS_MS     300u /* both thumbstick clicks: the screenshot gesture */
#define FAKE_SHOT_1_MS        3000u /* into the drive step, racing */
#define FAKE_PAUSE_MS         6000u /* Start tap: opens the pause menu */
/* In the pause menu (the game ignores driving input then): down x3 to the VR OPTIONS row, A opens the
 * page, A steps the vignette, down to HUD LAYOUT, A twice steps the layout, a screenshot of the page,
 * B goes back, Start closes the menu. The paused scene runs at about 12 frames per second on the slow
 * software compositor of the test host, so these presses are 200 ms (a 60 ms press can fall between two
 * frames and be lost) and at least 500 ms apart. */
#define FAKE_MENU_PRESS_MS    200u
#define FAKE_MENU_DOWN_1_MS   6900u
#define FAKE_MENU_DOWN_2_MS   7400u
#define FAKE_MENU_DOWN_3_MS   7900u
#define FAKE_MENU_OPEN_MS     8600u
#define FAKE_MENU_VIGNETTE_MS 9200u
#define FAKE_MENU_ROW_DOWN_MS 9800u
#define FAKE_MENU_HUD_1_MS   10400u
#define FAKE_MENU_HUD_2_MS   11000u
#define FAKE_SHOT_PAUSED_MS  11600u /* the screenshot gesture again, on the VR OPTIONS page */
#define FAKE_MENU_BACK_MS    12400u
#define FAKE_RESUME_MS       13200u /* Start tap: closes the pause menu */
#define FAKE_SHOT_2_MS       18000u /* racing again, with the options just chosen */
#define FAKE_EXIT_HOLD_MS     2600u /* > VR_SAFETY_HOLD_MS + the 250 ms grace */

enum {
    S_WAIT_MAIN, S_NAV_UP, S_ACCEPT_CAR, S_WAIT_CAR, S_BACK, S_WAIT_MAIN2,
    S_NAV_DOWN, S_ACCEPT_RACE, S_WAIT_RACE, S_COUNTDOWN, S_DRIVE, S_RELEASE,
    S_SAFETY_EXIT, S_TIMEOUT_EXIT, S_DONE
};
static const char *const k_names[] = {
    "wait-main-menu", "nav-up-x8", "accept-car-select", "wait-car-select", "back", "wait-main-menu-again",
    "nav-down-x8", "accept-start-race", "wait-racing", "countdown", "drive", "release",
    "safety-exit", "timeout-safety-exit", "done"
};

void vr_fake_input_init(VrFakeInput *f)
{
    memset(f, 0, sizeof(*f));
}

static const char *enter(VrFakeInput *f, int step, uint64_t now)
{
    f->step = step;
    f->step_start_ms = now;
    return k_names[step];
}

static bool press(uint64_t t) { return t < FAKE_PRESS_MS; }

/* true while t is within [start, start + len). */
static bool hold(uint64_t t, uint64_t start, uint64_t len) { return t >= start && t < start + len; }

/* Writes the current step's output for this frame. Returns the step to hand over
 * to, or -1 to stay. */
static int run_step(const VrFakeInput *f, VrFakeScreen screen, uint64_t now, VrInputActions *out)
{
    int next = -1;
    uint64_t t = now - f->step_start_ms;
    uint64_t on_screen = now - f->screen_since_ms;
    const uint64_t period = FAKE_PRESS_MS + FAKE_GAP_MS;

    switch (f->step) {
    case S_WAIT_MAIN:
        if (screen == VR_FAKE_SCREEN_MAIN_MENU && on_screen >= FAKE_SETTLE_MS) next = S_NAV_UP;
        else if (t > FAKE_WAIT_TIMEOUT_MS) next = S_TIMEOUT_EXIT;
        break;
    case S_NAV_UP:
        if (t / period >= FAKE_NAV_COUNT) next = S_ACCEPT_CAR;
        else out->dpad_up = (t % period) < FAKE_PRESS_MS;
        break;
    case S_ACCEPT_CAR:
        out->button_a = press(t);
        if (t >= period) next = S_WAIT_CAR;
        break;
    case S_WAIT_CAR:
        if (screen == VR_FAKE_SCREEN_CAR_SELECT && on_screen >= FAKE_DWELL_MS) next = S_BACK;
        else if (t > FAKE_WAIT_TIMEOUT_MS) next = S_TIMEOUT_EXIT;
        break;
    case S_BACK:
        out->button_b = press(t);
        if (t >= period) next = S_WAIT_MAIN2;
        break;
    case S_WAIT_MAIN2:
        if (screen == VR_FAKE_SCREEN_MAIN_MENU && on_screen >= FAKE_DWELL_MS) next = S_NAV_DOWN;
        else if (t > FAKE_WAIT_TIMEOUT_MS) next = S_TIMEOUT_EXIT;
        break;
    case S_NAV_DOWN:
        if (t / period >= FAKE_NAV_COUNT) next = S_ACCEPT_RACE;
        else out->dpad_down = (t % period) < FAKE_PRESS_MS;
        break;
    case S_ACCEPT_RACE:
        out->button_a = press(t);
        if (t >= period) next = S_WAIT_RACE;
        break;
    case S_WAIT_RACE:
        if (screen == VR_FAKE_SCREEN_RACING) next = S_COUNTDOWN;
        else if (t > FAKE_RACE_TIMEOUT_MS) next = S_TIMEOUT_EXIT;
        break;
    case S_COUNTDOWN:
        if (t >= FAKE_COUNTDOWN_MS) next = S_DRIVE;
        break;
    case S_DRIVE:
        out->right_trigger = 1.0f;
        if (t >= 4000 && t < 6000) out->left_stick_x = -0.7f;
        if (t >= 14500 && t < 17000) out->left_stick_x = 0.7f;
        if (t >= 17000 && t < 19500) out->left_stick_x = -0.7f;
        if (hold(t, FAKE_SHOT_1_MS, FAKE_SHOT_PRESS_MS) || hold(t, FAKE_SHOT_PAUSED_MS, FAKE_SHOT_PRESS_MS)
            || hold(t, FAKE_SHOT_2_MS, FAKE_SHOT_PRESS_MS))
            out->left_stick_click = out->right_stick_click = true;
        if (hold(t, FAKE_PAUSE_MS, FAKE_PRESS_MS) || hold(t, FAKE_RESUME_MS, FAKE_PRESS_MS))
            out->menu = true;
        if (hold(t, FAKE_MENU_DOWN_1_MS, FAKE_MENU_PRESS_MS) || hold(t, FAKE_MENU_DOWN_2_MS, FAKE_MENU_PRESS_MS)
            || hold(t, FAKE_MENU_DOWN_3_MS, FAKE_MENU_PRESS_MS) || hold(t, FAKE_MENU_ROW_DOWN_MS, FAKE_MENU_PRESS_MS))
            out->dpad_down = true;
        if (hold(t, FAKE_MENU_OPEN_MS, FAKE_MENU_PRESS_MS) || hold(t, FAKE_MENU_VIGNETTE_MS, FAKE_MENU_PRESS_MS)
            || hold(t, FAKE_MENU_HUD_1_MS, FAKE_MENU_PRESS_MS) || hold(t, FAKE_MENU_HUD_2_MS, FAKE_MENU_PRESS_MS))
            out->button_a = true;
        if (hold(t, FAKE_MENU_BACK_MS, FAKE_MENU_PRESS_MS))
            out->button_b = true;
        if (t >= FAKE_DRIVE_MS) next = S_RELEASE;
        break;
    case S_RELEASE:
        if (t >= FAKE_RELEASE_MS) next = S_SAFETY_EXIT;
        break;
    case S_SAFETY_EXIT:
    case S_TIMEOUT_EXIT:
        out->view = true;
        out->menu = true;
        if (t >= FAKE_EXIT_HOLD_MS) next = S_DONE;
        break;
    default:
        break;
    }
    return next;
}

const char *vr_fake_input_step(VrFakeInput *f, VrFakeScreen screen, uint64_t now, VrInputActions *out)
{
    const char *changed = NULL;
    memset(out, 0, sizeof(*out));
    if (!f->started) {
        f->started = true;
        f->last_screen = screen;
        f->screen_since_ms = now;
        changed = enter(f, S_WAIT_MAIN, now);
    }
    if (screen != f->last_screen) {
        f->last_screen = screen;
        f->screen_since_ms = now;
    }

    int next = run_step(f, screen, now, out);
    if (next >= 0) {
        /* One hand-over per frame, so every step name is logged on its own frame
         * (a step that is left at once, such as wait-racing, is still reported).
         * The entered step's first frame runs now, so a press that starts at its
         * t = 0 is not lost; its own hand-over waits for the next frame. */
        changed = enter(f, next, now);
        (void)run_step(f, screen, now, out);
    }
    return changed;
}
