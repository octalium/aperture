#include "module.h"

#include "encode_comp_spv.h"

#define TRANSFER_SRGB 0u

typedef struct {
    uint32_t transfer_function;
} encode_push_t;

static int encode_pack_push(const ap_module *self,
                            const ap_edit_state *edit,
                            const ap_raw_metadata *meta,
                            void *push_out)
{
    (void)self;
    (void)edit;
    (void)meta;
    encode_push_t *pc = push_out;
    pc->transfer_function = TRANSFER_SRGB;
    return 0;
}

const ap_module module_encode = {
    .name         = "encode",
    .display_name = "Output Transfer",
    .category     = AP_MODULE_OUTPUT_TRANSFER,
    .spv_data     = encode_comp_spv,
    .spv_size     = encode_comp_spv_size,
    .push_size    = sizeof(encode_push_t),
    .pack_push    = encode_pack_push,
};
