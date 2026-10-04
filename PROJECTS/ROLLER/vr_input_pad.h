/* PROJECTS/ROLLER/vr_input_pad.h
 *
 * The VR controller pad: an SDL virtual gamepad that Frame/Touch controller input
 * (and --vr-fake-input) is written into, so the game sees it as one more standard
 * gamepad (menus, bindings, the rebind screen). Takes its own reference on SDL's
 * joystick and gamepad subsystems, which keeps SDL's joystick driver up across
 * InputSetWindowsBackend()'s SDL_QuitSubSystem() calls.
 *
 * Joystick backend: SDL reads SDL_HINT_JOYSTICK_DIRECTINPUT once, when its joystick
 * driver initialises (SDL_dinputjoystick.c; only the Windows driver reads it, so the
 * hint is a no-op elsewhere). vr_input_pad_attach() therefore takes the backend and
 * sets the hint before it initialises the joystick subsystem. This guarantees:
 *  - the backend is fixed by the attach call: the pad's reference keeps SDL's driver
 *    alive, so a later InputSetWindowsBackend() does not change which joystick driver
 *    SDL uses until vr_input_pad_detach();
 *  - if the joystick subsystem was already initialised by someone else, the hint only
 *    takes effect at that earlier initialisation;
 *  - a call while attached returns true and ignores use_directinput.
 *
 * Lifetime precondition: vr_input_pad_detach() must run before SDL_Quit(). The
 * statics behind this API hold SDL handles, and detach calls SDL_CloseJoystick and
 * SDL_QuitSubSystem on them; after SDL_Quit() they dangle. */
#ifndef VR_INPUT_PAD_H
#define VR_INPUT_PAD_H

#include <stdbool.h>
#include <SDL3/SDL_joystick.h>
#include "vr_input_map.h"

#define VR_PAD_NAME "ROLLER VR Controllers"

/* before InputInit(); use_directinput is the game's backend, i.e.
 * InputGetWindowsBackend() == INPUT_WINDOWS_BACKEND_SDL_DINPUT. false (logged) on failure. */
bool vr_input_pad_attach(bool use_directinput);
bool vr_input_pad_attached(void);
SDL_JoystickID vr_input_pad_id(void);    /* 0 when not attached */
void vr_input_pad_write(const VrPadState *state); /* seen by the game after the next SDL_PumpEvents() */
void vr_input_pad_detach(void);

#endif /* VR_INPUT_PAD_H */
