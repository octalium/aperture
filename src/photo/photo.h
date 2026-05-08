#ifndef APERTURE_PHOTO_H
#define APERTURE_PHOTO_H

#include "gpu/gpu.h"
#include "gpu/pipeline_graph.h"

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
void      ap_photo_close(ap_photo *photo);

ap_pipeline_graph *ap_photo_graph(ap_photo *photo);

// Mutable — sliders write here; the render loop reads here.
ap_edit_state *ap_photo_edit(ap_photo *photo);

int         ap_photo_width(const ap_photo *photo);
int         ap_photo_height(const ap_photo *photo);
const char *ap_photo_path(const ap_photo *photo);

// Render the photo's processed output to a JPEG file at `out_path`.
// Reads back the GPU display image, drops alpha, encodes baseline
// JPEG. Synchronous. Returns 0 on success.
int ap_photo_export_jpeg(ap_photo *photo, const char *out_path,
                         int quality);

#ifdef __cplusplus
}
#endif

#endif
