/* tests/vr_fake_input_test.c -- unit tests for PROJECTS/ROLLER/vr_fake_input.c (pure C, no SDL).
 * A simulated game answers the script's presses in 10 ms steps. */
#include "vr_fake_input.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define SIM_STEP_MS   10u
#define SIM_LIMIT_MS  120000u
#define SIM_TITLE_MS   1000u   /* the game shows the main menu from this time on */
#define MAX_NAMES        32
#define MAX_RISES        64

typedef struct {
    const char *names[MAX_NAMES];   /* step names, in the order they were entered */
    uint64_t name_ms[MAX_NAMES];
    int n_names;
    char rises[MAX_RISES + 1];      /* rising edges: U dpad_up, D dpad_down, A, B */
    uint64_t rise_at[MAX_RISES];    /* when each of them happened (ms) */
    int n_rises;
    int press_count;                /* presses seen; press_ms[] holds how long each lasted */
    uint64_t press_ms[MAX_RISES];
    int drive_frames, drive_bad;    /* steady drive frames, and those with wrong values */
    int stray_trigger;              /* right_trigger set outside the drive step */
    int shot_frames, stray_shot;    /* both thumbstick clicks inside / outside the drive step's three windows */
    int pause_frames;               /* Start alone, inside the drive step's two pause/resume taps */
    int stray_hold;                 /* view or menu set outside the exit steps */
    uint64_t hold_run_ms, hold_best_ms, hold_start_ms, hold_best_start_ms;
    uint64_t countdown_ms, drive_ms, release_ms, exit_ms;
    bool done, timed_out;
} Trace;

/* The game: A on the main menu opens car select (first accept) and starts the
 * race (second accept); B on car select returns to the main menu. */
typedef struct {
    VrFakeScreen screen;
    int accepts;
    bool stuck_on_main;             /* case 2: accepting a car never reaches car select */
} SimGame;

typedef struct {
    bool down;
    uint64_t rise_ms;
} Button;

static void push_name(Trace *tr, const char *name, uint64_t now)
{
    if (tr->n_names < MAX_NAMES) {
        tr->names[tr->n_names] = name;
        tr->name_ms[tr->n_names] = now;
        tr->n_names++;
    }
}

/* Tracks one button across frames. Returns true on the frame it goes up. */
static bool track(Button *b, bool down, uint64_t now, char mark, Trace *tr)
{
    bool fell = false;
    if (down && !b->down) {
        b->rise_ms = now;
        if (tr->n_rises < MAX_RISES) {
            tr->rise_at[tr->n_rises] = now;
            tr->rises[tr->n_rises++] = mark;
        }
    }
    if (!down && b->down) {
        if (tr->press_count < MAX_RISES)
            tr->press_ms[tr->press_count] = now - b->rise_ms;
        tr->press_count++;
        fell = true;
    }
    b->down = down;
    return fell;
}

static void game_react(SimGame *g, bool fell_a, bool fell_b)
{
    if (fell_a && g->screen == VR_FAKE_SCREEN_MAIN_MENU) {
        if (g->accepts == 0 && !g->stuck_on_main)
            g->screen = VR_FAKE_SCREEN_CAR_SELECT;
        else if (g->accepts == 1)
            g->screen = VR_FAKE_SCREEN_RACING;
        g->accepts++;
    }
    if (fell_b && g->screen == VR_FAKE_SCREEN_CAR_SELECT)
        g->screen = VR_FAKE_SCREEN_MAIN_MENU;
}

static bool is_exit_step(const char *s)
{
    return strcmp(s, "safety-exit") == 0 || strcmp(s, "timeout-safety-exit") == 0 || strcmp(s, "done") == 0;
}

