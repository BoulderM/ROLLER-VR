/* PROJECTS/ROLLER/vr_session_fsm.h
 *
 * OpenXR session lifecycle as a pure state machine (no OpenXR types), so the
 * decisions can be unit-tested. vr_runtime.c feeds it XrSessionState changes and
 * carries out the returned actions (xrBeginSession, xrEndSession, telling the
 * game to quit). VrSessionState uses the same numeric values as XrSessionState;
 * vr_runtime.c asserts that at compile time. */
#ifndef VR_SESSION_FSM_H
#define VR_SESSION_FSM_H

#include <stdbool.h>

typedef enum {
    VR_STATE_UNKNOWN      = 0,
    VR_STATE_IDLE         = 1,
    VR_STATE_READY        = 2,
    VR_STATE_SYNCHRONIZED = 3,
    VR_STATE_VISIBLE      = 4,
    VR_STATE_FOCUSED      = 5,
    VR_STATE_STOPPING     = 6,
    VR_STATE_LOSS_PENDING = 7,
    VR_STATE_EXITING      = 8
} VrSessionState;

/* Bit flags returned by vr_fsm_on_state_changed(); the caller performs each set one. */
enum {
    VR_ACTION_NONE          = 0,
    VR_ACTION_BEGIN_SESSION = 1 << 0, /* call xrBeginSession, then vr_fsm_session_begun() */
    VR_ACTION_END_SESSION   = 1 << 1, /* call xrEndSession, then vr_fsm_session_ended() */
    VR_ACTION_REQUEST_QUIT  = 1 << 2  /* the runtime wants us gone: make the game quit cleanly */
};

typedef struct {
    VrSessionState state;
    bool session_running; /* between a successful xrBeginSession and xrEndSession */
    bool quit_requested;  /* the runtime asked us to quit; REQUEST_QUIT already returned once */
    bool exit_requested;  /* the game asked to quit; xrRequestExitSession already due once */
} VrSessionFsm;

void vr_fsm_init(VrSessionFsm *fsm);

/* Records a new session state and returns the VR_ACTION_* flags to carry out.
 * READY starts the session (once). STOPPING ends a running session and asks the
 * game to quit. EXITING and LOSS_PENDING ask the game to quit but never end the
 * session (xrEndSession is only valid from STOPPING). Each quit is requested
 * once, however many such events arrive. */
unsigned int vr_fsm_on_state_changed(VrSessionFsm *fsm, VrSessionState new_state);

void vr_fsm_session_begun(VrSessionFsm *fsm);
void vr_fsm_session_ended(VrSessionFsm *fsm);

/* Call when the game wants to quit. Returns true exactly once, and only while the
 * session is running and the runtime has not already started ending it, meaning
 * "call xrRequestExitSession now". */
bool vr_fsm_should_request_exit(VrSessionFsm *fsm);

/* True once the runtime has started ending the session (STOPPING, EXITING or
 * LOSS_PENDING): there is nothing left to request or wait for. */
bool vr_fsm_runtime_is_ending(const VrSessionFsm *fsm);

/* Frames may be waited on/begun/ended only while the session is running. */
bool vr_fsm_frames_allowed(const VrSessionFsm *fsm);
/* The app's frames are visible to the user (VISIBLE or FOCUSED). */
bool vr_fsm_is_visible(const VrSessionFsm *fsm);
/* The app has input focus (FOCUSED); input must read zero otherwise. */
bool vr_fsm_is_focused(const VrSessionFsm *fsm);

#endif /* VR_SESSION_FSM_H */
