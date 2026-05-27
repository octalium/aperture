// fixture module definitions for tests. linked into any test that
// exercises edit-stack pack/unpack — sidecar, library, modules. supplies
// ap_module_registry + ap_module_find so the production registry.c
// (which transitively pulls cimgui / vulkan / lensfun) stays out of the
// test binary.

#include "aptest_modules.h"

#include <string.h>

const ap_module aptest_module_demosaic = {
    .name           = "demosaic",
    .display_name   = "Demosaic",
    .category       = AP_MODULE_INPUT,
    .user_visible   = false,
    .params_count   = 0,
    .params_default = NULL,
    .params_names   = NULL,
};

const ap_module aptest_module_wb = {
    .name           = "wb",
    .display_name   = "White Balance",
    .category       = AP_MODULE_COLOR,
    .user_visible   = true,
    .params_count   = 0,
    .params_default = NULL,
    .params_names   = NULL,
};

const ap_module aptest_module_profile = {
    .name           = "profile",
    .display_name   = "Color Profile",
    .category       = AP_MODULE_COLOR,
    .user_visible   = true,
    .params_count   = 0,
    .params_default = NULL,
    .params_names   = NULL,
};

// color_grade: 9 float params (lift/gamma/gain x RGB), mirrors
// src/modules/color_grade.c's shape.
static const float color_grade_defaults[9] = {
    0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f,
};
static const char *const color_grade_names[9] = {
    "lift_r",  "lift_g",  "lift_b",
    "gamma_r", "gamma_g", "gamma_b",
    "gain_r",  "gain_g",  "gain_b",
};
const ap_module aptest_module_color_grade = {
    .name           = "color_grade",
    .display_name   = "Color Grade",
    .category       = AP_MODULE_COLOR,
    .user_visible   = true,
    .params_count   = 9,
    .params_default = color_grade_defaults,
    .params_names   = color_grade_names,
};

// lens_correction: matches the production module's surface (focal_mm,
// aperture, do_distortion, do_vignetting + camera/lens override strings).
static const float lens_defaults[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
static const char *const lens_param_names[4] = {
    "focal_mm", "aperture", "do_distortion", "do_vignetting",
};
static const char *const lens_str_names[2] = {
    "camera_override", "lens_override",
};
const ap_module aptest_module_lens_correction = {
    .name             = "lens_correction",
    .display_name     = "Lens Correction",
    .category         = AP_MODULE_GEOMETRIC,
    .user_visible     = true,
    .params_count     = 4,
    .params_default   = lens_defaults,
    .params_names     = lens_param_names,
    .str_params_count = 2,
    .str_params_names = lens_str_names,
};

// chromatic_aberration: mirrors production (mode, focal_mm, r_scale,
// b_scale, r_offset, b_offset + camera/lens override strings).
static const float ca_defaults[6] = {
    0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f,
};
static const char *const ca_param_names[6] = {
    "mode", "focal_mm", "r_scale", "b_scale", "r_offset", "b_offset",
};
static const char *const ca_str_names[2] = {
    "camera_override", "lens_override",
};
const ap_module aptest_module_chromatic_aberration = {
    .name             = "chromatic_aberration",
    .display_name     = "Chromatic Aberration",
    .category         = AP_MODULE_GEOMETRIC,
    .user_visible     = true,
    .params_count     = 6,
    .params_default   = ca_defaults,
    .params_names     = ca_param_names,
    .str_params_count = 2,
    .str_params_names = ca_str_names,
};

const ap_module *const ap_module_registry[] = {
    &aptest_module_demosaic,
    &aptest_module_wb,
    &aptest_module_profile,
    &aptest_module_color_grade,
    &aptest_module_lens_correction,
    &aptest_module_chromatic_aberration,
    NULL,
};

const ap_module *ap_module_find(const char *name)
{
    if (!name) return NULL;
    for (const ap_module *const *p = ap_module_registry; *p; p++) {
        if (strcmp((*p)->name, name) == 0) return *p;
    }
    return NULL;
}

void ap_module_resolve(const ap_module *self, const float *params,
                       ap_module_active *out)
{
    if (!out) return;
    *out = (ap_module_active){0};
    if (!self) return;
    out->push_size = self->push_size;
    out->pack_push = self->pack_push;
    (void)params;
}