/* Runs the script against the simulated game until "done" (or the time limit). */
static void run_sim(bool stuck_on_main, Trace *tr)
{
    VrFakeInput f;
    SimGame g = { VR_FAKE_SCREEN_OTHER, 0, stuck_on_main };
    Button up = { false, 0 }, down = { false, 0 }, a = { false, 0 }, b = { false, 0 };
    const char *cur = "";

    memset(tr, 0, sizeof(*tr));
    vr_fake_input_init(&f);

    for (uint64_t now = 0; now <= SIM_LIMIT_MS && !tr->done; now += SIM_STEP_MS) {
        if (g.screen == VR_FAKE_SCREEN_OTHER && now >= SIM_TITLE_MS)
            g.screen = VR_FAKE_SCREEN_MAIN_MENU;

        VrInputActions in;
        const char *prev = cur;
        const char *ch = vr_fake_input_step(&f, g.screen, now, &in);
        if (ch) {
            cur = ch;
            push_name(tr, ch, now);
            if (strcmp(ch, "countdown") == 0) tr->countdown_ms = now;
            if (strcmp(ch, "drive") == 0) tr->drive_ms = now;
            if (strcmp(ch, "release") == 0) tr->release_ms = now;
            if (strcmp(ch, "safety-exit") == 0 || strcmp(ch, "timeout-safety-exit") == 0) tr->exit_ms = now;
            if (strcmp(ch, "timeout-safety-exit") == 0) tr->timed_out = true;
            if (strcmp(ch, "done") == 0) tr->done = true;
        }

        /* Every drive frame carries the promised values (the entry frame included). */
        if (strcmp(cur, "drive") == 0) {
            uint64_t rel = now - tr->drive_ms;
            float want_x = 0.0f;
            if (rel >= 4000 && rel < 6000) want_x = -0.7f;
            if (rel >= 14500 && rel < 17000) want_x = 0.7f;
            if (rel >= 17000 && rel < 19500) want_x = -0.7f;
            tr->drive_frames++;
            if (in.right_trigger != 1.0f || fabsf(in.left_stick_x - want_x) > 1e-6f)
                tr->drive_bad++;
        }
        /* The screenshot gesture: both clicks for 300 ms at 3 s, 11.6 s (paused, on the VR OPTIONS page) and 18 s into the drive. */
        if (in.left_stick_click || in.right_stick_click) {
            uint64_t rel = now - tr->drive_ms;
            bool in_window = strcmp(cur, "drive") == 0
                          && ((rel >= 3000 && rel < 3300) || (rel >= 11600 && rel < 11900)
                              || (rel >= 18000 && rel < 18300));
            if (in_window && in.left_stick_click && in.right_stick_click)
                tr->shot_frames++;
            else
                tr->stray_shot++;
        }
        /* The trigger may be held while in drive, and on the frame that leaves it. */
        if (in.right_trigger != 0.0f && strcmp(cur, "drive") != 0 && strcmp(prev, "drive") != 0)
            tr->stray_trigger++;
        /* Start alone, 60 ms at 6 s (pause) and 13.2 s (resume) into the drive. */
        bool pause_tap = strcmp(cur, "drive") == 0 && in.menu && !in.view
                      && ((now - tr->drive_ms >= 6000 && now - tr->drive_ms < 6060)
                          || (now - tr->drive_ms >= 13200 && now - tr->drive_ms < 13260));
        if (pause_tap)
            tr->pause_frames++;
        else if ((in.view || in.menu) && !is_exit_step(cur))
            tr->stray_hold++;

        if (in.view && in.menu) {
            if (tr->hold_run_ms == 0)
                tr->hold_start_ms = now;
            tr->hold_run_ms += SIM_STEP_MS;
            if (tr->hold_run_ms > tr->hold_best_ms) {
                tr->hold_best_ms = tr->hold_run_ms;
                tr->hold_best_start_ms = tr->hold_start_ms;
            }
        } else {
            tr->hold_run_ms = 0;
        }

        track(&up, in.dpad_up, now, 'U', tr);
        track(&down, in.dpad_down, now, 'D', tr);
        bool fell_a = track(&a, in.button_a, now, 'A', tr);
        bool fell_b = track(&b, in.button_b, now, 'B', tr);
        game_react(&g, fell_a, fell_b);
    }
}

