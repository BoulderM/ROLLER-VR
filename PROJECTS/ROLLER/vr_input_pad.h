/* PROJECTS/ROLLER/vr_input_pad.h
 *
 * The VR controller pad: an SDL virtual gamepad that Frame/Touch controller input
 * (and --vr-fake-input) is written into, so the game sees it as one more standard
 * gamepad (menus, bindings, the rebind screen). Takes its own reference on SDL's
 * joystick and gamepad subsystems, so it exists under the WinMM backend (which
 * never initialises them) and survives InputSetWindowsBackend()'s
 * SDL_QuitSubSystem(). */
#ifndef VR_INPUT_PAD_H
#define VR_INPUT_PAD_H

#include <stdbool.h>
#include <SDL3/SDL_joystick.h>
#include "vr_input_map.h"

#define VR_PAD_NAME "ROLLER VR Controllers"

bool vr_input_pad_attach(void);          /* before InputInit(); false (logged) on failure */
bool vr_input_pad_attached(void);
SDL_JoystickID vr_input_pad_id(void);    /* 0 when not attached */
void vr_input_pad_write(const VrPadState *state); /* seen by the game after the next SDL_PumpEvents() */
void vr_input_pad_detach(void);

#endif /* VR_INPUT_PAD_H */
