#include "module.h"

#include "output_transfer_comp_spv.h"

#define TRANSFER_SRGB 0u

typedef struct {
    uint32_t transfer_function;
} output_transfer_push_t;

static int output_transfer_pack_push(const ap_module *self,
                                     const float *params,
                                     const ap_raw_metadata *meta,
                                     void *push_out)
{
    (void)self;
    (void)params;
    (void)meta;
    output_transfer_push_t *pc = push_out;
    pc->transfer_function = TRANSFER_SRGB;
    return 0;
}

// Transport module: always appended by the pipeline graph, never
// surfaced in the Tools palette. user_visible = false. The display
// loop renders sRGB so we always finish with the linear → sRGB EOTF;
// if multiple output transfers ever become user-selectable, this
// gets a real config UI and flips user_visible.
const ap_module module_output_transfer = {
    .name           = "output_transfer",
    .display_name   = "Output Transfer",
    .category       = AP_MODULE_OUTPUT_TRANSFER,
    .user_visible   = false,
    .spv_data       = output_transfer_comp_spv,
    .spv_size       = output_transfer_comp_spv_size,
    .push_size      = sizeof(output_transfer_push_t),
    .pack_push      = output_transfer_pack_push,
};
