#ifndef APERTURE_PHOTO_THUMBNAIL_H
#define APERTURE_PHOTO_THUMBNAIL_H

#include "gpu/gpu.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_thumbnail ap_thumbnail;

// CPU-only part: decode the camera's embedded preview for the raw at
// `path` into a freshly malloc'd RGBA8 buffer, downsampled + EXIF-
// oriented. Safe to run from any thread. This is the *fallback*
// thumbnail source; the edit-render JPEG from the library db is
// preferred (see ap_thumbnail_decode_jpeg). Caller frees
// `*out_pixels`. Returns 0 on success.
int ap_thumbnail_decode_cpu(const char *path,
                            uint8_t **out_pixels, int *out_w, int *out_h);

// CPU-only part: decode an in-memory JPEG (the edit-render blob
// stored in the library db) into a freshly malloc'd RGBA8 buffer,
// downsampled to the thumbnail target size. The blob was rendered
// from the upright pipeline output so no EXIF rotation is applied.
// Caller frees `*out_pixels`. Returns 0 on success.
int ap_thumbnail_decode_jpeg(const uint8_t *jpeg, size_t jpeg_size,
                             uint8_t **out_pixels, int *out_w, int *out_h);

// Downsample a full-resolution RGBA8 buffer to the thumbnail target
// size and JPEG-encode it into a freshly malloc'd buffer. Used on
// the GPU thread after a pipeline-graph readback to produce the
// edit-render blob stored in the library db. Caller frees
// `*out_jpeg`. Returns 0 on success.
int ap_thumbnail_encode_jpeg(const uint8_t *rgba, int width, int height,
                             uint8_t **out_jpeg, size_t *out_size);

// GPU-only part: upload an RGBA8 buffer (sRGB-encoded - bytes from
// decode_cpu are already that) as a SAMPLED-only texture. Must run
// on the GPU thread.
ap_thumbnail *ap_thumbnail_upload(ap_gpu *g, const uint8_t *rgba,
                                  int width, int height);

// Convenience: synchronous decode + upload. Equivalent to
// decode_cpu + upload back-to-back. GPU thread only.
ap_thumbnail *ap_thumbnail_create(ap_gpu *g, const char *path);
void          ap_thumbnail_destroy(ap_thumbnail *t);

VkImageView ap_thumbnail_view(const ap_thumbnail *t);
VkSampler   ap_thumbnail_sampler(const ap_thumbnail *t);
int         ap_thumbnail_width(const ap_thumbnail *t);
int         ap_thumbnail_height(const ap_thumbnail *t);

#ifdef __cplusplus
}
#endif

#endif
