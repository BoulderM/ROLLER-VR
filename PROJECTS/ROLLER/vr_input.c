/* PROJECTS/ROLLER/vr_input.c -- see vr_input.h */
#include "vr_fake_input.h"
#include "vr_input.h"
#include "vr_input_map.h"
#include "vr_input_pad.h"
#include "vr_log.h"
#include "vr_runtime.h" /* vr_request_screenshot() */

#include <SDL3/SDL.h>
#include <string.h>

#include "3d.h"        /* racing */
#include "frontend.h"  /* eFrontendCurrentState, quit_game */
#include "roller.h"    /* g_bVRFakeInput */

static XrInstance s_xr_instance = XR_NULL_HANDLE;
static XrSession s_xr_session = XR_NULL_HANDLE;
static XrActionSet s_action_set = XR_NULL_HANDLE;
static XrAction s_actions[VR_ACT_COUNT];
static XrPath s_hand_paths[2];
static bool s_attached = false;
static bool s_has_back = false;      /* set from the current interaction profile */
static VrInputMapState s_map;
static Uint64 s_last_ms = 0;
static VrPadState s_logged_pad;
static bool s_have_logged_pad = false;
/* --vr-fake-input: the script and its clock. The clock advances only on frames
 * the session is focused, so a stretch without focus does not skip presses. */
static VrFakeInput s_fake;
static uint64_t s_fake_ms = 0;

#define VR_MAX_BINDINGS_PER_PROFILE 32

static XrPath vr_to_path(const char *s)
{
    XrPath p = XR_NULL_PATH;
    XrResult r = xrStringToPath(s_xr_instance, s, &p);
    if (XR_FAILED(r)) {
        vr_log_line("WARN", "VR input: xrStringToPath(%s) failed (XrResult=%d)", s, (int)r);
        return XR_NULL_PATH;
    }
    return p;
}

static XrActionType vr_xr_action_type(VrActionKind kind)
{
    switch (kind) {
    case VR_ACT_KIND_FLOAT: return XR_ACTION_TYPE_FLOAT_INPUT;
    case VR_ACT_KIND_VEC2:  return XR_ACTION_TYPE_VECTOR2F_INPUT;
    default:                return XR_ACTION_TYPE_BOOLEAN_INPUT;
    }
}

