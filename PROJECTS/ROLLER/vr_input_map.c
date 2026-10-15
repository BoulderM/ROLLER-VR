/* PROJECTS/ROLLER/vr_input_map.c -- see vr_input_map.h */
#include "vr_input_map.h"

#include <math.h>
#include <string.h>

/* --- Action set and bindings --------------------------------------------- */

const VrActionDesc vr_input_action_descs[VR_ACT_COUNT] = {
    [VR_ACT_LEFT_STICK]        = { "left_stick",        "Left thumbstick",            VR_ACT_KIND_VEC2 },
    [VR_ACT_RIGHT_STICK]       = { "right_stick",       "Right thumbstick",           VR_ACT_KIND_VEC2 },
    [VR_ACT_LEFT_TRIGGER]      = { "left_trigger",      "Left trigger (brake)",       VR_ACT_KIND_FLOAT },
    [VR_ACT_RIGHT_TRIGGER]     = { "right_trigger",     "Right trigger (accelerate)", VR_ACT_KIND_FLOAT },
    [VR_ACT_LEFT_SQUEEZE]      = { "left_squeeze",      "Left grip",                  VR_ACT_KIND_FLOAT },
    [VR_ACT_RIGHT_SQUEEZE]     = { "right_squeeze",     "Right grip",                 VR_ACT_KIND_FLOAT },
    [VR_ACT_LEFT_SHOULDER]     = { "left_shoulder",     "Left shoulder (gear down)",  VR_ACT_KIND_BOOL },
    [VR_ACT_RIGHT_SHOULDER]    = { "right_shoulder",    "Right shoulder (gear up)",   VR_ACT_KIND_BOOL },
    [VR_ACT_LEFT_STICK_CLICK]  = { "left_stick_click",  "Left thumbstick click",      VR_ACT_KIND_BOOL },
    [VR_ACT_RIGHT_STICK_CLICK] = { "right_stick_click", "Right thumbstick click",     VR_ACT_KIND_BOOL },
    [VR_ACT_A]                 = { "button_a",          "A (accept)",                 VR_ACT_KIND_BOOL },
    [VR_ACT_B]                 = { "button_b",          "B (back)",                   VR_ACT_KIND_BOOL },
    [VR_ACT_X]                 = { "button_x",          "X",                          VR_ACT_KIND_BOOL },
    [VR_ACT_Y]                 = { "button_y",          "Y",                          VR_ACT_KIND_BOOL },
    [VR_ACT_MENU]              = { "menu",              "Menu (Start)",               VR_ACT_KIND_BOOL },
    [VR_ACT_VIEW]              = { "view",              "View (Back)",                VR_ACT_KIND_BOOL },
    [VR_ACT_DPAD_UP]           = { "dpad_up",           "D-pad up",                   VR_ACT_KIND_BOOL },
    [VR_ACT_DPAD_DOWN]         = { "dpad_down",         "D-pad down",                 VR_ACT_KIND_BOOL },
    [VR_ACT_DPAD_LEFT]         = { "dpad_left",         "D-pad left",                 VR_ACT_KIND_BOOL },
    [VR_ACT_DPAD_RIGHT]        = { "dpad_right",        "D-pad right",                VR_ACT_KIND_BOOL },
    [VR_ACT_SELECT]            = { "select",            "Select",                     VR_ACT_KIND_BOOL },
};

#define L_IN "/user/hand/left/input/"
#define R_IN "/user/hand/right/input/"

/* Valve Frame (XR_VALVE_frame_controller_interaction). Per-hand components from
 * SteamFrameControllerProfile.cs: dpad_* and view are left-hand only; a, b, x,
 * y and menu are right-hand only; system is left to the runtime. */
static const VrBinding k_valve_bindings[] = {
    { VR_ACT_LEFT_STICK,        L_IN "thumbstick" },
    { VR_ACT_RIGHT_STICK,       R_IN "thumbstick" },
    { VR_ACT_LEFT_TRIGGER,      L_IN "trigger/value" },
    { VR_ACT_RIGHT_TRIGGER,     R_IN "trigger/value" },
    { VR_ACT_LEFT_SQUEEZE,      L_IN "squeeze/value" },
    { VR_ACT_RIGHT_SQUEEZE,     R_IN "squeeze/value" },
    { VR_ACT_LEFT_SHOULDER,     L_IN "shoulder/click" },
    { VR_ACT_RIGHT_SHOULDER,    R_IN "shoulder/click" },
    { VR_ACT_LEFT_STICK_CLICK,  L_IN "thumbstick/click" },
    { VR_ACT_RIGHT_STICK_CLICK, R_IN "thumbstick/click" },
    { VR_ACT_A,                 R_IN "a/click" },
    { VR_ACT_B,                 R_IN "b/click" },
    { VR_ACT_X,                 R_IN "x/click" },
    { VR_ACT_Y,                 R_IN "y/click" },
    { VR_ACT_MENU,              R_IN "menu/click" },
    { VR_ACT_VIEW,              L_IN "view/click" },
    { VR_ACT_DPAD_UP,           L_IN "dpad_up/click" },
    { VR_ACT_DPAD_DOWN,         L_IN "dpad_down/click" },
    { VR_ACT_DPAD_LEFT,         L_IN "dpad_left/click" },
    { VR_ACT_DPAD_RIGHT,        L_IN "dpad_right/click" },
};

