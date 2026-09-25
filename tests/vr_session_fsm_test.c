/* tests/vr_session_fsm_test.c -- unit tests for PROJECTS/ROLLER/vr_session_fsm.c (pure C). */
#include "vr_session_fsm.h"

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* Drives the fsm as vr_runtime.c does: carry out BEGIN/END actions immediately. */
static unsigned int step(VrSessionFsm *f, VrSessionState s)
{
    unsigned int a = vr_fsm_on_state_changed(f, s);
    if (a & VR_ACTION_BEGIN_SESSION)
        vr_fsm_session_begun(f);
    if (a & VR_ACTION_END_SESSION)
        vr_fsm_session_ended(f);
    return a;
}

static void test_normal_startup_sequence(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    CHECK(!vr_fsm_frames_allowed(&f));
    CHECK(step(&f, VR_STATE_IDLE) == VR_ACTION_NONE);
    CHECK(step(&f, VR_STATE_READY) == VR_ACTION_BEGIN_SESSION);
    CHECK(vr_fsm_frames_allowed(&f));
    CHECK(!vr_fsm_is_visible(&f) && !vr_fsm_is_focused(&f));

    CHECK(step(&f, VR_STATE_SYNCHRONIZED) == VR_ACTION_NONE);
    CHECK(!vr_fsm_is_visible(&f));
    CHECK(step(&f, VR_STATE_VISIBLE) == VR_ACTION_NONE);
    CHECK(vr_fsm_is_visible(&f) && !vr_fsm_is_focused(&f));
    CHECK(step(&f, VR_STATE_FOCUSED) == VR_ACTION_NONE);
    CHECK(vr_fsm_is_visible(&f) && vr_fsm_is_focused(&f));

    /* dashboard opens: still visible, no longer focused */
    CHECK(step(&f, VR_STATE_VISIBLE) == VR_ACTION_NONE);
    CHECK(vr_fsm_is_visible(&f) && !vr_fsm_is_focused(&f));
}

static void test_ready_only_begins_once(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    CHECK(step(&f, VR_STATE_READY) == VR_ACTION_BEGIN_SESSION);
    CHECK(step(&f, VR_STATE_READY) == VR_ACTION_NONE); /* already running */
}

static void test_runtime_initiated_stop(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    step(&f, VR_STATE_READY);
    step(&f, VR_STATE_FOCUSED);

    unsigned int a = step(&f, VR_STATE_STOPPING);
    CHECK(a == (VR_ACTION_END_SESSION | VR_ACTION_REQUEST_QUIT));
    CHECK(!vr_fsm_frames_allowed(&f));

    /* the runtime then reports EXITING: quit was already requested, nothing new */
    CHECK(step(&f, VR_STATE_EXITING) == VR_ACTION_NONE);
    CHECK(step(&f, VR_STATE_STOPPING) == VR_ACTION_NONE);
}

static void test_stopping_before_session_began(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    CHECK(step(&f, VR_STATE_STOPPING) == VR_ACTION_REQUEST_QUIT); /* nothing to end */
}

static void test_loss_pending_never_ends_session(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    step(&f, VR_STATE_READY);
    step(&f, VR_STATE_VISIBLE);
    CHECK(step(&f, VR_STATE_LOSS_PENDING) == VR_ACTION_REQUEST_QUIT);
    CHECK(vr_fsm_frames_allowed(&f)); /* xrEndSession is invalid here, so still "running" */
    CHECK(step(&f, VR_STATE_LOSS_PENDING) == VR_ACTION_NONE);
}

static void test_exiting_alone_requests_quit_once(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    CHECK(step(&f, VR_STATE_EXITING) == VR_ACTION_REQUEST_QUIT);
    CHECK(step(&f, VR_STATE_EXITING) == VR_ACTION_NONE);
}

static void test_game_initiated_exit_request(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    CHECK(!vr_fsm_should_request_exit(&f)); /* not running yet */

    step(&f, VR_STATE_READY);
    step(&f, VR_STATE_FOCUSED);
    CHECK(vr_fsm_should_request_exit(&f));
    CHECK(!vr_fsm_should_request_exit(&f)); /* only once */

    /* the runtime answers with STOPPING: end the session, but the game already
     * knows it is quitting, so a quit request is still reported once (it is idempotent) */
    unsigned int a = step(&f, VR_STATE_STOPPING);
    CHECK(a & VR_ACTION_END_SESSION);
}

static void test_no_exit_request_once_runtime_is_ending(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    step(&f, VR_STATE_READY);
    step(&f, VR_STATE_LOSS_PENDING);
    CHECK(!vr_fsm_should_request_exit(&f));

    VrSessionFsm g;
    vr_fsm_init(&g);
    step(&g, VR_STATE_READY);
    vr_fsm_on_state_changed(&g, VR_STATE_STOPPING); /* STOPPING seen, END not yet applied */
    CHECK(!vr_fsm_should_request_exit(&g));
}

static void test_runtime_is_ending(void)
{
    VrSessionFsm f;
    vr_fsm_init(&f);
    CHECK(!vr_fsm_runtime_is_ending(&f));
    step(&f, VR_STATE_READY);
    step(&f, VR_STATE_FOCUSED);
    CHECK(!vr_fsm_runtime_is_ending(&f));
    step(&f, VR_STATE_STOPPING);
    CHECK(vr_fsm_runtime_is_ending(&f));

    VrSessionFsm g;
    vr_fsm_init(&g);
    step(&g, VR_STATE_LOSS_PENDING);
    CHECK(vr_fsm_runtime_is_ending(&g));
    VrSessionFsm h;
    vr_fsm_init(&h);
    step(&h, VR_STATE_EXITING);
    CHECK(vr_fsm_runtime_is_ending(&h));
    CHECK(!vr_fsm_runtime_is_ending(NULL));
}

static void test_null_safety(void)
{
    vr_fsm_init(NULL);
    CHECK(vr_fsm_on_state_changed(NULL, VR_STATE_READY) == VR_ACTION_NONE);
    vr_fsm_session_begun(NULL);
    vr_fsm_session_ended(NULL);
    CHECK(!vr_fsm_should_request_exit(NULL));
    CHECK(!vr_fsm_frames_allowed(NULL));
    CHECK(!vr_fsm_is_visible(NULL));
    CHECK(!vr_fsm_is_focused(NULL));
}

int main(void)
{
    test_normal_startup_sequence();
    test_ready_only_begins_once();
    test_runtime_initiated_stop();
    test_stopping_before_session_began();
    test_loss_pending_never_ends_session();
    test_exiting_alone_requests_quit_once();
    test_game_initiated_exit_request();
    test_no_exit_request_once_runtime_is_ending();
    test_runtime_is_ending();
    test_null_safety();

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("vr_session_fsm_test: ALL PASS\n");
    return 0;
}
