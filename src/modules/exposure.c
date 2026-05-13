#include "module.h"

#include "exposure_comp_spv.h"

#include "cimgui.h"

typedef struct {
    float exposure_ev;
} exposure_push_t;

enum { SLOT_EV = 0 };

static const float       exposure_defaults[] = { 0.0f };
static const char *const exposure_names[]    = { "ev" };

static int exposure_pack_push(const ap_module *self,
                              const float *params,
                              const ap_raw_metadata *meta,
                              void *push_out)
{
    (void)self;
    (void)meta;
    exposure_push_t *pc = push_out;
    pc->exposure_ev = params ? params[SLOT_EV] : 0.0f;
    return 0;
}

static void exposure_render(const ap_module *self, float *params)
{
    (void)self;
    if (!params) return;
    igSliderFloat("EV", &params[SLOT_EV], -5.0f, 5.0f, "%.2f", 0);
}

const ap_module module_exposure = {
    .name           = "exposure",
    .display_name   = "Exposure",
    .category       = AP_MODULE_TONE,
    .user_visible   = true,
    .spv_data       = exposure_comp_spv,
    .spv_size       = exposure_comp_spv_size,
    .push_size      = sizeof(exposure_push_t),
    .pack_push      = exposure_pack_push,
    .params_count   = 1,
    .params_default = exposure_defaults,
    .params_names   = exposure_names,
    .render_params  = exposure_render,
};
