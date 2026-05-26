#ifndef APERTURE_GPU_TEXTURE_H
#define APERTURE_GPU_TEXTURE_H

#include <stdint.h>

#include <vulkan/vulkan.h>

#include "gpu/gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_texture ap_texture;

ap_texture *ap_texture_create_rgba8(ap_gpu *g, const uint8_t *pixels,
                                    int width, int height);

// SRGB-encoded RGBA8, sampled-only (no STORAGE). Use this for input
// images that should round-trip through the swapchain's color
// management (thumbnails, embedded previews).
ap_texture *ap_texture_create_rgba8_srgb(ap_gpu *g, const uint8_t *pixels,
                                         int width, int height);

ap_texture *ap_texture_create_r16(ap_gpu *g, const uint16_t *pixels,
                                  int width, int height);
void ap_texture_destroy(ap_texture *t);

VkImageView   ap_texture_view(const ap_texture *t);
VkSampler     ap_texture_sampler(const ap_texture *t);
VkImageLayout ap_texture_layout(const ap_texture *t);
int           ap_texture_width(const ap_texture *t);
int           ap_texture_height(const ap_texture *t);

#ifdef __cplusplus
}
#endif

#endif
