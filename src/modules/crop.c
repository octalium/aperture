#include "module.h"

#include "crop_comp_spv.h"

#include "cimgui.h"

#include <math.h>

typedef struct {
    float x0;
    float y0;
    float x1;
    float y1;
} crop_push_t;

enum { SLOT_X0 = 0, SLOT_Y0 = 1, SLOT_X1 = 2, SLOT_Y1 = 3 };

static const float       crop_defaults[] = { 0.0f, 0.0f, 1.0f, 1.0f };
static const char *const crop_names[]    = { "x0", "y0", "x1", "y1" };

static int crop_pack_push(const ap_module *self,
                          const float *params,
                          const ap_raw_metadata *meta,
                          void *push_out)
{
    (void)self;
    (void)meta;
    crop_push_t *pc = push_out;
    pc->x0 = params ? params[SLOT_X0] : 0.0f;
    pc->y0 = params ? params[SLOT_Y0] : 0.0f;
    pc->x1 = params ? params[SLOT_X1] : 1.0f;
    pc->y1 = params ? params[SLOT_Y1] : 1.0f;
    return 0;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Crop is precise framing — numeric entry beats fuzzy sliders. The
// rect is stored as x0/y0/x1/y1 but presented as X / Y / Width /
// Height, which is the natural mental model. Values are fractions of
// the image ([0, 1]); a draggable canvas overlay is the eventual
// "something better" but wants canvas hit-testing first.
static void crop_render(const ap_module *self, float *params)
{
    (void)self;
    if (!params) return;

    float x = params[SLOT_X0];
    float y = params[SLOT_Y0];
    float w = params[SLOT_X1] - params[SLOT_X0];
    float h = params[SLOT_Y1] - params[SLOT_Y0];

    bool changed = false;
    changed |= igInputFloat("X",      &x, 0.01f, 0.1f, "%.3f", 0);
    changed |= igInputFloat("Y",      &y, 0.01f, 0.1f, "%.3f", 0);
    changed |= igInputFloat("Width",  &w, 0.01f, 0.1f, "%.3f", 0);
    changed |= igInputFloat("Height", &h, 0.01f, 0.1f, "%.3f", 0);

    if (changed) {
        x = clampf(x, 0.0f, 0.99f);
        y = clampf(y, 0.0f, 0.99f);
        w = clampf(w, 0.01f, 1.0f - x);
        h = clampf(h, 0.01f, 1.0f - y);
        params[SLOT_X0] = x;
        params[SLOT_Y0] = y;
        params[SLOT_X1] = x + w;
        params[SLOT_Y1] = y + h;
    }

    igTextDisabled("X/Y/W/H as fractions of the image; canvas drag-overlay is a follow-up");
}

// Geometric category. The pipeline graph's stage ordering keeps
// geometric modules together; running before tone / color means
// downstream stages see the cropped image as their working buffer
// content.
const ap_module module_crop = {
    .name           = "crop",
    .display_name   = "Crop",
    .category       = AP_MODULE_GEOMETRIC,
    .user_visible   = true,
    .spv_data       = crop_comp_spv,
    .spv_size       = crop_comp_spv_size,
    .push_size      = sizeof(crop_push_t),
    .pack_push      = crop_pack_push,
    .params_count   = 4,
    .params_default = crop_defaults,
    .params_names   = crop_names,
    .render_params  = crop_render,
};