bool vr_input_xr_init(XrInstance instance, XrSession session, bool valve_frame_enabled)
{
    s_xr_instance = instance;
    s_xr_session = session;
    vr_input_map_init(&s_map);
    vr_fake_input_init(&s_fake);
    s_fake_ms = 0;

    XrActionSetCreateInfo set_info = { .type = XR_TYPE_ACTION_SET_CREATE_INFO, .priority = 0 };
    strncpy(set_info.actionSetName, "roller", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(set_info.localizedActionSetName, "ROLLER", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    XrResult r = xrCreateActionSet(instance, &set_info, &s_action_set);
    if (XR_FAILED(r)) {
        vr_log_line("WARN", "VR input: xrCreateActionSet failed (XrResult=%d)", (int)r);
        s_action_set = XR_NULL_HANDLE;
        return false;
    }
    for (int i = 0; i < VR_ACT_COUNT; ++i) {
        XrActionCreateInfo info = { .type = XR_TYPE_ACTION_CREATE_INFO };
        info.actionType = vr_xr_action_type(vr_input_action_descs[i].kind);
        strncpy(info.actionName, vr_input_action_descs[i].name, XR_MAX_ACTION_NAME_SIZE - 1);
        strncpy(info.localizedActionName, vr_input_action_descs[i].localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        r = xrCreateAction(s_action_set, &info, &s_actions[i]);
        if (XR_FAILED(r)) {
            vr_log_line("WARN", "VR input: xrCreateAction(%s) failed (XrResult=%d)", info.actionName, (int)r);
            return false; /* the action set is destroyed in vr_input_xr_shutdown() */
        }
    }

    /* One call per profile: a rejected profile (bad path, unsupported profile)
     * must not take the others down with it. */
    int accepted = 0;
    for (int p = 0; p < VR_PROFILE_COUNT; ++p) {
        const VrProfileBindings *prof = &vr_input_profiles[p];
        if (prof->required_extension && !valve_frame_enabled) {
            vr_log_line("INFO", "VR input: skipping %s (%s not enabled)", prof->profile, prof->required_extension);
            continue;
        }
        XrActionSuggestedBinding bindings[VR_MAX_BINDINGS_PER_PROFILE];
        uint32_t n = 0;
        for (int k = 0; k < prof->count && n < VR_MAX_BINDINGS_PER_PROFILE; ++k) {
            XrPath path = vr_to_path(prof->bindings[k].path);
            if (path == XR_NULL_PATH)
                continue;
            bindings[n].action = s_actions[prof->bindings[k].action];
            bindings[n].binding = path;
            n++;
        }
        XrPath profile_path = vr_to_path(prof->profile);
        if (profile_path == XR_NULL_PATH || n == 0) {
            vr_log_line("WARN", "VR input: no usable bindings for %s; skipped", prof->profile);
            continue;
        }
        XrInteractionProfileSuggestedBinding suggested = {
            .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
            .interactionProfile = profile_path,
            .countSuggestedBindings = n,
            .suggestedBindings = bindings,
        };
        r = xrSuggestInteractionProfileBindings(instance, &suggested);
        vr_log_line(XR_FAILED(r) ? "WARN" : "INFO", "VR input: suggested %u bindings for %s -> XrResult=%d",
                    (unsigned)n, prof->profile, (int)r);
        if (XR_SUCCEEDED(r))
            accepted++;
    }
    if (accepted == 0)
        return false;

    XrSessionActionSetsAttachInfo attach = {
        .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
        .countActionSets = 1,
        .actionSets = &s_action_set,
    };
    r = xrAttachSessionActionSets(session, &attach);
    if (XR_FAILED(r)) {
        vr_log_line("WARN", "VR input: xrAttachSessionActionSets failed (XrResult=%d)", (int)r);
        return false;
    }
    s_hand_paths[0] = vr_to_path("/user/hand/left");
    s_hand_paths[1] = vr_to_path("/user/hand/right");
    s_attached = true;
    vr_log_line("INFO", "VR input: controller actions attached (%d of %d profiles accepted)", accepted, VR_PROFILE_COUNT);
    return true;
}

/* has_back of the binding-table entry for this profile path; false if unknown. */
static bool vr_profile_has_back(const char *profile)
{
    for (int p = 0; p < VR_PROFILE_COUNT; ++p)
        if (strcmp(profile, vr_input_profiles[p].profile) == 0)
            return vr_input_profiles[p].has_back;
    return false;
}

void vr_input_xr_on_profile_changed(void)
{
    if (!s_attached)
        return;
    bool has_back = false;
    for (int h = 0; h < 2; ++h) {
        XrInteractionProfileState state = { .type = XR_TYPE_INTERACTION_PROFILE_STATE };
        char name[XR_MAX_PATH_LENGTH] = "(none)";
        XrResult r = xrGetCurrentInteractionProfile(s_xr_session, s_hand_paths[h], &state);
        if (XR_SUCCEEDED(r) && state.interactionProfile != XR_NULL_PATH) {
            uint32_t len = 0;
            if (XR_FAILED(xrPathToString(s_xr_instance, state.interactionProfile, sizeof(name), &len, name)))
                strcpy(name, "(unreadable)");
            else if (vr_profile_has_back(name))
                has_back = true;
        }
        vr_log_line("INFO", "VR input: %s hand interaction profile: %s (XrResult=%d)", h ? "right" : "left", name, (int)r);
    }
    s_has_back = has_back;
    vr_log_line("INFO", "VR input: safety exit is %s held 2 s", s_has_back ? "Back+Start (View+Menu)" : "Start (Menu)");
}

/* Returns false when nothing was read this frame (not focused, or the sync failed). */
static bool vr_input_xr_read(VrInputActions *in)
{
    XrActiveActionSet active = { .actionSet = s_action_set, .subactionPath = XR_NULL_PATH };
    XrActionsSyncInfo sync = { .type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &active };
    XrResult r = xrSyncActions(s_xr_session, &sync);
    if (XR_FAILED(r)) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            vr_log_line("WARN", "VR input: xrSyncActions failed (XrResult=%d); controllers read as released", (int)r);
        }
        return false;
    }
    if (r == XR_SESSION_NOT_FOCUSED)
        return false;
    for (int i = 0; i < VR_ACT_COUNT; ++i) {
        XrActionStateGetInfo get = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = s_actions[i] };
        switch (vr_input_action_descs[i].kind) {
        case VR_ACT_KIND_BOOL: {
            XrActionStateBoolean st = { .type = XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_SUCCEEDED(xrGetActionStateBoolean(s_xr_session, &get, &st)) && st.isActive)
                vr_input_actions_set(in, (VrActionId)i, st.currentState ? 1.0f : 0.0f, 0.0f);
            break;
        }
        case VR_ACT_KIND_FLOAT: {
            XrActionStateFloat st = { .type = XR_TYPE_ACTION_STATE_FLOAT };
            if (XR_SUCCEEDED(xrGetActionStateFloat(s_xr_session, &get, &st)) && st.isActive)
                vr_input_actions_set(in, (VrActionId)i, st.currentState, 0.0f);
            break;
        }
        case VR_ACT_KIND_VEC2: {
            XrActionStateVector2f st = { .type = XR_TYPE_ACTION_STATE_VECTOR2F };
            if (XR_SUCCEEDED(xrGetActionStateVector2f(s_xr_session, &get, &st)) && st.isActive)
                vr_input_actions_set(in, (VrActionId)i, st.currentState.x, st.currentState.y);
            break;
        }
        }
    }
    return true;
}

/* Logs button changes, and the axes once a second while any is off rest, so a
 * roller-vr.log from the user's PC shows controller input reaching the pad. */
