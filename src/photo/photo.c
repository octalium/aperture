#include "photo.h"

#include "core/log.h"
#include "gpu/texture.h"
#include "io/raw.h"
#include "modules/module.h"
#include "output/jpeg.h"
#include "sidecar/sidecar.h"
#include "ui/imgui.h"

#include <stdlib.h>
#include <string.h>

struct ap_photo {
    ap_gpu *gpu;
    char   *path;

    int width;
    int height;

    ap_texture        *texture;
    ap_pipeline_graph *graph;
    uint64_t           tex_id;

    ap_edit_state edit;
};

static char *strdup_or_null(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n + 1);
    return out;
}

ap_photo *ap_photo_open(ap_gpu *g, const char *path)
{
    if (!g || !path) {
        AP_ERROR("ap_photo_open: invalid args");
        return NULL;
    }

    ap_photo *photo = calloc(1, sizeof(*photo));
    if (!photo) {
        AP_ERROR("ap_photo_open: out of memory");
        return NULL;
    }
    photo->gpu  = g;
    photo->path = strdup_or_null(path);
    if (!photo->path) {
        AP_ERROR("ap_photo_open: path duplication failed");
        free(photo);
        return NULL;
    }
    photo->edit = (ap_edit_state){
        .exposure_ev   = 0.0f,
        .tone_contrast = 1.0f,
        .tone_pivot    = 0.18f,
    };
    if (ap_sidecar_load_edit(path, &photo->edit) == 0) {
        AP_INFO("photo: loaded sidecar for %s", path);
    }

    ap_raw_image raw = {0};
    if (ap_raw_load(path, &raw) != 0) {
        goto fail;
    }

    photo->texture = ap_texture_create_r16(g, raw.bayer,
                                           raw.bayer_width, raw.bayer_height);
    if (!photo->texture) {
        ap_raw_image_free(&raw);
        goto fail;
    }
    // Display dims (post-orientation) — what the rest of the app and the
    // ImGui viewport report. The input texture stays at sensor dims;
    // demosaic maps display coords back to sensor coords internally.
    photo->width  = raw.width;
    photo->height = raw.height;

    const ap_module *chain[] = {
        ap_module_find("demosaic"),
        ap_module_find("exposure"),
        ap_module_find("tone"),
        ap_module_find("encode"),
    };
    photo->graph = ap_pipeline_graph_create(g, photo->texture,
                                            raw.width, raw.height,
                                            chain,
                                            (int)(sizeof(chain) / sizeof(chain[0])),
                                            &raw.meta);
    ap_raw_image_free(&raw);
    if (!photo->graph) {
        goto fail;
    }

    photo->tex_id = ap_imgui_register_texture(
        ap_pipeline_graph_output_sampler(photo->graph),
        ap_pipeline_graph_output_view(photo->graph),
        ap_pipeline_graph_output_layout(photo->graph));

    return photo;

fail:
    ap_photo_close(photo);
    return NULL;
}

void ap_photo_close(ap_photo *photo)
{
    if (!photo) return;

    // Persist current edit state before tearing down.
    if (photo->path) {
        if (ap_sidecar_save_edit(photo->path, &photo->edit) != 0) {
            AP_WARN("photo: failed to save sidecar for %s", photo->path);
        }
    }

    if (photo->tex_id) {
        ap_imgui_unregister_texture(photo->tex_id);
        photo->tex_id = 0;
    }
    if (photo->graph) {
        ap_pipeline_graph_destroy(photo->graph);
        photo->graph = NULL;
    }
    if (photo->texture) {
        ap_texture_destroy(photo->texture);
        photo->texture = NULL;
    }
    free(photo->path);
    free(photo);
}

ap_pipeline_graph *ap_photo_graph(ap_photo *photo) { return photo->graph; }
ap_edit_state     *ap_photo_edit(ap_photo *photo)  { return &photo->edit; }
uint64_t           ap_photo_tex_id(const ap_photo *photo)  { return photo->tex_id; }
int                ap_photo_width(const ap_photo *photo)   { return photo->width; }
int                ap_photo_height(const ap_photo *photo)  { return photo->height; }
const char        *ap_photo_path(const ap_photo *photo)    { return photo->path; }

int ap_photo_export_jpeg(ap_photo *photo, const char *out_path, int quality)
{
    if (!photo || !out_path) {
        return -1;
    }

    size_t bytes = (size_t)photo->width * (size_t)photo->height * 4u;
    uint8_t *rgba = malloc(bytes);
    if (!rgba) {
        AP_ERROR("ap_photo_export_jpeg: out of memory (%zu bytes)", bytes);
        return -1;
    }

    int rc = ap_pipeline_graph_readback(photo->graph, rgba, bytes);
    if (rc == 0) {
        rc = ap_export_jpeg(rgba, photo->width, photo->height,
                            out_path, quality);
    }
    free(rgba);
    return rc;
}
