/* PROJECTS/ROLLER/vr_session_fsm.c -- see vr_session_fsm.h */
#include "vr_session_fsm.h"

void vr_fsm_init(VrSessionFsm *fsm)
{
    if (!fsm)
        return;
    fsm->state = VR_STATE_UNKNOWN;
    fsm->session_running = false;
    fsm->quit_requested = false;
    fsm->exit_requested = false;
}

unsigned int vr_fsm_on_state_changed(VrSessionFsm *fsm, VrSessionState new_state)
{
    unsigned int actions = VR_ACTION_NONE;
    if (!fsm)
        return actions;

    fsm->state = new_state;
    switch (new_state) {
    case VR_STATE_READY:
        if (!fsm->session_running)
            actions |= VR_ACTION_BEGIN_SESSION;
        break;
    case VR_STATE_STOPPING:
        if (fsm->session_running)
            actions |= VR_ACTION_END_SESSION;
        if (!fsm->quit_requested) {
            fsm->quit_requested = true;
            actions |= VR_ACTION_REQUEST_QUIT;
        }
        break;
    case VR_STATE_EXITING:
    case VR_STATE_LOSS_PENDING:
        if (!fsm->quit_requested) {
            fsm->quit_requested = true;
            actions |= VR_ACTION_REQUEST_QUIT;
        }
        break;
    default:
        break;
    }
    return actions;
}

void vr_fsm_session_begun(VrSessionFsm *fsm)
{
    if (fsm)
        fsm->session_running = true;
}

void vr_fsm_session_ended(VrSessionFsm *fsm)
{
    if (fsm)
        fsm->session_running = false;
}

bool vr_fsm_should_request_exit(VrSessionFsm *fsm)
{
    if (!fsm || !fsm->session_running || fsm->exit_requested)
        return false;
    if (vr_fsm_runtime_is_ending(fsm))
        return false;
    fsm->exit_requested = true;
    return true;
}

bool vr_fsm_runtime_is_ending(const VrSessionFsm *fsm)
{
    return fsm && (fsm->state == VR_STATE_STOPPING || fsm->state == VR_STATE_EXITING ||
                   fsm->state == VR_STATE_LOSS_PENDING);
}

bool vr_fsm_frames_allowed(const VrSessionFsm *fsm)
{
    return fsm && fsm->session_running;
}

bool vr_fsm_is_visible(const VrSessionFsm *fsm)
{
    return fsm && fsm->session_running &&
           (fsm->state == VR_STATE_VISIBLE || fsm->state == VR_STATE_FOCUSED);
}

bool vr_fsm_is_focused(const VrSessionFsm *fsm)
{
    return fsm && fsm->session_running && fsm->state == VR_STATE_FOCUSED;
}
