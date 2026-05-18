#include "photo.h"

#include "core/log.h"
#include "gpu/texture.h"
#include "io/raw.h"
#include "library/library.h"
#include "modules/module.h"
#include "sidecar/sidecar.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct ap_photo {
    ap_gpu *gpu;
    char   *path;

    int width;
    int height;

    // Pre-graph state used to rebuild the graph after a stack change.
    int             sensor_w;
    int             sensor_h;
    int             display_w;     // post-orientation display dims
    int             display_h;
    ap_raw_metadata meta;

    ap_texture        *texture;
    ap_pipeline_graph *graph;

    ap_edit_stack stack;
    bool          respect_orientation;
    bool          view_raw;        // bypass user stack at graph build
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

// Build the pipeline graph from photo->stack with current orientation
// + meta + dims. Photo holds enough cached state that this can run
// at any time after the texture has been uploaded.
static int rebuild_graph(ap_photo *photo)
{
    if (!photo) return -1;

    int output_w, output_h;
    ap_raw_metadata graph_meta = photo->meta;
    if (photo->respect_orientation) {
        output_w = photo->display_w;
        output_h = photo->display_h;
    } else {
        output_w = photo->sensor_w;
        output_h = photo->sensor_h;
        graph_meta.flip = 0;
    }
    photo->width  = output_w;
    photo->height = output_h;

    if (photo->graph) {
        ap_pipeline_graph_destroy(photo->graph);
        photo->graph = NULL;
    }
    // In view-raw mode, pass an empty stack so the graph builds only
    // its auto-inserted raw_passthrough + output_transfer.
    ap_edit_stack empty;
    ap_edit_stack_init(&empty);
    const ap_edit_stack *use_stack = photo->view_raw ? &empty : &photo->stack;
    photo->graph = ap_pipeline_graph_create(photo->gpu, photo->texture,
                                            output_w, output_h,
                                            use_stack,
                                            &graph_meta);
    if (!photo->graph) {
        AP_ERROR("photo: graph build failed for %s", photo->path);
        return -1;
    }
    return 0;
}

// Seed the stack with the entries from the registry's default
// pipeline, skipping any transport modules (demosaic / encode) that
// the graph manages itself.
static void seed_stack_from_default(ap_edit_stack *stack)
{
    ap_pipeline_def def;
    if (ap_pipeline_get_default(&def) != 0) return;
    for (int i = 0; i < def.module_count; i++) {
        const ap_module *m = ap_module_find(def.modules[i]);
        if (!m || !m->user_visible) continue;
        ap_edit_stack_add(stack, def.modules[i]);
    }
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
    photo->respect_orientation = true;
    ap_edit_stack_init(&photo->stack);

    if (ap_sidecar_load(path, &photo->stack, &photo->respect_orientation) == 0) {
        AP_INFO("photo: loaded sidecar for %s", path);
    } else {
        // First open of this photo (or schema mismatch). Seed the
        // stack from the registry's default pipeline.
        seed_stack_from_default(&photo->stack);
    }

    photo->texture = ap_texture_create_r16(g, raw->bayer,
                                           raw->bayer_width, raw->bayer_height);
    if (!photo->texture) {
        ap_raw_image_free(raw);
        goto fail;
    }

    photo->sensor_w  = raw->bayer_width;
    photo->sensor_h  = raw->bayer_height;
    photo->display_w = raw->width;
    photo->display_h = raw->height;
    photo->meta      = raw->meta;
    ap_raw_image_free(raw);

    if (rebuild_graph(photo) < 0) {
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

int ap_photo_readback_rgba(ap_photo *photo,
                           uint8_t **out_rgba, int *out_w, int *out_h)
{
    if (!photo || !photo->graph || !out_rgba || !out_w || !out_h) return -1;
    int w = ap_pipeline_graph_output_width(photo->graph);
    int h = ap_pipeline_graph_output_height(photo->graph);
    if (w <= 0 || h <= 0) return -1;

    size_t bytes = (size_t)w * (size_t)h * 4u;
    uint8_t *rgba = malloc(bytes);
    if (!rgba) return -1;
    if (ap_pipeline_graph_readback(photo->graph, rgba, bytes) != 0) {
        free(rgba);
        return -1;
    }
    *out_rgba = rgba;
    *out_w    = w;
    *out_h    = h;
    return 0;
}

void ap_photo_close(ap_photo *photo)
{
    if (!photo) return;

    // Persist current edit state before tearing down. The edit-render
    // thumbnail blob is the app's job — it owns the library handle
    // and stores it via ap_library_store_thumbnail before calling
    // close.
    if (photo->path) {
        if (ap_sidecar_save(photo->path, &photo->stack,
                            photo->respect_orientation) != 0) {
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

int ap_photo_rebuild_graph(ap_photo *photo)
{
    return rebuild_graph(photo);
}

ap_pipeline_graph *ap_photo_graph(ap_photo *photo) { return photo->graph; }
ap_edit_stack     *ap_photo_stack(ap_photo *photo) { return &photo->stack; }
int                ap_photo_width(const ap_photo *photo)   { return photo->width; }
int                ap_photo_height(const ap_photo *photo)  { return photo->height; }
const char        *ap_photo_path(const ap_photo *photo)    { return photo->path; }

bool ap_photo_respect_orientation(const ap_photo *photo)
{
    return photo ? photo->respect_orientation : true;
}

void ap_photo_set_respect_orientation(ap_photo *photo, bool yes)
{
    if (!photo) return;
    photo->respect_orientation = yes;
}

bool ap_photo_view_raw(const ap_photo *photo)
{
    return photo ? photo->view_raw : false;
}

void ap_photo_set_view_raw(ap_photo *photo, bool yes)
{
    if (!photo) return;
    photo->view_raw = yes;
}
