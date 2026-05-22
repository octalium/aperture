#ifndef APERTURE_PIPELINE_GRAPH_PRIV_H
#define APERTURE_PIPELINE_GRAPH_PRIV_H

#include "pipeline_graph.h"
#include "gpu_internal.h"

#include "color/icc.h"
#include "core/log.h"
#include "modules/module.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STAGES 32

typedef struct {
    const ap_module       *module;
    int                    entry_idx;
    size_t                 push_size;
    ap_module_pack_push_fn pack_push;
    VkDescriptorSetLayout  dsl;
    VkPipelineLayout       pl;
    VkPipeline             pipeline;
    VkDescriptorSet        ds;
    VkImageView            read0_view;
    VkImageView            read1_view;
    VkImageView            write_view;
    VkImage                write_image;

    VkImage                lut_image;
    VkDeviceMemory         lut_memory;
    VkImageView            lut_view;

    // Runtime skip: when true, the stage is not dispatched. For
    // single-pass stages and the last pass of multi-pass modules,
    // the runner instead copies module_in_image -> write_image to
    // preserve the ping-pong invariant. Intermediate passes of a
    // multi-pass module (is_last_pass == false) are simply skipped
    // with no copy — only the final pass's copy matters.
    bool                   skip;
    bool                   is_last_pass;   // true for single-pass and
                                           // for the last pass of a
                                           // multi-pass module
    VkImage                module_in_image; // image feeding this module's
                                            // first pass; used for the
                                            // skip passthrough copy
} graph_stage;

struct ap_pipeline_graph {
    struct ap_gpu *gpu;
    int width;
    int height;

    bool            has_meta;
    ap_raw_metadata meta;

    bool            has_recorded;
    ap_edit_stack   record_snapshot;

    VkDescriptorPool descriptor_pool;

    VkImage        stage_a_image;
    VkDeviceMemory stage_a_memory;
    VkImageView    stage_a_view;

    VkImage        stage_b_image;
    VkDeviceMemory stage_b_memory;
    VkImageView    stage_b_view;

    int            scratch_count;
    VkImage        scratch_image[AP_MODULE_MAX_SCRATCH];
    VkDeviceMemory scratch_memory[AP_MODULE_MAX_SCRATCH];
    VkImageView    scratch_view[AP_MODULE_MAX_SCRATCH];

    VkImage        display_image;
    VkDeviceMemory display_memory;
    VkImageView    display_view_unorm;
    VkImageView    display_view_srgb;
    VkImageView    display_view_uint;  // R8G8B8A8_UINT reinterpret for histogram
    VkSampler      display_sampler;

    VkImage        thumb_image;
    VkDeviceMemory thumb_memory;
    int            thumb_width;
    int            thumb_height;

    // GPU histogram pass: 4 × 256 uint bins (R, G, B, Luma).
    // The buffer is host-visible + host-coherent so the panel can read
    // it directly after vkWaitForFences without a staging copy.
    VkBuffer             hist_buffer;
    VkDeviceMemory       hist_memory;
    void                *hist_map;         // persistent map; NULL until mapped
    VkDescriptorPool     hist_pool;
    VkDescriptorSetLayout hist_dsl;
    VkPipelineLayout     hist_pl;
    VkPipeline           hist_pipeline;
    VkDescriptorSet      hist_ds;

    int          stage_count;
    graph_stage  stages[MAX_STAGES];
};

int graph_create_image(VkDevice device, VkPhysicalDevice physical,
                       int width, int height, VkFormat format,
                       VkImageUsageFlags usage,
                       VkImageCreateFlags flags,
                       const VkFormat *view_formats, uint32_t view_format_count,
                       VkImage *out_image, VkDeviceMemory *out_memory);

int graph_create_view(VkDevice device, VkImage image, VkFormat format,
                      VkImageUsageFlags view_usage_override,
                      VkImageView *out_view);

int build_stage_lut(ap_pipeline_graph *graph, graph_stage *st,
                    const ap_module_active *a,
                    const ap_edit_stack *stack);

#endif
