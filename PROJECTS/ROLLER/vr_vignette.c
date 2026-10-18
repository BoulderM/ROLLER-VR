/* PROJECTS/ROLLER/vr_vignette.c -- comfort vignette maths; see vr_vignette.h. Pure C. */
#include "vr_vignette.h"
#include "vr_text.h"

#include <math.h>
#include <stddef.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* How the cockpit turn rate (deg/s) maps to a target strength: nothing below LO, the level's
 * peak at HI, linear between. Strong starts earlier and reaches its peak at gentler turning. */
#define MILD_RATE_LO_DEG_S    30.0f
#define MILD_RATE_HI_DEG_S   120.0f
#define STRONG_RATE_LO_DEG_S  15.0f
#define STRONG_RATE_HI_DEG_S  70.0f
#define MILD_PEAK             0.75f
#define STRONG_PEAK           1.0f

/* Smoothing time constants: the vignette closes quickly when the view starts to turn and opens
 * slowly afterwards, so it does not flicker through a chicane. */
#define ATTACK_TAU_S 0.12f
#define RELEASE_TAU_S 0.90f

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

const char *vr_vignette_level_name(VrVignetteLevel level)
{
    switch (level) {
        case VR_VIGNETTE_MILD:   return "mild";
        case VR_VIGNETTE_STRONG: return "strong";
        default:                 return "off";
    }
}

bool vr_vignette_level_parse(const char *text, VrVignetteLevel *out)
{
    if (!out)
        return false;
    for (int l = 0; l < VR_VIGNETTE_LEVEL_COUNT; ++l) {
        if (vr_text_matches(text, vr_vignette_level_name((VrVignetteLevel)l))) {
            *out = (VrVignetteLevel)l;
            return true;
        }
    }
    return false;
}

VrVignetteLevel vr_vignette_level_next(VrVignetteLevel level)
{
    switch (level) {
        case VR_VIGNETTE_OFF:  return VR_VIGNETTE_MILD;
        case VR_VIGNETTE_MILD: return VR_VIGNETTE_STRONG;
        default:               return VR_VIGNETTE_OFF;
    }
}

float vr_vignette_level_peak(VrVignetteLevel level)
{
    switch (level) {
        case VR_VIGNETTE_MILD:   return MILD_PEAK;
        case VR_VIGNETTE_STRONG: return STRONG_PEAK;
        default:                 return 0.0f;
    }
}

float vr_vignette_rotation_deg(const float a[3][3], const float b[3][3])
{
    /* trace(a^T b) = 1 + 2 cos(angle). */
    float trace = 0.0f;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            trace += a[i][j] * b[i][j];
    float c = clampf((trace - 1.0f) * 0.5f, -1.0f, 1.0f);
    return acosf(c) * (180.0f / (float)M_PI);
}

float vr_vignette_step(VrVignetteState *s, VrVignetteLevel level, float rate_deg_s, float dt_s, bool preview)
{
    const float peak = vr_vignette_level_peak(level);
    if (peak <= 0.0f) {
        s->strength = 0.0f;
        return 0.0f;
    }
    if (dt_s <= 0.0f)
        return s->strength;

    float lo = level == VR_VIGNETTE_STRONG ? STRONG_RATE_LO_DEG_S : MILD_RATE_LO_DEG_S;
    float hi = level == VR_VIGNETTE_STRONG ? STRONG_RATE_HI_DEG_S : MILD_RATE_HI_DEG_S;
    float target = preview ? peak : peak * clampf((rate_deg_s - lo) / (hi - lo), 0.0f, 1.0f);

    float tau = target > s->strength ? ATTACK_TAU_S : RELEASE_TAU_S;
    s->strength += (target - s->strength) * (1.0f - expf(-dt_s / tau));
    s->strength = clampf(s->strength, 0.0f, peak); /* a milder level cannot keep a darker value */
    return s->strength;
}

float vr_vignette_clear_half_angle_deg(float strength)
{
    float t = clampf(strength, 0.0f, 1.0f);
    return VR_VIGNETTE_OPEN_CLEAR_DEG + (VR_VIGNETTE_STRONG_CLEAR_DEG - VR_VIGNETTE_OPEN_CLEAR_DEG) * t;
}

float vr_vignette_quad_scale(float strength)
{
    const float deg2rad = (float)M_PI / 180.0f;
    return tanf(vr_vignette_clear_half_angle_deg(strength) * deg2rad)
         / tanf(VR_VIGNETTE_STRONG_CLEAR_DEG * deg2rad);
}

void vr_vignette_fill_texture(uint8_t *rgba)
{
    const int n = VR_VIGNETTE_TEX_SIZE;
    const float half = (float)n * 0.5f;
    const float t0 = tanf(VR_VIGNETTE_STRONG_CLEAR_DEG * ((float)M_PI / 180.0f));
    const float t1 = t0 + VR_VIGNETTE_FADE_WIDTH;
    for (int y = 0; y < n; ++y) {
        float dy = ((float)y + 0.5f - half) / half * VR_VIGNETTE_TEX_EXTENT;
        for (int x = 0; x < n; ++x) {
            float dx = ((float)x + 0.5f - half) / half * VR_VIGNETTE_TEX_EXTENT;
            float r = sqrtf(dx * dx + dy * dy);
            float u = clampf((r - t0) / (t1 - t0), 0.0f, 1.0f);
            float a = u * u * (3.0f - 2.0f * u); /* smoothstep */
            uint8_t *p = rgba + ((size_t)y * (size_t)n + (size_t)x) * 4;
            p[0] = p[1] = p[2] = 0;
            p[3] = (uint8_t)(a * 255.0f + 0.5f);
        }
    }
}
