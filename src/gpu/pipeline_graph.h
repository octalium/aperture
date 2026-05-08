#ifndef APERTURE_GPU_PIPELINE_GRAPH_H
#define APERTURE_GPU_PIPELINE_GRAPH_H

#include <stdint.h>

#include <vulkan/vulkan.h>

#include "gpu/gpu.h"
#include "gpu/texture.h"
#include "io/raw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_pipeline_graph ap_pipeline_graph;

// Forward decl from modules/module.h to avoid pulling the header from a
// gpu-layer file. Callers include modules/module.h to get the full type.
typedef struct ap_module ap_module;

// Construct a graph wired to the given input texture, dispatching the
// supplied modules in order. Allocates the float16 ping-pong working
// buffers and the UNORM display image at `output_width × output_height`,
// which may differ from the input texture's dimensions (e.g. when the
// first stage applies an EXIF orientation rotation). Dispatch dims are
// always the output dims; module shaders that need the input dims are
// expected to pull them from the metadata. Wires each module's
// descriptor set by ping-pong position: first reads input, last writes
// display, middle modules alternate between the two working buffers.
//
// `meta` is per-image static metadata (camera color matrix, black
// levels, sensor dims, flip code, etc.) — copied into the graph at
// create time and passed to each module's pack_push. May be NULL for
// chains that don't need it.
ap_pipeline_graph *ap_pipeline_graph_create(ap_gpu *g,
                                            ap_texture *input,
                                            int output_width,
                                            int output_height,
                                            const ap_module *const *modules,
                                            int module_count,
                                            const ap_raw_metadata *meta);
void ap_pipeline_graph_destroy(ap_pipeline_graph *graph);

// Records the full chain for one frame. Each module's pack_push runs
// against `edit`, then a dispatch + a barrier.
int ap_pipeline_graph_record(ap_pipeline_graph *graph, VkCommandBuffer cmd,
                             const ap_edit_state *edit);

// The display image (final output) — for sampling via ImGui or canvas.
VkImageView   ap_pipeline_graph_output_view(const ap_pipeline_graph *graph);
VkSampler     ap_pipeline_graph_output_sampler(const ap_pipeline_graph *graph);
VkImageLayout ap_pipeline_graph_output_layout(const ap_pipeline_graph *graph);
int           ap_pipeline_graph_output_width(const ap_pipeline_graph *graph);
int           ap_pipeline_graph_output_height(const ap_pipeline_graph *graph);

#ifdef __cplusplus
}
#endif

#endif
