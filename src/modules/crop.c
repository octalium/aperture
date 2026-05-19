#include "module.h"

#include "cimgui.h"

#include <math.h>

// Crop is a *framing* module, not a pixel-processing stage. It carries
// the crop rectangle as params and runs no shader — the pipeline keeps
// rendering the full frame. The app reads the rect (ap_photo_crop_rect)
// and the canvas displays only that sub-region; export writes only
// that region. So: no spv_data, no pack_push — metadata only.
//
// The rect is stored normalized ([0,1] of the image) so it survives
// being applied across photos of different sizes via a pipeline.
// The UI presents it in pixels using the open photo's dimensions
// from the render context.

enum { SLOT_X0 = 0, SLOT_Y0 = 1, SLOT_X1 = 2, SLOT_Y1 = 3 };

static const float       crop_defaults[] = { 0.0f, 0.0f, 1.0f, 1.0f };
static const char *const crop_names[]    = { "x0", "y0", "x1", "y1" };

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void crop_render(const ap_module *self, float *params,
                        const ap_module_render_ctx *ctx)
{
    (void)self;
    if (!params) return;

    int iw = ctx->image_width;
    int ih = ctx->image_height;
    if (iw <= 0 || ih <= 0) {
        igTextDisabled("(image dimensions unavailable)");
        return;
    }

    // Normalized rect -> pixels for display.
    int x = (int)lroundf(params[SLOT_X0] * (float)iw);
    int y = (int)lroundf(params[SLOT_Y0] * (float)ih);
    int w = (int)lroundf((params[SLOT_X1] - params[SLOT_X0]) * (float)iw);
    int h = (int)lroundf((params[SLOT_Y1] - params[SLOT_Y0]) * (float)ih);

    bool changed = false;
    changed |= igInputInt("X (px)",      &x, 1, 16, 0);
    changed |= igInputInt("Y (px)",      &y, 1, 16, 0);
    changed |= igInputInt("Width (px)",  &w, 1, 16, 0);
    changed |= igInputInt("Height (px)", &h, 1, 16, 0);

    if (igButton("Reset to full image", (ImVec2_c){ 0.0f, 0.0f })) {
        x = 0; y = 0; w = iw; h = ih;
        changed = true;
    }

    if (changed) {
        x = clampi(x, 0, iw - 1);
        y = clampi(y, 0, ih - 1);
        w = clampi(w, 1, iw - x);
        h = clampi(h, 1, ih - y);
        params[SLOT_X0] = (float)x        / (float)iw;
        params[SLOT_Y0] = (float)y        / (float)ih;
        params[SLOT_X1] = (float)(x + w)  / (float)iw;
        params[SLOT_Y1] = (float)(y + h)  / (float)ih;
    }

    igTextDisabled("crop output: %d x %d px", w, h);
}

// Metadata-only module: user_visible so it shows in Tools and on the
// stack, but spv_data == NULL so the pipeline graph skips it (no
// compute stage). The crop rect is consumed by the canvas + export.
const ap_module module_crop = {
    .name           = "crop",
    .display_name   = "Crop",
    .category       = AP_MODULE_GEOMETRIC,
    .user_visible   = true,
    .spv_data       = NULL,
    .spv_size       = 0,
    .push_size      = 0,
    .pack_push      = NULL,
    .params_count   = 4,
    .params_default = crop_defaults,
    .params_names   = crop_names,
    .render_params  = crop_render,
};
