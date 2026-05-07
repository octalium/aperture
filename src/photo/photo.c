#include "photo.h"

#include "core/log.h"
#include "gpu/texture.h"
#include "io/raw.h"
#include "modules/module.h"
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

    photo->texture = ap_texture_create_rgba8(g, raw.pixels, raw.width, raw.height);
    ap_raw_image_free(&raw);
    if (!photo->texture) {
        goto fail;
    }
    photo->width  = ap_texture_width(photo->texture);
    photo->height = ap_texture_height(photo->texture);

    const ap_module *chain[] = {
        ap_module_find("process"),
        ap_module_find("tone"),
        ap_module_find("encode"),
    };
    photo->graph = ap_pipeline_graph_create(g, photo->texture, chain,
                                            (int)(sizeof(chain) / sizeof(chain[0])));
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
