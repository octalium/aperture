#include "photo.h"

#include "core/log.h"
#include "gpu/texture.h"
#include "io/raw.h"
#include "library/library.h"
#include "modules/module.h"
#include "sidecar/sidecar.h"

#include <stdlib.h>
#include <string.h>

struct ap_photo {
    ap_gpu *gpu;
    char   *path;

    int width;
    int height;

    ap_texture        *texture;
    ap_pipeline_graph *graph;

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

ap_photo *ap_photo_open_with_raw(ap_gpu *g, const char *path,
                                 ap_raw_image *raw)
{
    if (!g || !path || !raw) {
        AP_ERROR("ap_photo_open_with_raw: invalid args");
        if (raw) ap_raw_image_free(raw);
        return NULL;
    }

    ap_photo *photo = calloc(1, sizeof(*photo));
    if (!photo) {
        AP_ERROR("ap_photo_open_with_raw: out of memory");
        ap_raw_image_free(raw);
        return NULL;
    }
    photo->gpu  = g;
    photo->path = strdup_or_null(path);
    if (!photo->path) {
        AP_ERROR("ap_photo_open_with_raw: path duplication failed");
        ap_raw_image_free(raw);
        free(photo);
        return NULL;
    }
    photo->edit = (ap_edit_state){
        .exposure_ev         = 0.0f,
        .tone_contrast       = 1.0f,
        .tone_pivot          = 0.18f,
        .respect_orientation = 1,
    };
    if (ap_sidecar_load_edit(path, &photo->edit) == 0) {
        AP_INFO("photo: loaded sidecar for %s", path);
    }

    photo->texture = ap_texture_create_r16(g, raw->bayer,
                                           raw->bayer_width, raw->bayer_height);
    if (!photo->texture) {
        ap_raw_image_free(raw);
        goto fail;
    }

    // Display dims default to the post-orientation size. When the
    // user has turned EXIF orientation off for this photo, the
    // pipeline graph runs at sensor dims with the demosaic flip
    // forced to 0. The input texture is always at sensor dims.
    int output_w = raw->width;
    int output_h = raw->height;
    ap_raw_metadata graph_meta = raw->meta;
    if (!photo->edit.respect_orientation) {
        output_w = raw->bayer_width;
        output_h = raw->bayer_height;
        graph_meta.flip = 0;
    }
    photo->width  = output_w;
    photo->height = output_h;

    ap_pipeline_def def;
    if (ap_pipeline_get_default(&def) != 0 || def.module_count <= 0) {
        AP_ERROR("photo: no default pipeline available");
        ap_raw_image_free(raw);
        goto fail;
    }
    const ap_module *chain[AP_PIPELINE_MAX_MODULES] = {0};
    for (int i = 0; i < def.module_count; i++) {
        chain[i] = ap_module_find(def.modules[i]);
        if (!chain[i]) {
            AP_ERROR("photo: pipeline '%s' references unknown module '%s'",
                     def.name, def.modules[i]);
            ap_raw_image_free(raw);
            goto fail;
        }
    }
    photo->graph = ap_pipeline_graph_create(g, photo->texture,
                                            output_w, output_h,
                                            chain, def.module_count,
                                            &graph_meta);
    ap_raw_image_free(raw);
    if (!photo->graph) {
        goto fail;
    }

    return photo;

fail:
    ap_photo_close(photo);
    return NULL;
}

ap_photo *ap_photo_open(ap_gpu *g, const char *path)
{
    if (!g || !path) {
        AP_ERROR("ap_photo_open: invalid args");
        return NULL;
    }
    ap_raw_image raw = {0};
    if (ap_raw_load(path, &raw) != 0) {
        return NULL;
    }
    return ap_photo_open_with_raw(g, path, &raw);
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
int                ap_photo_width(const ap_photo *photo)   { return photo->width; }
int                ap_photo_height(const ap_photo *photo)  { return photo->height; }
const char        *ap_photo_path(const ap_photo *photo)    { return photo->path; }

