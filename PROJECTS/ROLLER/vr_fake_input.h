/* PROJECTS/ROLLER/vr_fake_input.h -- scripted VR controller input (--vr-fake-input),
 * pure C. Driven by the current screen and the clock; the caller logs step changes. */
#ifndef VR_FAKE_INPUT_H
#define VR_FAKE_INPUT_H

#include <stdint.h>
#include "vr_input_map.h"

typedef enum { VR_FAKE_SCREEN_OTHER, VR_FAKE_SCREEN_MAIN_MENU, VR_FAKE_SCREEN_CAR_SELECT, VR_FAKE_SCREEN_RACING } VrFakeScreen;

typedef struct {
    int step;
    uint64_t step_start_ms;
    VrFakeScreen last_screen;
    uint64_t screen_since_ms;
    bool started;
} VrFakeInput;

void vr_fake_input_init(VrFakeInput *f);
/* Fills *out for this frame. Returns the new step's name when the step changed
 * on this call (log it), else NULL. A step that waits too long for its screen
 * jumps to the safety exit with the name "timeout-safety-exit". */
const char *vr_fake_input_step(VrFakeInput *f, VrFakeScreen screen, uint64_t now_ms, VrInputActions *out);

#endif