/* Oculus Touch (core). No d-pad (menus use the stick), no view: Back is
 * unavailable, so the safety exit falls back to Start held alone. Right system
 * is reserved by runtimes. */
static const VrBinding k_touch_bindings[] = {
    { VR_ACT_LEFT_STICK,        L_IN "thumbstick" },
    { VR_ACT_RIGHT_STICK,       R_IN "thumbstick" },
    { VR_ACT_LEFT_TRIGGER,      L_IN "trigger/value" },
    { VR_ACT_RIGHT_TRIGGER,     R_IN "trigger/value" },
    { VR_ACT_LEFT_SQUEEZE,      L_IN "squeeze/value" },
    { VR_ACT_RIGHT_SQUEEZE,     R_IN "squeeze/value" },
    { VR_ACT_LEFT_STICK_CLICK,  L_IN "thumbstick/click" },
    { VR_ACT_RIGHT_STICK_CLICK, R_IN "thumbstick/click" },
    { VR_ACT_A,                 R_IN "a/click" },
    { VR_ACT_B,                 R_IN "b/click" },
    { VR_ACT_X,                 L_IN "x/click" },
    { VR_ACT_Y,                 L_IN "y/click" },
    { VR_ACT_MENU,              L_IN "menu/click" },
};

/* KHR simple controller (core): menus only, nothing to drive with. */
static const VrBinding k_simple_bindings[] = {
    { VR_ACT_SELECT, L_IN "select/click" },
    { VR_ACT_SELECT, R_IN "select/click" },
    { VR_ACT_MENU,   L_IN "menu/click" },
    { VR_ACT_MENU,   R_IN "menu/click" },
};

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

const VrProfileBindings vr_input_profiles[VR_PROFILE_COUNT] = {
    { VR_XR_VALVE_FRAME_PROFILE, VR_XR_VALVE_FRAME_EXTENSION_NAME, k_valve_bindings, COUNT_OF(k_valve_bindings), true },
    { VR_XR_TOUCH_PROFILE,       NULL, k_touch_bindings,  COUNT_OF(k_touch_bindings),  false },
    { VR_XR_SIMPLE_PROFILE,      NULL, k_simple_bindings, COUNT_OF(k_simple_bindings), false },
};

void vr_input_actions_set(VrInputActions *a, VrActionId id, float x, float y)
{
    bool b = x >= 0.5f;
    if (!a)
        return;
    switch (id) {
    case VR_ACT_LEFT_STICK:        a->left_stick_x = x; a->left_stick_y = y; break;
    case VR_ACT_RIGHT_STICK:       a->right_stick_x = x; a->right_stick_y = y; break;
    case VR_ACT_LEFT_TRIGGER:      a->left_trigger = x; break;
    case VR_ACT_RIGHT_TRIGGER:     a->right_trigger = x; break;
    case VR_ACT_LEFT_SQUEEZE:      a->left_squeeze = x; break;
    case VR_ACT_RIGHT_SQUEEZE:     a->right_squeeze = x; break;
    case VR_ACT_LEFT_SHOULDER:     a->left_shoulder = b; break;
    case VR_ACT_RIGHT_SHOULDER:    a->right_shoulder = b; break;
    case VR_ACT_LEFT_STICK_CLICK:  a->left_stick_click = b; break;
    case VR_ACT_RIGHT_STICK_CLICK: a->right_stick_click = b; break;
    case VR_ACT_A:                 a->button_a = b; break;
    case VR_ACT_B:                 a->button_b = b; break;
    case VR_ACT_X:                 a->button_x = b; break;
    case VR_ACT_Y:                 a->button_y = b; break;
    case VR_ACT_MENU:              a->menu = b; break;
    case VR_ACT_VIEW:              a->view = b; break;
    case VR_ACT_DPAD_UP:           a->dpad_up = b; break;
    case VR_ACT_DPAD_DOWN:         a->dpad_down = b; break;
    case VR_ACT_DPAD_LEFT:         a->dpad_left = b; break;
    case VR_ACT_DPAD_RIGHT:        a->dpad_right = b; break;
    case VR_ACT_SELECT:            a->select = b; break;
    default: break;
    }
}

