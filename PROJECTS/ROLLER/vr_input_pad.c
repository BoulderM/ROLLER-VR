/* PROJECTS/ROLLER/vr_input_pad.c -- see vr_input_pad.h */
#include "vr_input_pad.h"

#include <SDL3/SDL.h>

_Static_assert((int)VR_PAD_BUTTON_SOUTH == (int)SDL_GAMEPAD_BUTTON_SOUTH &&
               (int)VR_PAD_BUTTON_EAST == (int)SDL_GAMEPAD_BUTTON_EAST &&
               (int)VR_PAD_BUTTON_WEST == (int)SDL_GAMEPAD_BUTTON_WEST &&
               (int)VR_PAD_BUTTON_NORTH == (int)SDL_GAMEPAD_BUTTON_NORTH &&
               (int)VR_PAD_BUTTON_BACK == (int)SDL_GAMEPAD_BUTTON_BACK &&
               (int)VR_PAD_BUTTON_GUIDE == (int)SDL_GAMEPAD_BUTTON_GUIDE &&
               (int)VR_PAD_BUTTON_START == (int)SDL_GAMEPAD_BUTTON_START &&
               (int)VR_PAD_BUTTON_LEFT_STICK == (int)SDL_GAMEPAD_BUTTON_LEFT_STICK &&
               (int)VR_PAD_BUTTON_RIGHT_STICK == (int)SDL_GAMEPAD_BUTTON_RIGHT_STICK &&
               (int)VR_PAD_BUTTON_LEFT_SHOULDER == (int)SDL_GAMEPAD_BUTTON_LEFT_SHOULDER &&
               (int)VR_PAD_BUTTON_RIGHT_SHOULDER == (int)SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER &&
               (int)VR_PAD_BUTTON_DPAD_UP == (int)SDL_GAMEPAD_BUTTON_DPAD_UP &&
               (int)VR_PAD_BUTTON_DPAD_DOWN == (int)SDL_GAMEPAD_BUTTON_DPAD_DOWN &&
               (int)VR_PAD_BUTTON_DPAD_LEFT == (int)SDL_GAMEPAD_BUTTON_DPAD_LEFT &&
               (int)VR_PAD_BUTTON_DPAD_RIGHT == (int)SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
               "VR pad button indices must equal SDL_GamepadButton");
_Static_assert((int)VR_PAD_AXIS_LEFTX == (int)SDL_GAMEPAD_AXIS_LEFTX &&
               (int)VR_PAD_AXIS_LEFTY == (int)SDL_GAMEPAD_AXIS_LEFTY &&
               (int)VR_PAD_AXIS_RIGHTX == (int)SDL_GAMEPAD_AXIS_RIGHTX &&
               (int)VR_PAD_AXIS_RIGHTY == (int)SDL_GAMEPAD_AXIS_RIGHTY &&
               (int)VR_PAD_AXIS_LEFT_TRIGGER == (int)SDL_GAMEPAD_AXIS_LEFT_TRIGGER &&
               (int)VR_PAD_AXIS_RIGHT_TRIGGER == (int)SDL_GAMEPAD_AXIS_RIGHT_TRIGGER &&
               (int)VR_PAD_AXIS_COUNT == (int)SDL_GAMEPAD_AXIS_COUNT,
               "VR pad axis indices must equal SDL_GamepadAxis");

static SDL_JoystickID s_pad_id = 0;
static SDL_Joystick *s_pad = NULL;
static bool s_subsystem_ref = false;

bool vr_input_pad_attach(void)
{
    if (s_pad)
        return true;
    if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        SDL_Log("VR input: SDL joystick/gamepad init failed: %s", SDL_GetError());
        return false;
    }
    s_subsystem_ref = true;

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = VR_PAD_AXIS_COUNT;
    desc.nbuttons = VR_PAD_BUTTON_COUNT;
    desc.axis_mask = VR_PAD_AXIS_MASK;
    desc.button_mask = VR_PAD_BUTTON_MASK;
    desc.name = VR_PAD_NAME;
    s_pad_id = SDL_AttachVirtualJoystick(&desc);
    if (!s_pad_id) {
        SDL_Log("VR input: SDL_AttachVirtualJoystick failed: %s", SDL_GetError());
        vr_input_pad_detach();
        return false;
    }
    s_pad = SDL_OpenJoystick(s_pad_id); /* the handle SDL_SetJoystickVirtual* need */
    if (!s_pad) {
        SDL_Log("VR input: opening the virtual pad failed: %s", SDL_GetError());
        vr_input_pad_detach();
        return false;
    }
    char *mapping = SDL_GetGamepadMappingForID(s_pad_id);
    SDL_Log("VR input: virtual pad \"%s\" attached (id %u, gamepad=%d) mapping: %s", VR_PAD_NAME,
            (unsigned)s_pad_id, (int)SDL_IsGamepad(s_pad_id), mapping ? mapping : "(none)");
    SDL_free(mapping);
    VrPadState rest;
    vr_input_pad_rest(&rest);
    vr_input_pad_write(&rest);
    return true;
}

bool vr_input_pad_attached(void) { return s_pad != NULL; }
SDL_JoystickID vr_input_pad_id(void) { return s_pad ? s_pad_id : 0; }

void vr_input_pad_write(const VrPadState *state)
{
    if (!s_pad || !state)
        return;
    for (int i = 0; i < VR_PAD_AXIS_COUNT; ++i)
        SDL_SetJoystickVirtualAxis(s_pad, i, state->axes[i]);
    for (int i = 0; i < VR_PAD_BUTTON_COUNT; ++i)
        SDL_SetJoystickVirtualButton(s_pad, i, state->buttons[i]);
}

void vr_input_pad_detach(void)
{
    if (s_pad) {
        SDL_CloseJoystick(s_pad);
        s_pad = NULL;
    }
    if (s_pad_id) {
        SDL_DetachVirtualJoystick(s_pad_id);
        s_pad_id = 0;
    }
    if (s_subsystem_ref) {
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD);
        s_subsystem_ref = false;
    }
}
