#include "module.h"

#include "raw_passthrough_comp_spv.h"

// Transport: R16 Bayer -> RGBA16F grayscale passthrough. Auto-inserted
// by the pipeline graph when there's no Demosaic on the chain, so the
// downstream modules (Color, Exposure, ...) still receive an RGBA16F
// input format. Also the body of the "View Raw" mode.
//
// user_visible = false; appears nowhere in the Tools palette.
const ap_module module_raw_passthrough = {
    .name         = "raw_passthrough",
    .display_name = "Raw Passthrough",
    .category     = AP_MODULE_INPUT,
    .user_visible = false,
    .spv_data     = raw_passthrough_comp_spv,
    .spv_size     = raw_passthrough_comp_spv_size,
    .push_size    = 0,
    .pack_push    = NULL,
};