/* --- Conversions ---------------------------------------------------------- */

int16_t vr_input_axis_from_unit(float v)
{
    if (!(v == v))            /* NaN */
        v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)floorf(v * 32767.0f + 0.5f);
}

int16_t vr_input_trigger_from_unit(float v)
{
    if (!(v == v))
        v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (v < 0.0f) v = 0.0f;
    float raw = floorf(-32768.0f + v * 65535.0f + 0.5f);
    if (raw > 32767.0f) raw = 32767.0f;
    if (raw < -32768.0f) raw = -32768.0f;
    return (int16_t)raw;
}

void vr_input_pad_rest(VrPadState *out)
{
    memset(out, 0, sizeof(*out));
    out->axes[VR_PAD_AXIS_LEFT_TRIGGER] = VR_PAD_TRIGGER_RAW_REST;
    out->axes[VR_PAD_AXIS_RIGHT_TRIGGER] = VR_PAD_TRIGGER_RAW_REST;
}

void vr_input_map_init(VrInputMapState *s)
{
    if (s)
        memset(s, 0, sizeof(*s));
}

/* --- Back/Start outside a combo ------------------------------------------ */

/* A lone press waits up to VR_COMBO_GRACE_MS for its partner (so the safety
 * gesture never also opens the pause menu); held past that it passes through;
 * released inside it, it is still delivered as a VR_TAP_PULSE_MS pulse (at
 * least one step). After a combo the button stays suppressed until released. */
static bool vr_lone_button(bool down, bool *latched, bool *pending, bool *forwarded,
                           uint32_t *held_ms, uint32_t *pulse_ms, uint32_t dt)
{
    if (*latched) {
        if (!down)
            *latched = false;
        return false;
    }
    if (down) {
        if (*forwarded)
            return true;
        *pending = true;
        *held_ms += dt;
        if (*held_ms >= VR_COMBO_GRACE_MS) {
            *pending = false;
            *forwarded = true;
            *held_ms = 0;
            return true;
        }
        return false;
    }
    bool out = false;
    if (*pending) {
        *pending = false;
        *pulse_ms = VR_TAP_PULSE_MS;
        out = true;
    }
    *forwarded = false;
    *held_ms = 0;
    if (*pulse_ms > 0) {
        out = true;
        *pulse_ms = *pulse_ms > dt ? *pulse_ms - dt : 0;
    }
    return out;
}

static void vr_reset_lone(VrInputMapState *s)
{
    s->back_pending = s->start_pending = false;
    s->back_forwarded = s->start_forwarded = false;
    s->back_held_ms = s->start_held_ms = 0;
    s->back_pulse_ms = s->start_pulse_ms = 0;
}

static bool vr_squeeze(bool *down, float v)
{
    if (!*down && v >= VR_SQUEEZE_PRESS)
        *down = true;
    else if (*down && v < VR_SQUEEZE_RELEASE)
        *down = false;
    return *down;
}

/* --- Per-frame step -------------------------------------------------------- */