static void vr_input_log_pad(const VrPadState *pad)
{
    static Uint64 s_last_axis_log = 0;
    unsigned bits = 0, old_bits = 0;
    for (int i = 0; i < VR_PAD_BUTTON_COUNT; ++i) {
        bits |= pad->buttons[i] ? 1u << i : 0u;
        old_bits |= (s_have_logged_pad && s_logged_pad.buttons[i]) ? 1u << i : 0u;
    }
    if (!s_have_logged_pad || bits != old_bits)
        vr_log_line("INFO", "VR input: pad buttons 0x%04x", bits);
    VrPadState rest;
    vr_input_pad_rest(&rest);
    if (memcmp(pad->axes, rest.axes, sizeof(rest.axes)) != 0 && SDL_GetTicks() - s_last_axis_log >= 1000) {
        s_last_axis_log = SDL_GetTicks();
        vr_log_line("INFO", "VR input: pad axes lx=%d ly=%d rx=%d ry=%d lt=%d rt=%d", pad->axes[0], pad->axes[1],
                    pad->axes[2], pad->axes[3], pad->axes[4], pad->axes[5]);
    }
    s_logged_pad = *pad;
    s_have_logged_pad = true;
}

static VrFakeScreen vr_fake_screen(void)
{
    switch (eFrontendCurrentState) {
    case eFRONTEND_STATE_MAIN_MENU:  return VR_FAKE_SCREEN_MAIN_MENU;
    case eFRONTEND_STATE_CAR_SELECT: return VR_FAKE_SCREEN_CAR_SELECT;
    case eFRONTEND_STATE_RACING:     return VR_FAKE_SCREEN_RACING;
    default:                         return VR_FAKE_SCREEN_OTHER;
    }
}

void vr_input_frame(bool session_focused)
{
    if (!vr_input_pad_attached())
        return;
    Uint64 now = SDL_GetTicks();
    Uint64 elapsed = s_last_ms ? now - s_last_ms : 0;
    s_last_ms = now;
    /* At least 1 ms so a lone-button tap pulse still counts down on the first frame
     * and on sub-millisecond frames; at most VR_MAX_STEP_MS so one hitch cannot
     * complete the 2 s safety hold. */
    Uint32 dt = elapsed < 1 ? 1u : elapsed > VR_MAX_STEP_MS ? VR_MAX_STEP_MS : (Uint32)elapsed;

    VrInputActions in;
    SDL_zero(in);
    bool has_back = s_has_back;
    bool read_ok;
    if (g_bVRFakeInput) {
        /* Scripted input stands in for the controllers. The script runs only while the
         * session is focused, so the quit-forwarded path (vr_input_frame(false)) and any
         * unfocused frame send the pad to rest and cannot keep pressing. It does not use
         * the OpenXR action set, so it also works when vr_input_xr_init() failed. */
        read_ok = session_focused;
        if (read_ok) {
            s_fake_ms += dt;
            const char *step = vr_fake_input_step(&s_fake, vr_fake_screen(), s_fake_ms, &in);
            if (step)
                vr_log_line("INFO", "VR fake input: step %s (frontend state %d)", step, (int)eFrontendCurrentState);
            has_back = true; /* exercises the Back+Start gesture; Monado has no controllers */
        }
    } else {
        /* A failed or not-focused read counts as not focused: the pad goes to rest and
         * no stale or half-read state can complete a gesture. */
        read_ok = s_attached && session_focused && vr_input_xr_read(&in);
    }

    VrPadState pad;
    VrInputEvent ev = vr_input_map_step(&s_map, &in, read_ok, has_back, dt, &pad);
    vr_input_pad_write(&pad);
    vr_input_log_pad(&pad);

    if (ev == VR_INPUT_EVENT_QUIT && !quit_game) {
        vr_log_line("INFO", "VR input: safety exit (%s held %u ms), requesting quit",
                    has_back ? "Back+Start" : "Start", (unsigned)VR_SAFETY_HOLD_MS);
        quit_game = 1;   /* the same flags --vr-frames uses (vr_run_frame()) */
        racing = 0;
    } else if (ev == VR_INPUT_EVENT_SCREENSHOT) {
        vr_log_line("INFO", "VR input: screenshot gesture (both thumbstick clicks), frontend state %d",
                    (int)eFrontendCurrentState);
        vr_request_screenshot();
    } else if (ev == VR_INPUT_EVENT_RECENTER) {
        vr_log_line("INFO", "VR input: Back+Start tap: recenter requested (frontend state %d; "
                            "nothing to recenter until the menu quad exists)", (int)eFrontendCurrentState);
    }
}

void vr_input_xr_shutdown(void)
{
    if (s_action_set != XR_NULL_HANDLE)
        xrDestroyActionSet(s_action_set); /* destroys its actions too */
    s_action_set = XR_NULL_HANDLE;
    memset(s_actions, 0, sizeof(s_actions));
    s_attached = false;
    s_xr_session = XR_NULL_HANDLE;
    s_xr_instance = XR_NULL_HANDLE;
}
