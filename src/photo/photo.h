#ifndef APERTURE_PHOTO_H
#define APERTURE_PHOTO_H

#include "edit/stack.h"
#include "gpu/gpu.h"
#include "gpu/pipeline_graph.h"
#include "io/raw.h"
#include "photo/metadata.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_photo ap_photo;

// Open a single source file as an editable photo. Allocates GPU
// resources (input texture, pipeline graph, display image).
//
// Lifetime: paired with ap_photo_close. Caller is responsible for
// dropping the photo from ap_gpu's current-graph pointer (via
// ap_gpu_set_graph(NULL)) before closing, and for waiting on the
// device to idle.
ap_photo *ap_photo_open(ap_gpu *g, const char *path);

// Async-friendly equivalent that takes a pre-loaded raw image and
// builds the photo around it without touching the disk in the main
// thread. The photo takes ownership of `*raw` regardless of outcome
// (succeed → consumed; fail → freed).
ap_photo *ap_photo_open_with_raw(ap_gpu *g, const char *path,
                                 ap_raw_image *raw);

void      ap_photo_close(ap_photo *photo);

ap_pipeline_graph *ap_photo_graph(ap_photo *photo);

// Per-photo edit stack. Mutable - panels mutate slot params and
// add / remove / move entries, the render loop reads it each frame.
ap_edit_stack *ap_photo_stack(ap_photo *photo);

// Whether to apply the raw's EXIF orientation. Persisted per-photo
// in the sidecar. Toggling rebuilds the graph at different dims, so
// the caller (app) reopens the photo on change.
bool ap_photo_respect_orientation(const ap_photo *photo);
void ap_photo_set_respect_orientation(ap_photo *photo, bool yes);

// Rebuild the pipeline graph from the current stack. The display
// image's view + sampler change, so the caller must rebind the
// canvas to the new outputs (ap_canvas_set_input). Returns 0 on
// success; the photo's previous graph is destroyed first.
int ap_photo_rebuild_graph(ap_photo *photo);

// Synchronous GPU readback of the rendered display image to a
// freshly malloc'd RGBA8 buffer (caller frees). Used by the
// photo-close path to snapshot pixels while the graph is alive;
// the downsample + JPEG encode + library db store happen off the
// GPU thread on a worker. Call on the GPU thread. Returns 0 on
// success.
int ap_photo_readback_rgba(ap_photo *photo,
                           uint8_t **out_rgba, int *out_w, int *out_h);

// "View Raw" mode: when on, the graph is rebuilt with an empty
// stack — every user edit is bypassed and only the raw_passthrough
// stage runs, showing the Bayer plane as grayscale. The user's
// stack data is preserved; toggling off restores the rendered
// view.
bool ap_photo_view_raw(const ap_photo *photo);
void ap_photo_set_view_raw(ap_photo *photo, bool yes);

int         ap_photo_width(const ap_photo *photo);
int         ap_photo_height(const ap_photo *photo);
const char *ap_photo_path(const ap_photo *photo);

// Per-field metadata accessors. The "effective" value is the user's
// override when set, otherwise the value the loader read from the
// file. `is_user` tells the panel whether to enable the per-field
// reset button. set_user marks the field overridden (even with an
// empty string, which means "user explicitly blanked it"); reset
// clears the override and reverts to the file value.
const char *ap_photo_metadata_value(const ap_photo *photo, ap_meta_field f);
const char *ap_photo_metadata_file_value(const ap_photo *photo,
                                         ap_meta_field f);
bool        ap_photo_metadata_is_user(const ap_photo *photo, ap_meta_field f);
void        ap_photo_metadata_set_user(ap_photo *photo, ap_meta_field f,
                                       const char *value);
void        ap_photo_metadata_reset(ap_photo *photo, ap_meta_field f);

#ifdef __cplusplus
}
#endif

#endif
