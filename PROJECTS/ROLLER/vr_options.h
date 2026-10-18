/* PROJECTS/ROLLER/vr_options.h
 *
 * The VR build's own options, as pure C (no SDL, no OpenXR) so they can be unit-tested
 * (tests/vr_options_test.c): the comfort vignette and the HUD layout, the roller-vr.ini file
 * they are saved in, and the rows of the pause menu's VR OPTIONS page that change them.
 *
 * The pause menu itself is the game's own (func2.c draws it, 3d.c handles its keys); those VR-only
 * blocks call the row helpers here and vr_options_set(), so the page looks and moves like the
 * game's other pause pages. vr_options_current() and vr_options_set() are implemented in
 * vr_runtime.c (they need SDL to load and save the file). */
#ifndef VR_OPTIONS_H
#define VR_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>

#include "vr_hud_layout.h"
#include "vr_vignette.h"

typedef struct {
    VrVignetteLevel vignette;
    VrHudLayout hud;
} VrOptions;

/* Off, original. */
VrOptions vr_options_default(void);

/* --- roller-vr.ini, next to the executable ---------------------------------------------------
 * Plain `key=value` lines: `vignette=off|mild|strong` and `hud=original|spread|wide`. Comments
 * (; or #), blank lines and unknown keys are ignored by the reader and kept by the writer. */

/* Applies every valid vignette= and hud= line in text to *io (the last valid one of each wins)
 * and returns how many keys were found; fields without a valid line are left as they were. */
int vr_options_parse(const char *text, VrOptions *io);
/* Writes old_text (may be NULL) into out with its vignette= and hud= lines set from *opts:
 * replaced in place, further lines of the same key dropped, missing ones appended. Returns the
 * length written (without the terminating NUL), or 0 if cap is too small. */
size_t vr_options_format(const char *old_text, const VrOptions *opts, char *out, size_t cap);

/* --- the pause menu page ---------------------------------------------------------------------
 * Rows, top to bottom on screen: VIGNETTE, HUD LAYOUT, then the exit row (the game's own EXIT
 * text). Their numbers follow the SOUND page's habit of making the exit row 0. */
enum {
    VR_OPTIONS_ROW_EXIT = 0,
    VR_OPTIONS_ROW_VIGNETTE = 1,
    VR_OPTIONS_ROW_HUD = 2
};
/* Down moves towards the exit row and stops there; up moves back and stops at the first option,
 * except that up from the exit row goes to the last option. A bad row becomes the first option. */
int vr_options_row_down(int row);
int vr_options_row_up(int row);
/* "VIGNETTE", "HUD LAYOUT"; NULL for the exit row or a bad row. */
const char *vr_options_row_label(int row);
/* The capitals the pause menu's font has: "OFF", "MILD", "STRONG"; "ORIGINAL", "SPREAD", "WIDE";
 * NULL for the exit row, a bad row or a NULL options pointer. */
const char *vr_options_row_value(const VrOptions *o, int row);
/* Steps the row's value one way (dir > 0 next, dir < 0 previous, wrapping) and returns true if
 * anything changed (false for the exit row, a bad row or dir == 0). */
bool vr_options_adjust(VrOptions *o, int row, int dir);

/* --- state, implemented in vr_runtime.c --------------------------------------------------------- */
/* The options in force (loaded from roller-vr.ini when the session starts). */
VrOptions vr_options_current(void);
/* Makes these the options in force and saves them to roller-vr.ini. */
void vr_options_set(const VrOptions *o);

/* The pause menu page's selected row; set to VR_OPTIONS_ROW_VIGNETTE when the page opens. */
extern int g_iVRPauseRow;

#endif /* VR_OPTIONS_H */
