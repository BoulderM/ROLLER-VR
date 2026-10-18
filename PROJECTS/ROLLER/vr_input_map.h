/* PROJECTS/ROLLER/vr_input_map.h
 *
 * Frame/Touch/simple-controller input as pure C (no SDL, no OpenXR), so the
 * mapping can be unit-tested: the logical actions the OpenXR side reads, the
 * per-profile binding table (as path strings), and the per-frame step that turns
 * actions into the virtual pad's state, zeroes everything while the session is
 * not FOCUSED and runs the Back+Start safety gesture. vr_input.c owns the
 * OpenXR calls, vr_input_pad.c the SDL virtual joystick. */
#ifndef VR_INPUT_MAP_H
#define VR_INPUT_MAP_H

#include <stdbool.h>
#include <stdint.h>

/* Not in the Khronos registry or the OpenXR headers (spec, "Constraints"); strings
 * from Valve's SteamFrameControllerProfile.cs. */
#define VR_XR_VALVE_FRAME_EXTENSION_NAME "XR_VALVE_frame_controller_interaction"
#define VR_XR_VALVE_FRAME_PROFILE        "/interaction_profiles/valve/frame_controller_valve"
#define VR_XR_TOUCH_PROFILE              "/interaction_profiles/oculus/touch_controller"
#define VR_XR_SIMPLE_PROFILE             "/interaction_profiles/khr/simple_controller"

/* Pad element indices. Equal to SDL3's SDL_GamepadButton / SDL_GamepadAxis
 * (vr_input_pad.c static-asserts it). The pad is attached with the contiguous
 * masks below, so an index is also the virtual joystick's raw index (the fork's
 * virtual driver packs mapped elements in mask-bit order; tests/vr_virtual_pad_test.c). */
enum {
    VR_PAD_BUTTON_SOUTH = 0, VR_PAD_BUTTON_EAST, VR_PAD_BUTTON_WEST, VR_PAD_BUTTON_NORTH,
    VR_PAD_BUTTON_BACK, VR_PAD_BUTTON_GUIDE, VR_PAD_BUTTON_START,
    VR_PAD_BUTTON_LEFT_STICK, VR_PAD_BUTTON_RIGHT_STICK,
    VR_PAD_BUTTON_LEFT_SHOULDER, VR_PAD_BUTTON_RIGHT_SHOULDER,
    VR_PAD_BUTTON_DPAD_UP, VR_PAD_BUTTON_DPAD_DOWN, VR_PAD_BUTTON_DPAD_LEFT, VR_PAD_BUTTON_DPAD_RIGHT,
    VR_PAD_BUTTON_COUNT /* 15 */
};
enum {
    VR_PAD_AXIS_LEFTX = 0, VR_PAD_AXIS_LEFTY, VR_PAD_AXIS_RIGHTX, VR_PAD_AXIS_RIGHTY,
    VR_PAD_AXIS_LEFT_TRIGGER, VR_PAD_AXIS_RIGHT_TRIGGER,
    VR_PAD_AXIS_COUNT /* 6 */
};
#define VR_PAD_BUTTON_MASK       0x7FFFu
#define VR_PAD_AXIS_MASK         0x3Fu
#define VR_PAD_TRIGGER_RAW_REST  (-32768)

#define VR_SAFETY_HOLD_MS   2000u /* Back+Start (or Start alone without Back) held this long quits */
#define VR_COMBO_GRACE_MS    250u /* a lone Back/Start waits this long for its partner before reaching the game */
/* A tap released inside the grace window is delivered as a pulse of at least
 * VR_TAP_PULSE_MS. The pulse runs in whole frames (the release frame, then frames
 * until 50 ms of frame time has counted down), so its exact length is a multiple
 * of the frame time: it is 50 ms only when the frame time divides 50. */
#define VR_TAP_PULSE_MS       50u
#define VR_MAX_STEP_MS       100u /* dt clamp: one long hitch cannot complete the 2 s hold */
#define VR_COMFORT_GRACE_MS  200u /* a lone left thumbstick click waits this long for the right one (screenshot) */
#define VR_SQUEEZE_PRESS    0.6f  /* squeeze -> bumper hysteresis */
#define VR_SQUEEZE_RELEASE  0.4f

/* One frame of controller input in OpenXR conventions (sticks -1..1, +x right,
 * +y up; triggers/squeeze 0..1). Zero-initialised = nothing pressed. */
typedef struct {
    float left_stick_x, left_stick_y, right_stick_x, right_stick_y;
    float left_trigger, right_trigger;
    float left_squeeze, right_squeeze;
    bool button_a, button_b, button_x, button_y;
    bool menu;   /* -> Start */
    bool view;   /* -> Back  */
    bool left_shoulder, right_shoulder;
    bool left_stick_click, right_stick_click;
    bool dpad_up, dpad_down, dpad_left, dpad_right;
    bool select; /* khr/simple_controller select -> South */
} VrInputActions;

