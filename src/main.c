#include "core/log.h"
#include "gpu/gpu.h"
#include "gpu/texture.h"
#include "io/raw.h"
#include "ui/imgui.h"

#include <stdint.h>

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

    ap_texture *texture = NULL;
    uint64_t    tex_id  = 0;
    int         img_w   = 0;
    int         img_h   = 0;

    if (argc > 1) {
        ap_raw_image raw = {0};
        if (ap_raw_load(argv[1], &raw) != 0) {
            ap_gpu_wait_idle(g);
            ap_gpu_destroy(g);
            return 1;
        }

        texture = ap_texture_create_rgba8(g, raw.pixels, raw.width, raw.height);
        ap_raw_image_free(&raw);
        if (!texture) {
            ap_gpu_wait_idle(g);
            ap_gpu_destroy(g);
            return 1;
        }

        tex_id = ap_imgui_register_texture(ap_texture_sampler(texture),
                                           ap_texture_view(texture),
                                           ap_texture_layout(texture));
        img_w  = ap_texture_width(texture);
        img_h  = ap_texture_height(texture);
    }

    int rc = 0;
    while (ap_gpu_should_run(g)) {
        ap_imgui_new_frame();
        ap_imgui_demo_window("aperture", APERTURE_VERSION);
        if (texture) {
            ap_imgui_viewport_window("image", tex_id, img_w, img_h);
        }
        if (ap_gpu_render_frame(g) < 0) {
            rc = 1;
            break;
        }
    }

    ap_gpu_wait_idle(g);
    if (texture) {
        ap_imgui_unregister_texture(tex_id);
        ap_texture_destroy(texture);
    }
    ap_gpu_destroy(g);
    return rc;
}