static VrInputEvent vr_input_map_step_pad(VrInputMapState *s, const VrInputActions *in, bool focused,
                                          bool has_back, uint32_t dt, VrPadState *out)
{
    VrInputEvent ev = VR_INPUT_EVENT_NONE;
    vr_input_pad_rest(out);
    if (dt > VR_MAX_STEP_MS)
        dt = VR_MAX_STEP_MS;
    if (!focused || !in) {
        vr_input_map_init(s); /* nothing carries over a focus loss */
        return ev;
    }

    out->axes[VR_PAD_AXIS_LEFTX] = vr_input_axis_from_unit(in->left_stick_x);
    out->axes[VR_PAD_AXIS_LEFTY] = vr_input_axis_from_unit(-in->left_stick_y);   /* OpenXR +y up, SDL +y down */
    out->axes[VR_PAD_AXIS_RIGHTX] = vr_input_axis_from_unit(in->right_stick_x);
    out->axes[VR_PAD_AXIS_RIGHTY] = vr_input_axis_from_unit(-in->right_stick_y);
    out->axes[VR_PAD_AXIS_LEFT_TRIGGER] = vr_input_trigger_from_unit(in->left_trigger);
    out->axes[VR_PAD_AXIS_RIGHT_TRIGGER] = vr_input_trigger_from_unit(in->right_trigger);

    bool squeeze_l = vr_squeeze(&s->squeeze_left_down, in->left_squeeze);
    bool squeeze_r = vr_squeeze(&s->squeeze_right_down, in->right_squeeze);
    out->buttons[VR_PAD_BUTTON_SOUTH] = in->button_a || in->select;
    out->buttons[VR_PAD_BUTTON_EAST] = in->button_b;
    out->buttons[VR_PAD_BUTTON_WEST] = in->button_x;
    out->buttons[VR_PAD_BUTTON_NORTH] = in->button_y;
    out->buttons[VR_PAD_BUTTON_LEFT_STICK] = in->left_stick_click;
    out->buttons[VR_PAD_BUTTON_RIGHT_STICK] = in->right_stick_click;
    out->buttons[VR_PAD_BUTTON_LEFT_SHOULDER] = in->left_shoulder || squeeze_l;
    out->buttons[VR_PAD_BUTTON_RIGHT_SHOULDER] = in->right_shoulder || squeeze_r;
    out->buttons[VR_PAD_BUTTON_DPAD_UP] = in->dpad_up;
    out->buttons[VR_PAD_BUTTON_DPAD_DOWN] = in->dpad_down;
    out->buttons[VR_PAD_BUTTON_DPAD_LEFT] = in->dpad_left;
    out->buttons[VR_PAD_BUTTON_DPAD_RIGHT] = in->dpad_right;
    /* GUIDE is never set: system belongs to the runtime. */

    bool back = in->view, start = in->menu;

    if (!has_back) {
        out->buttons[VR_PAD_BUTTON_BACK] = back;
        out->buttons[VR_PAD_BUTTON_START] = start;
        if (start) {
            s->start_only_ms += dt;
            if (!s->start_only_fired && s->start_only_ms >= VR_SAFETY_HOLD_MS) {
                s->start_only_fired = true;
                ev = VR_INPUT_EVENT_QUIT;
            }
        } else {
            s->start_only_ms = 0;
            s->start_only_fired = false;
        }
        return ev;
    }
    s->start_only_ms = 0;
    s->start_only_fired = false;

    if (s->combo) {
        if (back && start) {
            s->combo_ms += dt;
            if (!s->combo_fired && s->combo_ms >= VR_SAFETY_HOLD_MS) {
                s->combo_fired = true;
                ev = VR_INPUT_EVENT_QUIT;
            }
        } else {
            if (!s->combo_fired && s->combo_clean)
                ev = VR_INPUT_EVENT_RECENTER;
            s->combo = false;
            s->combo_ms = 0;
            s->combo_fired = false;
            s->back_latched = back;
            s->start_latched = start;
        }
        return ev; /* Back and Start stay released for the whole gesture */
    }

    if (back && start && !s->back_latched && !s->start_latched) {
        /* Clean = neither button had reached the game yet; only a clean tap recenters.
         * Any combo held long enough quits, whichever order the buttons came in. */
        s->combo = true;
        s->combo_clean = !s->back_forwarded && !s->start_forwarded;
        s->combo_fired = false;
        s->combo_ms = 0;
        vr_reset_lone(s);
        return ev;
    }

    out->buttons[VR_PAD_BUTTON_BACK] = vr_lone_button(back, &s->back_latched, &s->back_pending,
                                                      &s->back_forwarded, &s->back_held_ms,
                                                      &s->back_pulse_ms, dt);
    out->buttons[VR_PAD_BUTTON_START] = vr_lone_button(start, &s->start_latched, &s->start_pending,
                                                       &s->start_forwarded, &s->start_held_ms,
                                                       &s->start_pulse_ms, dt);
    return ev;
}

/* Both thumbstick clicks down fires VR_INPUT_EVENT_SCREENSHOT once, whichever came
 * first. The clicks themselves still reach the pad (the game binds neither). It
 * re-arms only when both are up, so a bouncing finger does not fire twice, and it
 * never masks the Back+Start events. */
VrInputEvent vr_input_map_step(VrInputMapState *s, const VrInputActions *in, bool focused,
                               bool has_back, uint32_t dt, VrPadState *out)
{
    VrInputEvent ev = vr_input_map_step_pad(s, in, focused, has_back, dt, out);
    if (!focused || !in)
        return ev; /* the step above already cleared the state */

    if (!in->left_stick_click && !in->right_stick_click) {
        s->shot_disarmed = false;
    } else if (in->left_stick_click && in->right_stick_click && !s->shot_disarmed) {
        s->shot_disarmed = true;
        if (ev == VR_INPUT_EVENT_NONE)
            ev = VR_INPUT_EVENT_SCREENSHOT;
    }
    return ev;
}
