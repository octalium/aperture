#include "core/log.h"
#include "gpu/gpu.h"
#include "ui/imgui.h"

#ifndef APERTURE_VERSION
#error "APERTURE_VERSION must be defined at compile time (set via meson)"
#endif

int main(void)
{
    AP_INFO("aperture %s", APERTURE_VERSION);

    ap_gpu *g = ap_gpu_create(1280, 720, "aperture");
    if (!g) {
        return 1;
    }

    while (ap_gpu_should_run(g)) {
        ap_imgui_new_frame();
        ap_imgui_demo_window("aperture", APERTURE_VERSION);

        if (ap_gpu_render_frame(g) < 0) {
            ap_gpu_wait_idle(g);
            ap_gpu_destroy(g);
            return 1;
        }
    }

    ap_gpu_wait_idle(g);
    ap_gpu_destroy(g);
    return 0;
}
