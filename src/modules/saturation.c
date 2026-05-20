#include "module.h"

#include "saturation_comp_spv.h"

#include "cimgui.h"

typedef struct {
    float saturation;
    float vibrance;
} saturation_push_t;

enum { SLOT_SAT = 0, SLOT_VIB = 1 };

static const float       saturation_defaults[] = { 0.0f, 0.0f };
static const char *const saturation_names[]    = { "saturation", "vibrance" };

static int saturation_pack_push(const ap_module *self,
                                const float *params,
                                const char (*str_params)[AP_EDIT_STR_LEN],
                                const ap_raw_metadata *meta,
                                void *push_out)
{
    (void)self;
    (void)str_params;
    (void)meta;
    saturation_push_t *pc = push_out;
    pc->saturation = params ? params[SLOT_SAT] : 0.0f;
    pc->vibrance   = params ? params[SLOT_VIB] : 0.0f;
    return 0;
}

static void saturation_render(const ap_module *self, float *params,
                          const ap_module_render_ctx *ctx)
{
    (void)ctx;
    if (!params) return;
    igSliderFloat("Saturation", &params[SLOT_SAT], -1.0f, 1.0f, "%.2f", 0);
    if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
        params[SLOT_SAT] = self->params_default[SLOT_SAT];
    }
    igSliderFloat("Vibrance",   &params[SLOT_VIB], -1.0f, 1.0f, "%.2f", 0);
    if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
        params[SLOT_VIB] = self->params_default[SLOT_VIB];
    }
}

const ap_module module_saturation = {
    .name           = "saturation",
    .display_name   = "Saturation",
    .category       = AP_MODULE_TONE,
    .user_visible   = true,
    .spv_data       = saturation_comp_spv,
    .spv_size       = saturation_comp_spv_size,
    .push_size      = sizeof(saturation_push_t),
    .pack_push      = saturation_pack_push,
    .params_count   = 2,
    .params_default = saturation_defaults,
    .params_names   = saturation_names,
    .render_params  = saturation_render,
};
