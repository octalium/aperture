#ifndef APERTURE_GPU_COMPUTE_H
#define APERTURE_GPU_COMPUTE_H

#include <vulkan/vulkan.h>

#include "gpu/gpu.h"
#include "gpu/texture.h"
#include "io/raw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_compute ap_compute;

ap_compute *ap_compute_create(ap_gpu *g, ap_texture *input,
                              const ap_raw_metadata *meta);
void        ap_compute_destroy(ap_compute *c);

VkImageView   ap_compute_output_view(const ap_compute *c);
VkSampler     ap_compute_output_sampler(const ap_compute *c);
VkImageLayout ap_compute_output_layout(const ap_compute *c);
int           ap_compute_output_width(const ap_compute *c);
int           ap_compute_output_height(const ap_compute *c);

void ap_compute_record(ap_compute *c, VkCommandBuffer cmd,
                       const ap_edit_state *edit);

#ifdef __cplusplus
}
#endif

#endif
