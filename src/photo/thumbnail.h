#ifndef APERTURE_PHOTO_THUMBNAIL_H
#define APERTURE_PHOTO_THUMBNAIL_H

#include "gpu/gpu.h"

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_thumbnail ap_thumbnail;

// CPU-only part: open the raw, extract the embedded preview, decode
// to a freshly malloc'd RGBA8 buffer. Safe to run from any thread -
// no GPU access. Caller frees `*out_pixels`. Returns 0 on success.
int ap_thumbnail_decode_cpu(const char *path,
                            uint8_t **out_pixels, int *out_w, int *out_h);

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
