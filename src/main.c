#include "core/log.h"
#include "gpu/compute.h"
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
    ap_compute *compute = NULL;
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

        texture = ap_texture_create_r16(g, raw.bayer, raw.width, raw.height);
        if (!texture) {
            ap_raw_image_free(&raw);
            ap_gpu_wait_idle(g);
            ap_gpu_destroy(g);
            return 1;
        }

        compute = ap_compute_create(g, texture, &raw.meta);
        ap_raw_image_free(&raw);
        if (!compute) {
            ap_texture_destroy(texture);
            ap_gpu_wait_idle(g);
            ap_gpu_destroy(g);
            return 1;
        }
        ap_gpu_set_compute(g, compute);

        tex_id = ap_imgui_register_texture(ap_compute_output_sampler(compute),
                                           ap_compute_output_view(compute),
                                           ap_compute_output_layout(compute));
        img_w  = ap_compute_output_width(compute);
        img_h  = ap_compute_output_height(compute);
    }

    ap_edit_state edit = {
        .exposure_ev   = 0.0f,
        .tone_contrast = 1.0f,
        .tone_pivot    = 0.18f,
    };

    int rc = 0;
    while (ap_gpu_should_run(g)) {
        ap_imgui_new_frame();
        ap_imgui_demo_window("aperture", APERTURE_VERSION);
        if (compute) {
            ap_imgui_edit_panel(&edit.exposure_ev,
                                &edit.tone_contrast,
                                &edit.tone_pivot);
            ap_imgui_viewport_window("image", tex_id, img_w, img_h);
        }
        if (ap_gpu_render_frame(g, &edit) < 0) {
            rc = 1;
            break;
        }
    }

    ap_gpu_wait_idle(g);
    if (compute) {
        ap_imgui_unregister_texture(tex_id);
        ap_gpu_set_compute(g, NULL);
        ap_compute_destroy(compute);
    }
    if (texture) {
        ap_texture_destroy(texture);
    }
    ap_gpu_destroy(g);
    return rc;
}
