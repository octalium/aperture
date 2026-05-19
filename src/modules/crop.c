#include "module.h"

#include "crop_comp_spv.h"

#include "cimgui.h"

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

static void slider_with_reset(const ap_module *self, float *params,
                              const char *label, int slot)
{
    igSliderFloat(label, &params[slot], 0.0f, 1.0f, "%.3f", 0);
    if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
        params[slot] = self->params_default[slot];
    }
}

static void crop_render(const ap_module *self, float *params)
{
    if (!params) return;
    slider_with_reset(self, params, "X0", SLOT_X0);
    slider_with_reset(self, params, "Y0", SLOT_Y0);
    slider_with_reset(self, params, "X1", SLOT_X1);
    slider_with_reset(self, params, "Y1", SLOT_Y1);

    // Guardrail: keep the rect non-degenerate so the shader's small
    // epsilon clamp doesn't have to carry the whole load.
    if (params[SLOT_X1] <= params[SLOT_X0]) params[SLOT_X1] = params[SLOT_X0] + 0.01f;
    if (params[SLOT_Y1] <= params[SLOT_Y0]) params[SLOT_Y1] = params[SLOT_Y0] + 0.01f;
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
