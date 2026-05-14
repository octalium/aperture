#include "module.h"

#include "tone_comp_spv.h"

#include "cimgui.h"

typedef struct {
    float contrast;
    float pivot;
} tone_push_t;

enum { SLOT_CONTRAST = 0, SLOT_PIVOT = 1 };

static const float       tone_defaults[] = { 1.0f, 0.18f };
static const char *const tone_names[]    = { "contrast", "pivot" };

static int tone_pack_push(const ap_module *self,
                          const float *params,
                          const ap_raw_metadata *meta,
                          void *push_out)
{
    (void)self;
    (void)meta;
    tone_push_t *pc = push_out;
    pc->contrast = params ? params[SLOT_CONTRAST] : 1.0f;
    pc->pivot    = params ? params[SLOT_PIVOT]    : 0.18f;
    return 0;
}

static void tone_render(const ap_module *self, float *params)
{
    if (!params) return;
    igSliderFloat("Contrast", &params[SLOT_CONTRAST], 0.5f,  4.0f, "%.2f", 0);
    if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
        params[SLOT_CONTRAST] = self->params_default[SLOT_CONTRAST];
    }
    igSliderFloat("Pivot",    &params[SLOT_PIVOT],    0.05f, 0.5f, "%.3f", 0);
    if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
        params[SLOT_PIVOT] = self->params_default[SLOT_PIVOT];
    }
}

const ap_module module_tone = {
    .name           = "tone",
    .display_name   = "Tone",
    .category       = AP_MODULE_TONE,
    .user_visible   = true,
    .spv_data       = tone_comp_spv,
    .spv_size       = tone_comp_spv_size,
    .push_size      = sizeof(tone_push_t),
    .pack_push      = tone_pack_push,
    .params_count   = 2,
    .params_default = tone_defaults,
    .params_names   = tone_names,
    .render_params  = tone_render,
};
