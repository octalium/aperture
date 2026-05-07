#include "module.h"

#include "process_comp_spv.h"

typedef struct {
    float exposure_ev;
} process_push_t;

static int process_pack_push(const ap_module *self,
                             const ap_edit_state *edit,
                             void *push_out)
{
    (void)self;
    process_push_t *pc = push_out;
    pc->exposure_ev = edit ? edit->exposure_ev : 0.0f;
    return 0;
}

const ap_module module_process = {
    .name         = "process",
    .display_name = "Process",
    .category     = AP_MODULE_TONE,
    .spv_data     = process_comp_spv,
    .spv_size     = process_comp_spv_size,
    .push_size    = sizeof(process_push_t),
    .pack_push    = process_pack_push,
};
