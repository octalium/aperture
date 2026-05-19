#include "module.h"

#include "sharpen_comp_spv.h"

#include "cimgui.h"

typedef struct {
    float amount;
    float threshold;
} sharpen_push_t;

enum { SLOT_AMOUNT = 0, SLOT_THRESHOLD = 1 };

static const float       sharpen_defaults[] = { 0.0f, 0.0f };
static const char *const sharpen_names[]    = { "amount", "threshold" };

static int sharpen_pack_push(const ap_module *self,
                             const float *params,
                             const ap_raw_metadata *meta,
                             void *push_out)
{
    (void)self;
    (void)meta;
    sharpen_push_t *pc = push_out;
    pc->amount    = params ? params[SLOT_AMOUNT]    : 0.0f;
    pc->threshold = params ? params[SLOT_THRESHOLD] : 0.0f;
    return 0;
}

static void sharpen_render(const ap_module *self, float *params,
                          const ap_module_render_ctx *ctx)
{
    (void)ctx;
    if (!params) return;
    igSliderFloat("Amount",    &params[SLOT_AMOUNT],    0.0f, 5.0f,  "%.2f", 0);
    if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
        params[SLOT_AMOUNT] = self->params_default[SLOT_AMOUNT];
    }
    igSliderFloat("Threshold", &params[SLOT_THRESHOLD], 0.0f, 0.10f, "%.4f", 0);
    if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
        params[SLOT_THRESHOLD] = self->params_default[SLOT_THRESHOLD];
    }
}

const ap_module module_sharpen = {
    .name           = "sharpen",
    .display_name   = "Sharpen",
    .category       = AP_MODULE_DETAIL,
    .user_visible   = true,
    .spv_data       = sharpen_comp_spv,
    .spv_size       = sharpen_comp_spv_size,
    .push_size      = sizeof(sharpen_push_t),
    .pack_push      = sharpen_pack_push,
    .params_count   = 2,
    .params_default = sharpen_defaults,
    .params_names   = sharpen_names,
    .render_params  = sharpen_render,
};
