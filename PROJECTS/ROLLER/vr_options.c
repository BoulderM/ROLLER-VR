/* PROJECTS/ROLLER/vr_options.c -- VR options: settings file and pause menu rows; see vr_options.h. Pure C. */
#include "vr_options.h"

#include <stddef.h>

#include "vr_text.h"

int g_iVRPauseRow = VR_OPTIONS_ROW_VIGNETTE;

VrOptions vr_options_default(void)
{
    VrOptions o = { VR_VIGNETTE_OFF, VR_HUD_ORIGINAL };
    return o;
}

/* --- roller-vr.ini --------------------------------------------------------------------- */

#define KEY_VIGNETTE "vignette"
#define KEY_HUD      "hud"

typedef struct {
    const char *key;       /* lower case */
    size_t key_len;
} Key;

static const Key k_keys[] = {
    { KEY_VIGNETTE, sizeof(KEY_VIGNETTE) - 1 },
    { KEY_HUD, sizeof(KEY_HUD) - 1 },
};
#define KEY_COUNT ((int)(sizeof(k_keys) / sizeof(k_keys[0])))

/* One line, [start, end) without its newline. Returns the start of the value and sets the key's
 * span, or returns NULL (blank line, comment, no '='). */
static const char *split_line(const char *start, const char *end, const char **key_start, const char **key_end)
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

/* Which of k_keys the span is (case-insensitive, whole key), or -1. */
static int key_index(const char *ks, const char *ke)
{
    if (!ks)
        return -1;
    for (int k = 0; k < KEY_COUNT; ++k) {
        if ((size_t)(ke - ks) != k_keys[k].key_len)
            continue;
        size_t i = 0;
        while (i < k_keys[k].key_len && vr_text_lower(ks[i]) == k_keys[k].key[i])
            ++i;
        if (i == k_keys[k].key_len)
            return k;
    }
    return -1;
}

int vr_options_parse(const char *text, VrOptions *io)
{
    if (!text || !io)
        return 0;
    bool found[KEY_COUNT] = { false };
    VrOptions o = *io;
    const char *line = text;
    while (*line) {
        const char *end = line;
        while (*end && *end != '\n')
            ++end;
        const char *ks, *ke;
        const char *value = split_line(line, end, &ks, &ke);
        int k = value ? key_index(ks, ke) : -1;
        if (k >= 0) {
            char buf[32];
            size_t n = (size_t)(end - value);
            if (n < sizeof(buf)) {
                for (size_t i = 0; i < n; ++i)
                    buf[i] = value[i];
                buf[n] = '\0';
                if (k == 0) {
                    VrVignetteLevel v;
                    if (vr_vignette_level_parse(buf, &v)) {
                        o.vignette = v;
                        found[k] = true;
                    }
                } else {
                    VrHudLayout h;
                    if (vr_hud_layout_parse(buf, &h)) {
                        o.hud = h;
                        found[k] = true;
                    }
                }
            }
        }
        line = *end ? end + 1 : end;
    }
    *io = o;
    int count = 0;
    for (int k = 0; k < KEY_COUNT; ++k)
        count += found[k];
    return count;
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

static size_t cstr_len(const char *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

/* "key=value\n". */
static bool put_setting(char *out, size_t cap, size_t *len, int k, const char *value)
{
    return put(out, cap, len, k_keys[k].key, k_keys[k].key_len) && put(out, cap, len, "=", 1) &&
           put(out, cap, len, value, cstr_len(value)) && put(out, cap, len, "\n", 1);
}

size_t vr_options_format(const char *old_text, const VrOptions *opts, char *out, size_t cap)
{
    if (!out || cap == 0 || !opts)
        return 0;
    out[0] = '\0';
    size_t len = 0;
    const char *values[KEY_COUNT] = { vr_vignette_level_name(opts->vignette), vr_hud_layout_name(opts->hud) };
    bool wrote[KEY_COUNT] = { false };
    bool ok = true;

    if (!old_text || !*old_text) {
        static const char header[] =
            "; roller-vr settings. vignette: off, mild or strong. hud: original, spread or wide.\n";
        ok = put(out, cap, &len, header, sizeof(header) - 1);
        old_text = "";
    }
    const char *line = old_text;
    while (ok && *line) {
        const char *end = line;
        while (*end && *end != '\n')
            ++end;
        const char *ks, *ke;
        const char *value = split_line(line, end, &ks, &ke);
        int k = value ? key_index(ks, ke) : -1;
        if (k >= 0) {
            if (!wrote[k]) {
                ok = put_setting(out, cap, &len, k, values[k]);
                wrote[k] = true;
            } /* further lines of the same key are dropped */
        } else {
            ok = put(out, cap, &len, line, (size_t)(end - line)) && put(out, cap, &len, "\n", 1);
        }
        line = *end ? end + 1 : end;
    }
    for (int k = 0; ok && k < KEY_COUNT; ++k)
        if (!wrote[k])
            ok = put_setting(out, cap, &len, k, values[k]);
    if (!ok) {
        out[0] = '\0';
        return 0;
    }
    return len;
}

/* --- the pause menu page ----------------------------------------------------------------- */

int vr_options_row_down(int row)
{
    if (row == VR_OPTIONS_ROW_VIGNETTE)
        return VR_OPTIONS_ROW_HUD;
    if (row == VR_OPTIONS_ROW_HUD || row == VR_OPTIONS_ROW_EXIT)
        return VR_OPTIONS_ROW_EXIT;
    return VR_OPTIONS_ROW_VIGNETTE;
}

int vr_options_row_up(int row)
{
    if (row == VR_OPTIONS_ROW_EXIT)
        return VR_OPTIONS_ROW_HUD;
    return VR_OPTIONS_ROW_VIGNETTE; /* from the HUD row up one; from the first option (or a bad row) stay */
}

const char *vr_options_row_label(int row)
{
    if (row == VR_OPTIONS_ROW_VIGNETTE)
        return "VIGNETTE";
    if (row == VR_OPTIONS_ROW_HUD)
        return "HUD LAYOUT";
    return NULL;
}

static const char *vignette_label(VrVignetteLevel level)
{
    switch (level) {
        case VR_VIGNETTE_MILD:   return "MILD";
        case VR_VIGNETTE_STRONG: return "STRONG";
        default:                 return "OFF";
    }
}

const char *vr_options_row_value(const VrOptions *o, int row)
{
    if (!o)
        return NULL;
    if (row == VR_OPTIONS_ROW_VIGNETTE)
        return vignette_label(o->vignette);
    if (row == VR_OPTIONS_ROW_HUD)
        return vr_hud_layout_label(o->hud);
    return NULL;
}

bool vr_options_adjust(VrOptions *o, int row, int dir)
{
    if (!o || dir == 0)
        return false;
    if (row == VR_OPTIONS_ROW_VIGNETTE) {
        if (dir > 0) {
            o->vignette = vr_vignette_level_next(o->vignette);
        } else {
            /* previous = next, next: three levels */
            o->vignette = vr_vignette_level_next(vr_vignette_level_next(o->vignette));
        }
        return true;
    }
    if (row == VR_OPTIONS_ROW_HUD) {
        o->hud = dir > 0 ? vr_hud_layout_next(o->hud) : vr_hud_layout_prev(o->hud);
        return true;
    }
    return false;
}
