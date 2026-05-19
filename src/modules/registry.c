#include "module.h"

#include "cimgui.h"
#include "edit/stack.h"

#include <string.h>

extern const ap_module module_raw_passthrough;
extern const ap_module module_demosaic;
extern const ap_module module_wb;
extern const ap_module module_profile;
extern const ap_module module_exposure;
extern const ap_module module_tone;
extern const ap_module module_saturation;
extern const ap_module module_vignette;
extern const ap_module module_sharpen;
extern const ap_module module_transform;
extern const ap_module module_grain;
extern const ap_module module_color_grade;
extern const ap_module module_hsl;
extern const ap_module module_denoise;
extern const ap_module module_output_transfer;

const ap_module *const ap_module_registry[] = {
    &module_raw_passthrough,
    &module_demosaic,
    &module_wb,
    &module_profile,
    &module_exposure,
    &module_tone,
    &module_color_grade,
    &module_hsl,
    &module_saturation,
    &module_vignette,
    &module_denoise,
    &module_sharpen,
    &module_grain,
    &module_transform,
    &module_output_transfer,
    NULL,
};

const ap_module *ap_module_find(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (const ap_module *const *p = ap_module_registry; *p; p++) {
        if (strcmp((*p)->name, name) == 0) {
            return *p;
        }
    }
    return NULL;
}

void ap_module_resolve(const ap_module *self, const float *params,
                       ap_module_active *out)
{
    if (!out) return;
    out->display_name = "";
    out->variant_idx  = 0;
    if (!self) {
        out->spv_data  = NULL;
        out->spv_size  = 0;
        out->push_size = 0;
        out->pack_push = NULL;
        return;
    }
    if (self->variant_count > 0 && self->variants) {
        int slot = self->variant_param_slot;
        int idx  = 0;
        if (params && slot >= 0 && slot < AP_EDIT_PARAMS_SLOTS) {
            idx = (int)params[slot];
        }
        if (idx < 0) idx = 0;
        if (idx >= self->variant_count) idx = self->variant_count - 1;
        const ap_module_variant *v = &self->variants[idx];
        out->spv_data     = v->spv_data;
        out->spv_size     = v->spv_size;
        out->push_size    = v->push_size;
        out->pack_push    = v->pack_push;
        out->display_name = v->display_name ? v->display_name : "";
        out->variant_idx  = idx;
        return;
    }
    out->spv_data  = self->spv_data;
    out->spv_size  = self->spv_size;
    out->push_size = self->push_size;
    out->pack_push = self->pack_push;
}

bool ap_module_render_variant_combo(const ap_module *self, float *params)
{
    if (!self || !params) return false;
    if (self->variant_count <= 0 || !self->variants) return false;
    if (self->variant_param_slot < 0 ||
        self->variant_param_slot >= AP_EDIT_PARAMS_SLOTS) {
        return false;
    }

    int current = (int)params[self->variant_param_slot];
    if (current < 0) current = 0;
    if (current >= self->variant_count) current = self->variant_count - 1;

    // Build a flat "Name1\0Name2\0Name3\0\0" for igCombo_Str. Bounded
    // by ap_module_variant::display_name which is short identifier-y
    // text; 2 KB is more than enough for any realistic variant list.
    char combo_items[2048];
    size_t off = 0;
    for (int i = 0; i < self->variant_count; i++) {
        const char *n = self->variants[i].display_name
            ? self->variants[i].display_name : "(unnamed)";
        size_t len = strlen(n);
        if (off + len + 2 >= sizeof(combo_items)) break;
        memcpy(combo_items + off, n, len + 1); // includes NUL
        off += len + 1;
    }
    combo_items[off]     = '\0';  // double-NUL terminator

    int picked = current;
    bool changed = igCombo_Str("Algorithm", &picked, combo_items, -1);
    if (changed) {
        params[self->variant_param_slot] = (float)picked;
    }
    return changed;
}
