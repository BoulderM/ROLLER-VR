/* PROJECTS/ROLLER/vr_vignette.c -- comfort vignette maths; see vr_vignette.h. Pure C. */
#include "vr_vignette.h"

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

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
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
    if (!text || !out)
        return false;
    while (*text == ' ' || *text == '\t')
        ++text;
    size_t len = 0;
    while (text[len] && text[len] != ' ' && text[len] != '\t' && text[len] != '\r' && text[len] != '\n')
        ++len;
    size_t end = len;
    while (text[end] == ' ' || text[end] == '\t' || text[end] == '\r' || text[end] == '\n')
        ++end;
    if (text[end] != '\0' || len == 0)
        return false;
    for (int l = 0; l < VR_VIGNETTE_LEVEL_COUNT; ++l) {
        const char *name = vr_vignette_level_name((VrVignetteLevel)l);
        size_t i = 0;
        while (i < len && name[i] && lower(text[i]) == name[i])
            ++i;
        if (i == len && name[i] == '\0') {
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

/* --- Setting file ---------------------------------------------------------------------- */

#define SETTINGS_KEY "vignette"

/* One line, [start, end) without its newline. Sets *key_end to the '=' (or NULL when the line
 * is blank, a comment or has no '=') and returns the start of the value. */
static const char *split_line(const char *start, const char *end, const char **key_start,
                              const char **key_end)
{
    const char *p = start;
    *key_start = *key_end = NULL;
    while (p < end && (*p == ' ' || *p == '\t'))
        ++p;
    if (p >= end || *p == ';' || *p == '#')
        return NULL;
    const char *eq = p;
    while (eq < end && *eq != '=')
        ++eq;
    if (eq >= end)
        return NULL;
    const char *k_end = eq;
    while (k_end > p && (k_end[-1] == ' ' || k_end[-1] == '\t'))
        --k_end;
    *key_start = p;
    *key_end = k_end;
    return eq + 1;
}

static bool is_vignette_key(const char *key_start, const char *key_end)
{
    const size_t want = sizeof(SETTINGS_KEY) - 1;
    if (!key_start || (size_t)(key_end - key_start) != want)
        return false;
    for (size_t i = 0; i < want; ++i)
        if (lower(key_start[i]) != SETTINGS_KEY[i])
            return false;
    return true;
}

bool vr_vignette_settings_parse(const char *text, VrVignetteLevel *out)
{
    if (!text || !out)
        return false;
    bool found = false;
    VrVignetteLevel level = VR_VIGNETTE_OFF;
    const char *line = text;
    while (*line) {
        const char *end = line;
        while (*end && *end != '\n')
            ++end;
        const char *ks, *ke;
        const char *value = split_line(line, end, &ks, &ke);
        if (value && is_vignette_key(ks, ke)) {
            char buf[32];
            size_t n = (size_t)(end - value);
            if (n < sizeof(buf)) {
                for (size_t i = 0; i < n; ++i)
                    buf[i] = value[i];
                buf[n] = '\0';
                VrVignetteLevel parsed;
                if (vr_vignette_level_parse(buf, &parsed)) {
                    level = parsed;
                    found = true;
                }
            }
        }
        line = *end ? end + 1 : end;
    }
    if (found)
        *out = level;
    return found;
}

static bool put(char *out, size_t cap, size_t *len, const char *s, size_t n)
{
    if (*len + n + 1 > cap)
        return false;
    for (size_t i = 0; i < n; ++i)
        out[*len + i] = s[i];
    *len += n;
    out[*len] = '\0';
    return true;
}

size_t vr_vignette_settings_format(const char *old_text, VrVignetteLevel level, char *out, size_t cap)
{
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    size_t len = 0;
    const char *name = vr_vignette_level_name(level);
    size_t name_len = 0;
    while (name[name_len])
        ++name_len;
    bool wrote = false, ok = true;

    if (!old_text || !*old_text) {
        static const char header[] = "; roller-vr settings. vignette: off, mild or strong.\n";
        ok = put(out, cap, &len, header, sizeof(header) - 1);
        old_text = "";
    }
    const char *line = old_text;
    while (ok && *line) {
        const char *end = line;
        while (*end && *end != '\n')
            ++end;
        const char *ks, *ke;
        split_line(line, end, &ks, &ke);
        if (is_vignette_key(ks, ke)) {
            if (!wrote) {
                ok = put(out, cap, &len, SETTINGS_KEY "=", sizeof(SETTINGS_KEY)) &&
                     put(out, cap, &len, name, name_len) && put(out, cap, &len, "\n", 1);
                wrote = true;
            } /* further vignette= lines are dropped */
        } else {
            ok = put(out, cap, &len, line, (size_t)(end - line)) && put(out, cap, &len, "\n", 1);
        }
        line = *end ? end + 1 : end;
    }
    if (ok && !wrote)
        ok = put(out, cap, &len, SETTINGS_KEY "=", sizeof(SETTINGS_KEY)) &&
             put(out, cap, &len, name, name_len) && put(out, cap, &len, "\n", 1);
    if (!ok) {
        out[0] = '\0';
        return 0;
    }
    return len;
}
