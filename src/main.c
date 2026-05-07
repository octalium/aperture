#include "core/log.h"
#include "gpu/gpu.h"
#include "photo/photo.h"
#include "ui/imgui.h"

#ifndef APERTURE_VERSION
#error "APERTURE_VERSION must be defined at compile time (set via meson)"
#endif

int main(int argc, char **argv)
{
    AP_INFO("aperture %s", APERTURE_VERSION);

    ap_gpu *g = ap_gpu_create(1280, 720, "aperture");
    if (!g) {
        return 1;
    }

    ap_photo *photo = NULL;
    if (argc > 1) {
        photo = ap_photo_open(g, argv[1]);
        if (!photo) {
            ap_gpu_wait_idle(g);
            ap_gpu_destroy(g);
            return 1;
        }
        ap_gpu_set_graph(g, ap_photo_graph(photo));
    }

    int rc = 0;
    while (ap_gpu_should_run(g)) {
        ap_imgui_new_frame();
        ap_imgui_demo_window("aperture", APERTURE_VERSION);
        if (photo) {
            ap_edit_state *edit = ap_photo_edit(photo);
            ap_imgui_edit_panel(&edit->exposure_ev,
                                &edit->tone_contrast,
                                &edit->tone_pivot);
            ap_imgui_viewport_window("image",
                                     ap_photo_tex_id(photo),
                                     ap_photo_width(photo),
                                     ap_photo_height(photo));
        }

        const ap_edit_state *edit = photo ? ap_photo_edit(photo) : NULL;
        if (ap_gpu_render_frame(g, edit) < 0) {
            rc = 1;
            break;
        }
    }

    ap_gpu_wait_idle(g);
    if (photo) {
        ap_gpu_set_graph(g, NULL);
        ap_photo_close(photo);
    }
    ap_gpu_destroy(g);
    return rc;
}