/* What goes into the virtual joystick: raw axis values and button states. */
typedef struct {
    int16_t axes[VR_PAD_AXIS_COUNT];
    bool buttons[VR_PAD_BUTTON_COUNT];
} VrPadState;

typedef enum {
    VR_INPUT_EVENT_NONE = 0,
    VR_INPUT_EVENT_RECENTER, /* clean Back+Start tap released before VR_SAFETY_HOLD_MS (consumed by M4) */
    VR_INPUT_EVENT_QUIT,     /* safety exit: request a clean quit */
    VR_INPUT_EVENT_SCREENSHOT, /* both thumbstick clicks down together: save the eye images */
    VR_INPUT_EVENT_COMFORT    /* left thumbstick click alone, held VR_COMFORT_GRACE_MS: cycle the vignette */
} VrInputEvent;

typedef struct {
    bool squeeze_left_down, squeeze_right_down;
    /* Per-button state for Back and Start outside a combo. */
    bool back_pending, start_pending;       /* pressed, still inside the grace window */
    bool back_forwarded, start_forwarded;   /* passing through to the pad */
    bool back_latched, start_latched;       /* suppressed until released (after a combo) */
    uint32_t back_held_ms, start_held_ms;
    uint32_t back_pulse_ms, start_pulse_ms;
    /* The Back+Start gesture. */
    bool combo, combo_clean, combo_fired;
    uint32_t combo_ms;
    /* Profiles without Back: Start held alone. */
    uint32_t start_only_ms;
    bool start_only_fired;
    /* Screenshot gesture: set when it fires, cleared once both clicks are released. */
    bool shot_disarmed;
    /* Comfort gesture: how long the left click has been down alone, and whether it has fired. */
    uint32_t comfort_ms;
    bool comfort_fired;
} VrInputMapState;

/* --- OpenXR action set and binding table ---------------------------------- */

typedef enum {
    VR_ACT_LEFT_STICK, VR_ACT_RIGHT_STICK,
    VR_ACT_LEFT_TRIGGER, VR_ACT_RIGHT_TRIGGER,
    VR_ACT_LEFT_SQUEEZE, VR_ACT_RIGHT_SQUEEZE,
    VR_ACT_LEFT_SHOULDER, VR_ACT_RIGHT_SHOULDER,
    VR_ACT_LEFT_STICK_CLICK, VR_ACT_RIGHT_STICK_CLICK,
    VR_ACT_A, VR_ACT_B, VR_ACT_X, VR_ACT_Y,
    VR_ACT_MENU, VR_ACT_VIEW,
    VR_ACT_DPAD_UP, VR_ACT_DPAD_DOWN, VR_ACT_DPAD_LEFT, VR_ACT_DPAD_RIGHT,
    VR_ACT_SELECT,
    VR_ACT_COUNT
} VrActionId;

typedef enum { VR_ACT_KIND_BOOL, VR_ACT_KIND_FLOAT, VR_ACT_KIND_VEC2 } VrActionKind;

typedef struct {
    const char *name;      /* XrActionCreateInfo.actionName: [a-z0-9_-.] */
    const char *localized;
    VrActionKind kind;
} VrActionDesc;

typedef struct {
    VrActionId action;
    const char *path;      /* full binding path, e.g. "/user/hand/right/input/a/click" */
} VrBinding;

typedef struct {
    const char *profile;             /* interaction profile path */
    const char *required_extension;  /* NULL = core; else suggest only when enabled */
    const VrBinding *bindings;
    int count;
    bool has_back;                   /* profile has a Back (view) button */
} VrProfileBindings;

#define VR_PROFILE_COUNT 3
extern const VrActionDesc vr_input_action_descs[VR_ACT_COUNT];
extern const VrProfileBindings vr_input_profiles[VR_PROFILE_COUNT]; /* Valve, Touch, simple */

/* Stores one action's current value: x/y for VEC2, x for FLOAT, x >= 0.5 for BOOL. */
void vr_input_actions_set(VrInputActions *actions, VrActionId id, float x, float y);

/* --- Per-frame mapping --------------------------------------------------- */

void vr_input_map_init(VrInputMapState *state);
void vr_input_pad_rest(VrPadState *out);           /* sticks 0, triggers at raw rest, no buttons */
int16_t vr_input_axis_from_unit(float v);          /* -1..1 -> -32767..32767 (NaN -> 0) */
int16_t vr_input_trigger_from_unit(float v);       /* 0..1 -> raw -32768..32767 (NaN -> rest) */

/* One frame. Not focused (or in == NULL): out = rest, gesture state reset, no event.
 * has_back: the active profile has Back (Valve). False (Touch/simple): Start passes
 * straight through and Start held VR_SAFETY_HOLD_MS alone quits. Returns at most
 * one event; QUIT fires once per hold. */
VrInputEvent vr_input_map_step(VrInputMapState *state, const VrInputActions *in, bool focused,
                               bool has_back, uint32_t dt_ms, VrPadState *out);

#endif /* VR_INPUT_MAP_H */
