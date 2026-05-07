#include "module.h"

#include "tone_comp_spv.h"

typedef struct {
    float contrast;
    float pivot;
} tone_push_t;

static int tone_pack_push(const ap_module *self,
                          const ap_edit_state *edit,
                          void *push_out)
{
    (void)self;
    tone_push_t *pc = push_out;
    pc->contrast = edit ? edit->tone_contrast : 1.0f;
    pc->pivot    = edit ? edit->tone_pivot    : 0.18f;
    return 0;
}

const ap_module module_tone = {
    .name         = "tone",
    .display_name = "Tone",
    .category     = AP_MODULE_TONE,
    .spv_data     = tone_comp_spv,
    .spv_size     = tone_comp_spv_size,
    .push_size    = sizeof(tone_push_t),
    .pack_push    = tone_pack_push,
};
