/* PROJECTS/ROLLER/vr_input_pad.h
 *
 * The VR controller pad: an SDL virtual gamepad that Frame/Touch controller input
 * (and --vr-fake-input) is written into, so the game sees it as one more standard
 * gamepad (menus, bindings, the rebind screen). Takes its own reference on SDL's
 * joystick and gamepad subsystems, which keeps SDL's joystick driver up across
 * InputSetWindowsBackend()'s SDL_QuitSubSystem() calls.
 *
 * Joystick backend: SDL reads SDL_HINT_JOYSTICK_DIRECTINPUT when its joystick driver
 * initialises (SDL_dinputjoystick.c) and again when its haptic driver initialises
 * (SDL_dinputhaptic.c). Both readers are Windows-only, so the hint is a no-op elsewhere.
 * vr_input_pad_attach() takes the backend and sets the hint immediately before its
 * joystick init, so it decides the backend only if no earlier code has initialised
 * SDL's joystick subsystem. While attached, a later InputSetWindowsBackend() does not
 * change the joystick driver until vr_input_pad_detach(). A call while attached returns
 * true and ignores use_directinput.
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