static void check_names(const Trace *tr, const char *const *want, int want_n)
{
    CHECK(tr->n_names == want_n);
    for (int i = 0; i < want_n && i < tr->n_names; ++i) {
        CHECK(strcmp(tr->names[i], want[i]) == 0);
        if (strcmp(tr->names[i], want[i]) != 0)
            printf("  step %d: got %s, want %s\n", i, tr->names[i], want[i]);
    }
}

static void test_full_sequence(void)
{
    static const char *const want[] = {
        "wait-main-menu", "nav-up-x8", "accept-car-select", "wait-car-select", "back",
        "wait-main-menu-again", "nav-down-x8", "accept-start-race", "wait-racing", "countdown",
        "drive", "release", "safety-exit", "done"
    };
    Trace tr;
    run_sim(false, &tr);

    check_names(&tr, want, (int)(sizeof(want) / sizeof(want[0])));
    CHECK(tr.done);
    CHECK(!tr.timed_out);
    /* 8 dpad_up rising edges, then 8 dpad_down; A and B at their steps; then, in the drive step, the
     * pause menu walk: down x3 (to VR OPTIONS), A (open), A (vignette), down (HUD LAYOUT), A, A
     * (layout twice), B (back). */
    CHECK(strcmp(tr.rises, "UUUUUUUUABDDDDDDDDADDDAADAAB") == 0);
    CHECK(tr.n_rises == 28);
    CHECK(tr.press_count == 28);
    if (tr.n_rises == 28) {
        /* The menu walk: 200 ms presses (the paused scene runs at ~12 fps on the test host), the others 60 ms. */
        static const uint64_t want_rel[9] = { 6900, 7400, 7900, 8600, 9200, 9800, 10400, 11000, 12400 };
        for (int i = 0; i < 19; ++i)
            CHECK(tr.press_ms[i] == 60);
        for (int i = 0; i < 9; ++i) {
            CHECK(tr.rise_at[19 + i] - tr.drive_ms == want_rel[i]);
            CHECK(tr.press_ms[19 + i] == 200);
        }
    }
    /* Drive: throttle held, steering in the promised windows (left, then right and left once resumed). */
    CHECK(tr.drive_frames == 2050);
    CHECK(tr.drive_bad == 0);
    CHECK(tr.stray_trigger == 0);
    CHECK(tr.shot_frames == 90); /* three 300 ms presses at 10 ms frames */
    CHECK(tr.pause_frames == 12); /* two 60 ms Start taps */
    CHECK(tr.stray_shot == 0);
    CHECK(tr.drive_ms - tr.countdown_ms == 12000);
    CHECK(tr.release_ms - tr.drive_ms == 20500);
    CHECK(tr.exit_ms - tr.release_ms == 1000);
    /* Safety exit: view+menu held continuously for at least 2250 ms, and nothing else. */
    CHECK(tr.hold_best_ms >= 2250);
    CHECK(tr.hold_best_start_ms == tr.exit_ms);
    CHECK(tr.stray_hold == 0);
}

static void test_stuck_on_main(void)
{
    static const char *const want[] = {
        "wait-main-menu", "nav-up-x8", "accept-car-select", "wait-car-select",
        "timeout-safety-exit", "done"
    };
    Trace tr;
    run_sim(true, &tr);

    check_names(&tr, want, (int)(sizeof(want) / sizeof(want[0])));
    CHECK(tr.done);
    CHECK(tr.timed_out);
    if (tr.n_names >= 5) {
        uint64_t waited = tr.name_ms[4] - tr.name_ms[3];
        CHECK(waited > 20000 && waited <= 20010);
    }
    CHECK(strcmp(tr.rises, "UUUUUUUUA") == 0);
    for (int i = 0; i < tr.press_count && i < MAX_RISES; ++i)
        CHECK(tr.press_ms[i] == 60);
    CHECK(tr.drive_frames == 0);
    CHECK(tr.hold_best_ms >= 2250);
    CHECK(tr.hold_best_start_ms == tr.exit_ms);
    CHECK(tr.stray_hold == 0);
}

int main(void)
{
    test_full_sequence();
    test_stuck_on_main();

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("vr_fake_input_test: ALL PASS\n");
    return 0;
}
