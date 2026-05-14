#include "module.h"

#include "raw_passthrough_comp_spv.h"

#include <string.h>

typedef struct {
    uint32_t sensor_size_flip[4]; // .x = sensor_w, .y = sensor_h, .z = flip
} raw_passthrough_push_t;

static int raw_passthrough_pack_push(const ap_module *self,
                                     const float *params,
                                     const ap_raw_metadata *meta,
                                     void *push_out)
{
    (void)self;
    (void)params;
    if (!meta) {
        return -1;
    }
    raw_passthrough_push_t *pc = push_out;
    memset(pc, 0, sizeof(*pc));
    pc->sensor_size_flip[0] = (uint32_t)meta->sensor_width;
    pc->sensor_size_flip[1] = (uint32_t)meta->sensor_height;
    pc->sensor_size_flip[2] = (uint32_t)meta->flip;
    pc->sensor_size_flip[3] = 0;
    return 0;
}

// Transport: R16 Bayer -> RGBA16F grayscale passthrough. Auto-inserted
// by the pipeline graph when there's no Demosaic on the chain, so the
// downstream modules (Color, Exposure, ...) still receive an RGBA16F
// input format. Also the body of the "View Raw" mode. Honors the
// EXIF flip so portrait shots stay upright when Respect EXIF
// Orientation is on.
//
// user_visible = false; appears nowhere in the Tools palette.
const ap_module module_raw_passthrough = {
    .name         = "raw_passthrough",
    .display_name = "Raw Passthrough",
    .category     = AP_MODULE_INPUT,
    .user_visible = false,
    .spv_data     = raw_passthrough_comp_spv,
    .spv_size     = raw_passthrough_comp_spv_size,
    .push_size    = sizeof(raw_passthrough_push_t),
    .pack_push    = raw_passthrough_pack_push,
};
