/* PROJECTS/ROLLER/vr_hud_layout.h
 *
 * Where the race HUD goes in the headset, as pure C (no SDL, no OpenXR) so the geometry can
 * be unit-tested (tests/vr_hud_layout_test.c).
 *
 * The game draws its HUD into one transparent sheet (640x400 in SVGA, 320x200 otherwise):
 * position and car icons top left, the timer top centre, the lap top right, the minimap
 * bottom left, the speedometer bottom right. ORIGINAL shows that sheet as one head-locked quad,
 * as the game draws it. SPREAD and WIDE keep every element exactly as it is, and in the same
 * corner, but show the five clusters as separate panels cut from the same sheet and moved
 * further out from the centre, so the middle of the view stays clear. Whatever else the game
 * puts on the sheet (race messages, mostly in the middle) stays where it was, so nothing is lost.
 *
 * The cuts are fixed rectangles of the 640x400 sheet, chosen in the gaps the real HUD leaves
 * between its clusters (measured on racing frames); other sheet sizes scale them. */
#ifndef VR_HUD_LAYOUT_H
#define VR_HUD_LAYOUT_H

#include <stdbool.h>

typedef enum {
    VR_HUD_ORIGINAL = 0,
    VR_HUD_SPREAD,
    VR_HUD_WIDE,
    VR_HUD_LAYOUT_COUNT
} VrHudLayout;

/* "original", "spread", "wide" (the settings file) and "ORIGINAL", "SPREAD", "WIDE" (the
 * pause menu's font has capitals only). Out-of-range values read as ORIGINAL. */
const char *vr_hud_layout_name(VrHudLayout layout);
const char *vr_hud_layout_label(VrHudLayout layout);
/* Case-insensitive; surrounding spaces and tabs are ignored. false (and *out untouched) if the
 * text is not one of the three names. */
bool vr_hud_layout_parse(const char *text, VrHudLayout *out);
/* original -> spread -> wide -> original, and back. */
VrHudLayout vr_hud_layout_next(VrHudLayout layout);
VrHudLayout vr_hud_layout_prev(VrHudLayout layout);

/* How far SPREAD and WIDE move a cluster: its distance from the centre of the HUD is
 * multiplied by this (its size is not changed). */
#define VR_HUD_SPREAD_FACTOR 1.5f
#define VR_HUD_WIDE_FACTOR   2.0f

#define VR_HUD_MAX_PANELS 6

typedef struct {
    int x, y, w, h;                 /* the rectangle of the HUD texture this panel shows, in pixels */
    float right_m, up_m;            /* its centre, relative to the centre of the HUD quad (+right, +up), metres */
    float width_m, height_m;        /* its size, metres */
} VrHudPanel;

/* Fills out[] with the panels for this layout and returns how many (1 for ORIGINAL: the whole
 * texture at the centre; 6 otherwise). tex_w x tex_h is the HUD texture, quad_width_m the width
 * the whole sheet would have as one quad; metres per pixel is quad_width_m / tex_w. The panels'
 * rectangles tile the texture exactly, with no gaps or overlaps. Returns 0 for a texture
 * smaller than 64x40 or a non-positive width. */
int vr_hud_panels(VrHudLayout layout, int tex_w, int tex_h, float quad_width_m, VrHudPanel out[VR_HUD_MAX_PANELS]);

#endif /* VR_HUD_LAYOUT_H */
