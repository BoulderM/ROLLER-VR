/* PROJECTS/ROLLER/vr_hud_layout.c -- HUD layout geometry; see vr_hud_layout.h. Pure C. */
#include "vr_hud_layout.h"

#include <stddef.h>

#include "vr_text.h"

static const char *const k_names[VR_HUD_LAYOUT_COUNT] = { "original", "spread", "wide" };
static const char *const k_labels[VR_HUD_LAYOUT_COUNT] = { "ORIGINAL", "SPREAD", "WIDE" };

static bool in_range(VrHudLayout l)
{
    return (int)l >= 0 && (int)l < VR_HUD_LAYOUT_COUNT;
}

const char *vr_hud_layout_name(VrHudLayout layout)
{
    return k_names[in_range(layout) ? layout : VR_HUD_ORIGINAL];
}

const char *vr_hud_layout_label(VrHudLayout layout)
{
    return k_labels[in_range(layout) ? layout : VR_HUD_ORIGINAL];
}

bool vr_hud_layout_parse(const char *text, VrHudLayout *out)
{
    if (!out)
        return false;
    for (int l = 0; l < VR_HUD_LAYOUT_COUNT; ++l) {
        if (vr_text_matches(text, k_names[l])) {
            *out = (VrHudLayout)l;
            return true;
        }
    }
    return false;
}

VrHudLayout vr_hud_layout_next(VrHudLayout layout)
{
    return in_range(layout) ? (VrHudLayout)(((int)layout + 1) % VR_HUD_LAYOUT_COUNT) : VR_HUD_ORIGINAL;
}

VrHudLayout vr_hud_layout_prev(VrHudLayout layout)
{
    return in_range(layout) ? (VrHudLayout)(((int)layout + VR_HUD_LAYOUT_COUNT - 1) % VR_HUD_LAYOUT_COUNT)
                            : VR_HUD_ORIGINAL;
}

/* The reference sheet is 640x400. The cut lines run through the gaps the real HUD leaves (empty
 * columns 160-189 and 448-479, empty rows 226-255 on a racing frame): x at 175 and 464; on the
 * left y at 240 (position and cars above, minimap below), on the right y at 190 (lap above,
 * speedometer below); in the middle y at 96 (timer above, the rest below). offset_*: where the
 * cluster's content sits relative to the centre of the sheet, measured on racing frames. */
#define REF_W 640
#define REF_H 400

typedef struct {
    int x0, y0, x1, y1;       /* cut rectangle in the reference sheet */
    int off_x, off_y;         /* content centre minus sheet centre, reference pixels (+x right, +y down) */
    bool moves;               /* false for the middle panel (race messages stay put) */
} Cut;

static const Cut k_cuts[VR_HUD_MAX_PANELS] = {
    {   0,   0, 175, 240, -240,  -79, true  },  /* position and car icons */
    { 175,   0, 464,  96,   -8, -155, true  },  /* timer */
    { 464,   0, 640, 190,  245, -134, true  },  /* lap */
    {   0, 240, 175, 400, -240,  140, true  },  /* minimap */
    { 464, 190, 640, 400,  239,  126, true  },  /* speedometer and gear */
    { 175,  96, 464, 400,    0,    0, false },  /* everything else, in the middle */
};

int vr_hud_panels(VrHudLayout layout, int tex_w, int tex_h, float quad_width_m, VrHudPanel out[VR_HUD_MAX_PANELS])
{
    if (!out || tex_w < 64 || tex_h < 40 || !(quad_width_m > 0.0f))
        return 0;
    const float quad_height_m = quad_width_m * (float)tex_h / (float)tex_w;
    if (layout != VR_HUD_SPREAD && layout != VR_HUD_WIDE) {
        out[0].x = 0; out[0].y = 0; out[0].w = tex_w; out[0].h = tex_h;
        out[0].right_m = 0.0f; out[0].up_m = 0.0f;
        out[0].width_m = quad_width_m; out[0].height_m = quad_height_m;
        return 1;
    }
    const float factor = layout == VR_HUD_WIDE ? VR_HUD_WIDE_FACTOR : VR_HUD_SPREAD_FACTOR;
    const float mx = quad_width_m / (float)tex_w;     /* metres per texture pixel */
    const float my = quad_height_m / (float)tex_h;
    for (int i = 0; i < VR_HUD_MAX_PANELS; ++i) {
        const Cut *c = &k_cuts[i];
        /* Shared cut lines are computed the same way on both sides, so the panels tile exactly. */
        int x0 = c->x0 * tex_w / REF_W, x1 = c->x1 * tex_w / REF_W;
        int y0 = c->y0 * tex_h / REF_H, y1 = c->y1 * tex_h / REF_H;
        out[i].x = x0; out[i].y = y0; out[i].w = x1 - x0; out[i].h = y1 - y0;
        out[i].width_m = (float)out[i].w * mx;
        out[i].height_m = (float)out[i].h * my;
        float cx = (float)x0 + (float)out[i].w * 0.5f - (float)tex_w * 0.5f;   /* from the sheet centre */
        float cy = (float)y0 + (float)out[i].h * 0.5f - (float)tex_h * 0.5f;
        float move = c->moves ? factor - 1.0f : 0.0f;
        /* The content offset is in reference pixels; scale it to this texture, then to metres. */
        float dx = (float)c->off_x * (float)tex_w / (float)REF_W * move;
        float dy = (float)c->off_y * (float)tex_h / (float)REF_H * move;
        out[i].right_m = (cx + dx) * mx;
        out[i].up_m = -(cy + dy) * my;
    }
    return VR_HUD_MAX_PANELS;
}
