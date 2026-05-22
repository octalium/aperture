#include "module.h"

#include "hsl_comp_spv.h"

#include "cimgui.h"

// 8 bands * 3 knobs (H, S, L) = 24 floats. Fits in the bumped 32-slot
// edit-entry cap (#108). The push constant mirrors the shader's
// layout: three separate arrays of 8 floats each.

typedef struct {
    float h[8];
    float s[8];
    float l[8];
} hsl_push_t;

#define N_BANDS 8

enum {
    SLOT_H_BASE = 0,           // h[0..7] → slots [0..7]
    SLOT_S_BASE = N_BANDS,     // s[0..7] → slots [8..15]
    SLOT_L_BASE = 2 * N_BANDS, // l[0..7] → slots [16..23]
};

static const char *const BAND_NAMES[N_BANDS] = {
    "Red", "Orange", "Yellow", "Green",
    "Aqua", "Blue", "Purple", "Magenta",
};

static const float hsl_defaults[3 * N_BANDS] = {0};

static const char *const hsl_names[3 * N_BANDS] = {
    // H
    "h_red", "h_orange", "h_yellow", "h_green",
    "h_aqua", "h_blue", "h_purple", "h_magenta",
    // S
    "s_red", "s_orange", "s_yellow", "s_green",
    "s_aqua", "s_blue", "s_purple", "s_magenta",
    // L
    "l_red", "l_orange", "l_yellow", "l_green",
    "l_aqua", "l_blue", "l_purple", "l_magenta",
};

static int hsl_pack_push(const ap_module *self,
                         const float *params,
                         const char (*str_params)[AP_EDIT_STR_LEN],
                         const ap_raw_metadata *meta,
                         void *push_out)
{
    (void)self;
    (void)str_params;
    (void)meta;
    hsl_push_t *pc = push_out;
    for (int i = 0; i < N_BANDS; i++) {
        pc->h[i] = params ? params[SLOT_H_BASE + i] : 0.0f;
        pc->s[i] = params ? params[SLOT_S_BASE + i] : 0.0f;
        pc->l[i] = params ? params[SLOT_L_BASE + i] : 0.0f;
    }
    return 0;
}

// Width of the band-name label column and each per-channel slider in the
// HSL grid. Kept as named constants so the layout can be tuned in one place.
#define HSL_LABEL_COL_W 80.0f
#define HSL_SLIDER_W    110.0f

static void band_row(const ap_module *self, float *params, int band)
{
    igPushID_Int(band);
    igText("%-7s", BAND_NAMES[band]);
    igSameLine(HSL_LABEL_COL_W, 0.0f);
    igSetNextItemWidth(HSL_SLIDER_W);
    ap_module_slider_reset(self, params, "H", SLOT_H_BASE + band, -0.1f, 0.1f, "%.3f");
    igSameLine(0.0f, -1.0f);
    igSetNextItemWidth(HSL_SLIDER_W);
    ap_module_slider_reset(self, params, "S", SLOT_S_BASE + band, -1.0f, 1.0f, "%.2f");
    igSameLine(0.0f, -1.0f);
    igSetNextItemWidth(HSL_SLIDER_W);
    ap_module_slider_reset(self, params, "L", SLOT_L_BASE + band, -0.5f, 0.5f, "%.2f");
    igPopID();
}

static void hsl_render(const ap_module *self, float *params,
                          const ap_module_render_ctx *ctx)
{
    (void)ctx;
    if (!params) return;
    for (int i = 0; i < N_BANDS; i++) band_row(self, params, i);
    igTextDisabled("H = hue shift, S = saturation scale, L = luma offset");
}

const ap_module module_hsl = {
    .name           = "hsl",
    .display_name   = "HSL",
    .category       = AP_MODULE_TONE,
    .user_visible   = true,
    .spv_data       = hsl_comp_spv,
    .spv_size       = hsl_comp_spv_size,
    .push_size      = sizeof(hsl_push_t),
    .pack_push      = hsl_pack_push,
    .params_count   = 3 * N_BANDS,
    .params_default = hsl_defaults,
    .params_names   = hsl_names,
    .render_params  = hsl_render,
};
