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

// CPU-only part: produce an RGBA8 thumbnail buffer for the raw at
// `path`, safe to run from any thread. Prefers the edit-render
// cache (a JPEG of the photo run through its actual edit stack,
// written by ap_thumbnail_cache_write when a photo is closed) and
// falls back to the camera's embedded preview when no fresh cache
// exists. Caller frees `*out_pixels`. Returns 0 on success.
int ap_thumbnail_decode_cpu(const char *path,
                            uint8_t **out_pixels, int *out_w, int *out_h);

// ----- edit-render thumbnail cache --------------------------------
//
// `<app_root>/thumbs/<hash-of-abspath>.jpg` — a small JPEG of the
// photo rendered through its current edit stack, so the library
// grid reflects edits instead of the camera's embedded preview.

// Build the cache path for `source_path`. Returns 0 on success.
int  ap_thumbnail_cache_path(const char *source_path,
                             char *out, size_t out_len);

// True when a cache file exists and is at least as new as the
// photo's `.aperture` sidecar — i.e. it reflects the current edits.
bool ap_thumbnail_cache_valid(const char *source_path);

// Downsample the supplied full-resolution RGBA8 buffer, JPEG-encode
// it, and write it to the cache path atomically. Returns 0 on
// success. Called on the GPU thread after a pipeline-graph readback.
int  ap_thumbnail_cache_write(const char *source_path,
                              const uint8_t *rgba, int width, int height);

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
