#include "photo.h"

#include "core/log.h"
#include "edit/history.h"
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

    ap_edit_stack   stack;
    ap_edit_history history;
    bool            respect_orientation;
    bool            view_raw;      // bypass user stack at graph build

    // Metadata: file_meta is what the loader extracted from the raw
    // file; user_meta is the per-field overlay persisted in the
    // sidecar. user_set marks which fields the overlay defines.
    // Display merges them (overlay wins when set, file otherwise).
    ap_photo_metadata file_meta;
    ap_photo_metadata user_meta;
    bool              user_set[AP_META_FIELD_COUNT];

    // Culling state — rating / flag / colour — persisted in the
    // sidecar's [metadata] table. Carried through so a sidecar save
    // on close preserves it.
    ap_photo_culling  culling;

    // Group membership, persisted in the sidecar. Carried through so a
    // sidecar save on close preserves it.
    ap_photo_groups   groups;

    // Keyword list, persisted in the sidecar's [metadata] `keywords`
    // array. Carried through so a sidecar save on close preserves it.
    ap_photo_keywords keywords;
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

// Seed the stack from the registry's default pipeline. User-visible
// modules only (transport modules like demosaic/encode are managed by
// the graph). Param defaults come from each module's params_default.
static void seed_stack_from_default(ap_edit_stack *stack)
{
    ap_pipeline_apply_default_to_stack(stack);
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
    ap_edit_history_init(&photo->history);
    ap_photo_metadata_clear(&photo->user_meta);
    for (int i = 0; i < AP_META_FIELD_COUNT; i++) photo->user_set[i] = false;
    ap_photo_culling_clear(&photo->culling);
    ap_photo_keywords_clear(&photo->keywords);

    if (ap_sidecar_load(path, &photo->stack, &photo->respect_orientation,
                        &photo->user_meta, photo->user_set,
                        &photo->culling, &photo->groups,
                        &photo->keywords) == 0) {
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
    photo->file_meta = raw->file_meta;
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
    int tw = ap_pipeline_graph_thumb_width(photo->graph);
    int th = ap_pipeline_graph_thumb_height(photo->graph);
    if (tw <= 0 || th <= 0) return -1;

    size_t bytes = (size_t)tw * (size_t)th * 4u;
    uint8_t *rgba = malloc(bytes);
    if (!rgba) return -1;
    int got_w = 0, got_h = 0;
    if (ap_pipeline_graph_readback_thumb(photo->graph, rgba, bytes,
                                         &got_w, &got_h) != 0) {
        free(rgba);
        return -1;
    }
    *out_rgba = rgba;
    *out_w    = got_w;
    *out_h    = got_h;
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
                            photo->respect_orientation,
                            &photo->user_meta, photo->user_set,
                            &photo->culling, &photo->groups,
                            &photo->keywords) != 0) {
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

ap_viewport ap_photo_viewport(const ap_photo *photo)
{
    if (photo) {
        int n = ap_edit_stack_count(&photo->stack);
        for (int i = 0; i < n; i++) {
            const ap_edit_entry *e = ap_edit_stack_at_const(&photo->stack, i);
            if (!e || !e->enabled) continue;
            if (strcmp(e->module_name, "transform") != 0) continue;
            return ap_transform_viewport(e->params);
        }
    }
    return ap_viewport_identity();
}

bool ap_photo_respect_orientation(const ap_photo *photo)
{
    return photo ? photo->respect_orientation : true;
}

void ap_photo_set_respect_orientation(ap_photo *photo, bool yes)
{
    if (!photo) return;
    photo->respect_orientation = yes;
}

const char *ap_photo_metadata_value(const ap_photo *photo, ap_meta_field f)
{
    if (!photo || f < 0 || f >= AP_META_FIELD_COUNT) return "";
    if (photo->user_set[f]) {
        return ap_photo_metadata_get(&photo->user_meta, f);
    }
    return ap_photo_metadata_get(&photo->file_meta, f);
}

const char *ap_photo_metadata_file_value(const ap_photo *photo,
                                         ap_meta_field f)
{
    if (!photo || f < 0 || f >= AP_META_FIELD_COUNT) return "";
    return ap_photo_metadata_get(&photo->file_meta, f);
}

const ap_photo_metadata *ap_photo_file_meta(const ap_photo *photo)
{
    if (!photo) return NULL;
    return &photo->file_meta;
}

bool ap_photo_metadata_is_user(const ap_photo *photo, ap_meta_field f)
{
    if (!photo || f < 0 || f >= AP_META_FIELD_COUNT) return false;
    return photo->user_set[f];
}

void ap_photo_metadata_set_user(ap_photo *photo, ap_meta_field f,
                                const char *value)
{
    if (!photo || f < 0 || f >= AP_META_FIELD_COUNT) return;
    ap_photo_metadata_set(&photo->user_meta, f, value);
    photo->user_set[f] = true;
}

void ap_photo_metadata_reset(ap_photo *photo, ap_meta_field f)
{
    if (!photo || f < 0 || f >= AP_META_FIELD_COUNT) return;
    ap_photo_metadata_set(&photo->user_meta, f, "");
    photo->user_set[f] = false;
}

ap_photo_culling ap_photo_get_culling(const ap_photo *photo)
{
    if (!photo) {
        ap_photo_culling empty;
        ap_photo_culling_clear(&empty);
        return empty;
    }
    return photo->culling;
}

void ap_photo_set_rating(ap_photo *photo, int rating)
{
    if (!photo) return;
    photo->culling.rating = ap_rating_clamp(rating);
}

void ap_photo_set_flag(ap_photo *photo, ap_flag flag)
{
    if (!photo) return;
    photo->culling.flag = flag;
}

void ap_photo_set_color_label(ap_photo *photo, ap_color_label color)
{
    if (!photo) return;
    photo->culling.color = color;
}

const ap_photo_keywords *ap_photo_get_keywords(const ap_photo *photo)
{
    return photo ? &photo->keywords : NULL;
}

bool ap_photo_keyword_add(ap_photo *photo, const char *kw)
{
    if (!photo) return false;
    return ap_photo_keywords_add(&photo->keywords, kw);
}

bool ap_photo_keyword_remove(ap_photo *photo, const char *kw)
{
    if (!photo) return false;
    return ap_photo_keywords_remove(&photo->keywords, kw);
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

void ap_photo_edit_snapshot(ap_photo *photo)
{
    if (!photo) return;
    ap_edit_history_snapshot(&photo->history, &photo->stack);
}

bool ap_photo_undo(ap_photo *photo)
{
    if (!photo) return false;
    return ap_edit_history_undo(&photo->history, &photo->stack);
}

bool ap_photo_redo(ap_photo *photo)
{
    if (!photo) return false;
    return ap_edit_history_redo(&photo->history, &photo->stack);
}

ap_edit_history *ap_photo_history(ap_photo *photo)
{
    return photo ? &photo->history : NULL;
}
