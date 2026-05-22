#include "module.h"

#include "vignette_comp_spv.h"

#include "cimgui.h"

typedef struct {
    float amount;
    float midpoint;
    float feather;
} vignette_push_t;

enum { SLOT_AMOUNT = 0, SLOT_MIDPOINT = 1, SLOT_FEATHER = 2 };

static const float       vignette_defaults[] = { 0.0f, 0.7f, 0.5f };
static const char *const vignette_names[]    = { "amount", "midpoint", "feather" };

static int vignette_pack_push(const ap_module *self,
                              const float *params,
                              const char (*str_params)[AP_EDIT_STR_LEN],
                              const ap_raw_metadata *meta,
                              void *push_out)
{
    (void)self;
    (void)str_params;
    (void)meta;
    vignette_push_t *pc = push_out;
    pc->amount   = params ? params[SLOT_AMOUNT]   : 0.0f;
    pc->midpoint = params ? params[SLOT_MIDPOINT] : 0.7f;
    pc->feather  = params ? params[SLOT_FEATHER]  : 0.5f;
    return 0;
}

static void vignette_render(const ap_module *self, float *params,
                          const ap_module_render_ctx *ctx)
{
    (void)ctx;
    if (!params) return;
    ap_module_slider_reset(self, params, "Amount",   SLOT_AMOUNT,   -1.0f, 1.0f, "%.2f");
    ap_module_slider_reset(self, params, "Midpoint", SLOT_MIDPOINT,  0.0f, 1.0f, "%.2f");
    ap_module_slider_reset(self, params, "Feather",  SLOT_FEATHER,   0.0f, 1.0f, "%.2f");
}

const ap_module module_vignette = {
    .name           = "vignette",
    .display_name   = "Vignette",
    .category       = AP_MODULE_TONE,
    .user_visible   = true,
    .spv_data       = vignette_comp_spv,
    .spv_size       = vignette_comp_spv_size,
    .push_size      = sizeof(vignette_push_t),
    .pack_push      = vignette_pack_push,
    .params_count   = 3,
    .params_default = vignette_defaults,
    .params_names   = vignette_names,
    .render_params  = vignette_render,
};
