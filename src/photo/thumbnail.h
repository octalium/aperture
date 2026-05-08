#ifndef APERTURE_PHOTO_THUMBNAIL_H
#define APERTURE_PHOTO_THUMBNAIL_H

#include "gpu/gpu.h"

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_thumbnail ap_thumbnail;

// Decode the embedded preview JPEG of the raw at `path`, upload as a
// SAMPLED-only RGBA8 SRGB texture, and return it. Synchronous —
// caller must invoke on the GPU thread. Returns NULL on failure
// (file unreadable, no embedded preview, decode failure).
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
