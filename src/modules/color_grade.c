#include "module.h"

#include "color_grade_comp_spv.h"

#include "cimgui.h"

typedef struct {
    float lift_r,  lift_g,  lift_b;
    float gamma_r, gamma_g, gamma_b;
    float gain_r,  gain_g,  gain_b;
} color_grade_push_t;

enum {
    SLOT_LIFT_R  = 0, SLOT_LIFT_G  = 1, SLOT_LIFT_B  = 2,
    SLOT_GAMMA_R = 3, SLOT_GAMMA_G = 4, SLOT_GAMMA_B = 5,
    SLOT_GAIN_R  = 6, SLOT_GAIN_G  = 7, SLOT_GAIN_B  = 8,
};

static const float color_grade_defaults[] = {
    0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f,
};
static const char *const color_grade_names[] = {
    "lift_r",  "lift_g",  "lift_b",
    "gamma_r", "gamma_g", "gamma_b",
    "gain_r",  "gain_g",  "gain_b",
};

static int color_grade_pack_push(const ap_module *self,
                                 const float *params,
                                 const ap_raw_metadata *meta,
                                 void *push_out)
{
    (void)self;
    (void)meta;
    color_grade_push_t *pc = push_out;
    pc->lift_r  = params ? params[SLOT_LIFT_R]  : 0.0f;
    pc->lift_g  = params ? params[SLOT_LIFT_G]  : 0.0f;
    pc->lift_b  = params ? params[SLOT_LIFT_B]  : 0.0f;
    pc->gamma_r = params ? params[SLOT_GAMMA_R] : 0.0f;
    pc->gamma_g = params ? params[SLOT_GAMMA_G] : 0.0f;
    pc->gamma_b = params ? params[SLOT_GAMMA_B] : 0.0f;
    pc->gain_r  = params ? params[SLOT_GAIN_R]  : 0.0f;
    pc->gain_g  = params ? params[SLOT_GAIN_G]  : 0.0f;
    pc->gain_b  = params ? params[SLOT_GAIN_B]  : 0.0f;
    return 0;
}

static void rgb_row(const ap_module *self, float *params,
                    const char *label, int slot_r,
                    float lo, float hi)
{
    igText("%s", label);
    igPushID_Str(label);
    igSetNextItemWidth(120.0f);
    igSliderFloat("R", &params[slot_r],     lo, hi, "%.3f", 0);
    if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
        params[slot_r] = self->params_default[slot_r];
    }
    igSameLine(0.0f, -1.0f);
    igSetNextItemWidth(120.0f);
    igSliderFloat("G", &params[slot_r + 1], lo, hi, "%.3f", 0);
    if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
        params[slot_r + 1] = self->params_default[slot_r + 1];
    }
    igSameLine(0.0f, -1.0f);
    igSetNextItemWidth(120.0f);
    igSliderFloat("B", &params[slot_r + 2], lo, hi, "%.3f", 0);
    if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
        params[slot_r + 2] = self->params_default[slot_r + 2];
    }
    igPopID();
}

static void color_grade_render(const ap_module *self, float *params,
                          const ap_module_render_ctx *ctx)
{
    (void)ctx;
    if (!params) return;
    rgb_row(self, params, "Lift (shadows)",     SLOT_LIFT_R,  -0.5f, 0.5f);
    rgb_row(self, params, "Gamma (midtones)",   SLOT_GAMMA_R, -1.0f, 1.0f);
    rgb_row(self, params, "Gain (highlights)",  SLOT_GAIN_R,  -1.0f, 1.0f);
    igTextDisabled("ASC CDL form; range-weighted wheel UI is a follow-up");
}

const ap_module module_color_grade = {
    .name           = "color_grade",
    .display_name   = "Color Grade",
    .category       = AP_MODULE_TONE,
    .user_visible   = true,
    .spv_data       = color_grade_comp_spv,
    .spv_size       = color_grade_comp_spv_size,
    .push_size      = sizeof(color_grade_push_t),
    .pack_push      = color_grade_pack_push,
    .params_count   = 9,
    .params_default = color_grade_defaults,
    .params_names   = color_grade_names,
    .render_params  = color_grade_render,
};
