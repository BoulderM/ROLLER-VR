/* PROJECTS/ROLLER/vr_vignette.h
 *
 * Comfort vignette for the VR build, as pure C (no SDL, no OpenXR) so the maths can be
 * unit-tested (tests/vr_vignette_test.c). The vignette is a head-locked quad layer whose
 * texture is black with a clear centre; vr_runtime.c scales that quad to open or close the
 * clear centre, driven by how fast the cockpit view is turning:
 *
 *   level (off / mild / strong)  -> how sensitive it is and how far it closes
 *   cockpit turn rate (deg/s)    -> target strength 0..1, smoothed (fast in, slow out)
 *   strength 0..1                -> half-angle of the fully clear centre -> quad scale
 *
 * The turn rate is the rotation of the cockpit (the car's view before the head pose is
 * added), so turning your own head never darkens anything: only motion you did not make does,
 * which is what the vignette is for. While the pause menu is up the level's peak is shown
 * (preview) so the setting can be seen as it is changed. */
#ifndef VR_VIGNETTE_H
#define VR_VIGNETTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VR_VIGNETTE_OFF = 0,
    VR_VIGNETTE_MILD,
    VR_VIGNETTE_STRONG,
    VR_VIGNETTE_LEVEL_COUNT
} VrVignetteLevel;

/* "off", "mild", "strong". */
const char *vr_vignette_level_name(VrVignetteLevel level);
/* Case-insensitive; surrounding spaces and tabs are ignored. false (and *out untouched) if
 * the text is not one of the three names. */
bool vr_vignette_level_parse(const char *text, VrVignetteLevel *out);
/* off -> mild -> strong -> off. */
VrVignetteLevel vr_vignette_level_next(VrVignetteLevel level);

/* How dark the level gets at most (0..1): off 0, mild 0.75, strong 1. */
float vr_vignette_level_peak(VrVignetteLevel level);

/* Angle in degrees (0..180) of the rotation that takes a to b; both are rotation matrices in
 * [row][col] order. */
float vr_vignette_rotation_deg(const float a[3][3], const float b[3][3]);

typedef struct {
    float strength; /* smoothed, 0..1 */
} VrVignetteState;

/* One frame. rate_deg_s: how fast the cockpit view is turning. preview: hold the level's peak
 * (pause menu). dt_s: frame time in seconds (<= 0 changes nothing). Returns the smoothed
 * strength, always 0 for OFF and never above the level's peak. */
float vr_vignette_step(VrVignetteState *s, VrVignetteLevel level, float rate_deg_s, float dt_s, bool preview);

/* Geometry. A strength of 0 puts the clear centre so wide it is outside any headset's view
 * (the layer is not submitted below a small strength); 1 closes it to
 * VR_VIGNETTE_STRONG_CLEAR_DEG. */
#define VR_VIGNETTE_OPEN_CLEAR_DEG   75.0f
#define VR_VIGNETTE_STRONG_CLEAR_DEG 26.0f
#define VR_VIGNETTE_MIN_VISIBLE      0.02f /* below this strength the layer is skipped */
/* Half-angle (degrees) of the fully clear centre at this strength. */
float vr_vignette_clear_half_angle_deg(float strength);
/* Factor (>= 1) the base quad is scaled by to put the clear centre at that strength. */
float vr_vignette_quad_scale(float strength);

/* The ring texture: VR_VIGNETTE_TEX_SIZE squared RGBA8, black, with alpha 0 in the centre
 * rising smoothly to 255 and staying there. At quad scale 1 it covers +-VR_VIGNETTE_TEX_EXTENT
 * (a tangent, so about +-74 degrees) from the centre; the clear centre ends at
 * VR_VIGNETTE_STRONG_CLEAR_DEG and the fade is VR_VIGNETTE_FADE_WIDTH wide. */
#define VR_VIGNETTE_TEX_SIZE   512
#define VR_VIGNETTE_TEX_EXTENT 3.5f
#define VR_VIGNETTE_FADE_WIDTH 0.6f
void vr_vignette_fill_texture(uint8_t *rgba);

/* The setting file, roller-vr.ini next to the executable: plain `key=value` lines, of which only
 * `vignette=off|mild|strong` is read. Comments (; or #), blank lines and unknown keys are
 * ignored by the reader and kept by the writer. */
/* true if text holds a valid vignette= line (the last valid one wins); *out is untouched
 * otherwise. */
bool vr_vignette_settings_parse(const char *text, VrVignetteLevel *out);
/* Writes old_text (may be NULL) into out with its vignette= line set to `level`: replaced in
 * place, further vignette= lines dropped, appended if there was none. Returns the length
 * written (without the terminating NUL), or 0 if cap is too small. */
size_t vr_vignette_settings_format(const char *old_text, VrVignetteLevel level, char *out, size_t cap);

#endif /* VR_VIGNETTE_H */
