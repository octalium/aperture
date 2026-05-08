#include "module.h"

#include "exposure_comp_spv.h"

typedef struct {
    float exposure_ev;
} exposure_push_t;

static int exposure_pack_push(const ap_module *self,
                              const ap_edit_state *edit,
                              const ap_raw_metadata *meta,
                              void *push_out)
{
    (void)self;
    (void)meta;
    exposure_push_t *pc = push_out;
    pc->exposure_ev = edit ? edit->exposure_ev : 0.0f;
    return 0;
}

const ap_module module_exposure = {
    .name         = "exposure",
    .display_name = "Exposure",
    .category     = AP_MODULE_TONE,
    .spv_data     = exposure_comp_spv,
    .spv_size     = exposure_comp_spv_size,
    .push_size    = sizeof(exposure_push_t),
    .pack_push    = exposure_pack_push,
};
