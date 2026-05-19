#include "module.h"

#include "orientation_comp_spv.h"

#include "cimgui.h"

#include <math.h>

#ifndef M_PIf
#define M_PIf 3.14159265358979323846f
#endif

typedef struct {
    float angle_rad;
} orientation_push_t;

enum { SLOT_ANGLE_DEG = 0 };

static const float       orientation_defaults[] = { 0.0f };
static const char *const orientation_names[]    = { "angle_deg" };

static int orientation_pack_push(const ap_module *self,
                                 const float *params,
                                 const ap_raw_metadata *meta,
                                 void *push_out)
{
    (void)self;
    (void)meta;
    orientation_push_t *pc = push_out;
    float deg = params ? params[SLOT_ANGLE_DEG] : 0.0f;
    pc->angle_rad = deg * (M_PIf / 180.0f);
    return 0;
}

static void orientation_render(const ap_module *self, float *params)
{
    if (!params) return;
    igSliderFloat("Angle (°)", &params[SLOT_ANGLE_DEG],
                  -180.0f, 180.0f, "%.1f", 0);
    if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
        params[SLOT_ANGLE_DEG] = self->params_default[SLOT_ANGLE_DEG];
    }
}

const ap_module module_orientation = {
    .name           = "orientation",
    .display_name   = "Orientation",
    .category       = AP_MODULE_GEOMETRIC,
    .user_visible   = true,
    .spv_data       = orientation_comp_spv,
    .spv_size       = orientation_comp_spv_size,
    .push_size      = sizeof(orientation_push_t),
    .pack_push      = orientation_pack_push,
    .params_count   = 1,
    .params_default = orientation_defaults,
    .params_names   = orientation_names,
    .render_params  = orientation_render,
};
