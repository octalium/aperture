#include "module.h"

#include "edit/viewport.h"

#include "cimgui.h"

#include <math.h>

// Transform: the unified viewport tool — crop, rotation, flip, and
// per-axis scale in one module. Metadata only: it carries the
// viewport params and runs no shader. The pixel pipeline renders the
// full frame; the canvas + export apply the viewport (see
// src/edit/viewport.{h,c}).

enum {
    SLOT_X0       = 0,
    SLOT_Y0       = 1,
    SLOT_X1       = 2,
    SLOT_Y1       = 3,
    SLOT_ROTATION = 4,
    SLOT_FLIP_X   = 5,
    SLOT_FLIP_Y   = 6,
    SLOT_SCALE_X  = 7,
    SLOT_SCALE_Y  = 8,
    SLOT_COUNT    = 9,
};

static const float transform_defaults[SLOT_COUNT] = {
    /* x0 */ 0.0f, /* y0 */ 0.0f, /* x1 */ 1.0f, /* y1 */ 1.0f,
    /* rotation */ 0.0f,
    /* flip_x */ 0.0f, /* flip_y */ 0.0f,
    /* scale_x */ 1.0f, /* scale_y */ 1.0f,
};
static const char *const transform_names[SLOT_COUNT] = {
    "x0", "y0", "x1", "y1",
    "rotation", "flip_x", "flip_y", "scale_x", "scale_y",
};

// Decode an edit entry's params into a viewport. The slot layout
// above is the on-disk contract.
ap_viewport ap_transform_viewport(const float *params)
{
    if (!params) return ap_viewport_identity();
    ap_viewport vp;
    vp.crop_x0      = params[SLOT_X0];
    vp.crop_y0      = params[SLOT_Y0];
    vp.crop_x1      = params[SLOT_X1];
    vp.crop_y1      = params[SLOT_Y1];
    vp.rotation_deg = params[SLOT_ROTATION];
    vp.flip_x       = params[SLOT_FLIP_X] != 0.0f;
    vp.flip_y       = params[SLOT_FLIP_Y] != 0.0f;
    vp.scale_x      = params[SLOT_SCALE_X];
    vp.scale_y      = params[SLOT_SCALE_Y];
    return vp;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void transform_render(const ap_module *self, float *params,
                             const ap_module_render_ctx *ctx)
{
    if (!params) return;

    int iw = ctx->image_width;
    int ih = ctx->image_height;

    // ---- Crop, in pixels ------------------------------------------
    igSeparatorText("Crop");
    if (iw > 0 && ih > 0) {
        int x = (int)lroundf(params[SLOT_X0] * (float)iw);
        int y = (int)lroundf(params[SLOT_Y0] * (float)ih);
        int w = (int)lroundf((params[SLOT_X1] - params[SLOT_X0]) * (float)iw);
        int h = (int)lroundf((params[SLOT_Y1] - params[SLOT_Y0]) * (float)ih);

        bool changed = false;
        changed |= igInputInt("X (px)",      &x, 1, 16, 0);
        changed |= igInputInt("Y (px)",      &y, 1, 16, 0);
        changed |= igInputInt("Width (px)",  &w, 1, 16, 0);
        changed |= igInputInt("Height (px)", &h, 1, 16, 0);
        if (igButton("Reset crop", (ImVec2_c){ 0.0f, 0.0f })) {
            x = 0; y = 0; w = iw; h = ih;
            changed = true;
        }
        if (changed) {
            x = clampi(x, 0, iw - 1);
            y = clampi(y, 0, ih - 1);
            w = clampi(w, 1, iw - x);
            h = clampi(h, 1, ih - y);
            params[SLOT_X0] = (float)x       / (float)iw;
            params[SLOT_Y0] = (float)y       / (float)ih;
            params[SLOT_X1] = (float)(x + w) / (float)iw;
            params[SLOT_Y1] = (float)(y + h) / (float)ih;
        }
        igTextDisabled("crop output: %d x %d px", w, h);
    } else {
        igTextDisabled("(image dimensions unavailable)");
    }

    // ---- Rotation -------------------------------------------------
    igSeparatorText("Rotation");
    ap_module_slider_reset(self, params, "Angle (deg)", SLOT_ROTATION,
                           -180.0f, 180.0f, "%.1f");

    // ---- Flip -----------------------------------------------------
    igSeparatorText("Flip");
    bool fx = params[SLOT_FLIP_X] != 0.0f;
    bool fy = params[SLOT_FLIP_Y] != 0.0f;
    if (igCheckbox("Flip X", &fx)) params[SLOT_FLIP_X] = fx ? 1.0f : 0.0f;
    igSameLine(0.0f, -1.0f);
    if (igCheckbox("Flip Y", &fy)) params[SLOT_FLIP_Y] = fy ? 1.0f : 0.0f;

    // ---- Scale ----------------------------------------------------
    igSeparatorText("Scale");
    ap_module_slider_reset(self, params, "Scale X", SLOT_SCALE_X, 0.25f, 4.0f, "%.2f");
    ap_module_slider_reset(self, params, "Scale Y", SLOT_SCALE_Y, 0.25f, 4.0f, "%.2f");
}

// Metadata-only: spv_data == NULL so the pipeline graph skips it.
const ap_module module_transform = {
    .name           = "transform",
    .display_name   = "Transform",
    .category       = AP_MODULE_GEOMETRIC,
    .user_visible   = true,
    .spv_data       = NULL,
    .spv_size       = 0,
    .push_size      = 0,
    .pack_push      = NULL,
    .params_count   = SLOT_COUNT,
    .params_default = transform_defaults,
    .params_names   = transform_names,
    .render_params  = transform_render,
};
