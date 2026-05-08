#include "pipeline_graph.h"

#include "gpu_internal.h"

#include "core/log.h"
#include "modules/module.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define WORKGROUP_SIZE 16
#define MAX_MODULES    16

typedef struct {
    const ap_module      *module;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout      pl;
    VkPipeline            pipeline;
    VkDescriptorSet       ds;
    VkImage               output_image; // for the post-dispatch barrier target
} graph_stage;

struct ap_pipeline_graph {
    struct ap_gpu *gpu;
    int width;
    int height;

    bool            has_meta;
    ap_raw_metadata meta;

    VkDescriptorPool descriptor_pool;

    VkImage        stage_a_image;
    VkDeviceMemory stage_a_memory;
    VkImageView    stage_a_view;

    VkImage        stage_b_image;
    VkDeviceMemory stage_b_memory;
    VkImageView    stage_b_view;

    VkImage        display_image;
    VkDeviceMemory display_memory;
    VkImageView    display_view_unorm;
    VkImageView    display_view_srgb;
    VkSampler      display_sampler;

    int          stage_count;
    graph_stage  stages[MAX_MODULES];
};

static int find_memory_type(VkPhysicalDevice phys, uint32_t type_bits,
                            VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) {
            return (int)i;
        }
    }
    return -1;
}

static int create_image(VkDevice device, VkPhysicalDevice physical,
                        int width, int height, VkFormat format,
                        VkImageUsageFlags usage,
                        VkImageCreateFlags flags,
                        const VkFormat *view_formats, uint32_t view_format_count,
                        VkImage *out_image, VkDeviceMemory *out_memory)
{
    VkImageFormatListCreateInfo fmt_list = {
        .sType           = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = view_format_count,
        .pViewFormats    = view_formats,
    };

    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext         = view_format_count > 0 ? &fmt_list : NULL,
        .flags         = flags,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = { (uint32_t)width, (uint32_t)height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = usage,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(device, &ici, NULL, out_image) != VK_SUCCESS) {
        AP_ERROR("graph: vkCreateImage failed");
        return -1;
    }

    VkMemoryRequirements mreq;
    vkGetImageMemoryRequirements(device, *out_image, &mreq);
    int mt = find_memory_type(physical, mreq.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) {
        AP_ERROR("graph: no device-local memory type");
        return -1;
    }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mreq.size,
        .memoryTypeIndex = (uint32_t)mt,
    };
    if (vkAllocateMemory(device, &mai, NULL, out_memory) != VK_SUCCESS) {
        AP_ERROR("graph: image memory alloc failed");
        return -1;
    }
    vkBindImageMemory(device, *out_image, *out_memory, 0);
    return 0;
}

static int create_view(VkDevice device, VkImage image, VkFormat format,
                       VkImageUsageFlags view_usage_override,
                       VkImageView *out_view)
{
    VkImageViewUsageCreateInfo usage_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = view_usage_override,
    };

    VkImageViewCreateInfo vci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext    = view_usage_override ? &usage_ci : NULL,
        .image    = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = format,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    if (vkCreateImageView(device, &vci, NULL, out_view) != VK_SUCCESS) {
        AP_ERROR("graph: image view create failed (format %d)", format);
        return -1;
    }
    return 0;
}

static int create_buffers(ap_pipeline_graph *graph, int width, int height)
{
    if (create_image(graph->gpu->device, graph->gpu->physical, width, height,
                     VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT,
                     0, NULL, 0,
                     &graph->stage_a_image, &graph->stage_a_memory) < 0) return -1;
    if (create_view(graph->gpu->device, graph->stage_a_image,
                    VK_FORMAT_R16G16B16A16_SFLOAT, 0,
                    &graph->stage_a_view) < 0) return -1;

    if (create_image(graph->gpu->device, graph->gpu->physical, width, height,
                     VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT,
                     0, NULL, 0,
                     &graph->stage_b_image, &graph->stage_b_memory) < 0) return -1;
    if (create_view(graph->gpu->device, graph->stage_b_image,
                    VK_FORMAT_R16G16B16A16_SFLOAT, 0,
                    &graph->stage_b_view) < 0) return -1;

    VkFormat display_view_formats[2] = {
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
    };
    if (create_image(graph->gpu->device, graph->gpu->physical, width, height,
                     VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                     display_view_formats, 2,
                     &graph->display_image, &graph->display_memory) < 0) return -1;
    if (create_view(graph->gpu->device, graph->display_image,
                    VK_FORMAT_R8G8B8A8_UNORM, 0,
                    &graph->display_view_unorm) < 0) return -1;
    if (create_view(graph->gpu->device, graph->display_image,
                    VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT,
                    &graph->display_view_srgb) < 0) return -1;

    VkSamplerCreateInfo sci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
    };
    if (vkCreateSampler(graph->gpu->device, &sci, NULL, &graph->display_sampler) != VK_SUCCESS) {
        AP_ERROR("graph: sampler create failed");
        return -1;
    }
    return 0;
}

static int create_descriptor_pool(ap_pipeline_graph *graph, int module_count)
{
    VkDescriptorPoolSize pool_size = {
        .type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = (uint32_t)(module_count * 2),
    };
    VkDescriptorPoolCreateInfo pci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = (uint32_t)module_count,
        .poolSizeCount = 1,
        .pPoolSizes    = &pool_size,
    };
    if (vkCreateDescriptorPool(graph->gpu->device, &pci, NULL,
                               &graph->descriptor_pool) != VK_SUCCESS) {
        AP_ERROR("graph: descriptor pool create failed");
        return -1;
    }
    return 0;
}

static int create_stage(ap_pipeline_graph *graph, graph_stage *st,
                        const ap_module *module)
{
    st->module = module;

    VkDescriptorSetLayoutBinding bindings[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings    = bindings,
    };
    if (vkCreateDescriptorSetLayout(graph->gpu->device, &dslci, NULL, &st->dsl) != VK_SUCCESS) {
        AP_ERROR("graph: %s: descriptor set layout failed", module->name);
        return -1;
    }

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = (uint32_t)module->push_size,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &st->dsl,
        .pushConstantRangeCount = module->push_size > 0 ? 1u : 0u,
        .pPushConstantRanges    = module->push_size > 0 ? &push : NULL,
    };
    if (vkCreatePipelineLayout(graph->gpu->device, &plci, NULL, &st->pl) != VK_SUCCESS) {
        AP_ERROR("graph: %s: pipeline layout failed", module->name);
        return -1;
    }

    VkShaderModuleCreateInfo smci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = module->spv_size,
        .pCode    = module->spv_data,
    };
    VkShaderModule sm;
    if (vkCreateShaderModule(graph->gpu->device, &smci, NULL, &sm) != VK_SUCCESS) {
        AP_ERROR("graph: %s: shader module failed", module->name);
        return -1;
    }

    VkComputePipelineCreateInfo cpci = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = sm,
            .pName  = "main",
        },
        .layout = st->pl,
    };
    VkResult r = vkCreateComputePipelines(graph->gpu->device, VK_NULL_HANDLE,
                                          1, &cpci, NULL, &st->pipeline);
    vkDestroyShaderModule(graph->gpu->device, sm, NULL);
    if (r != VK_SUCCESS) {
        AP_ERROR("graph: %s: vkCreateComputePipelines -> %d", module->name, r);
        return -1;
    }

    VkDescriptorSetAllocateInfo dai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = graph->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &st->dsl,
    };
    if (vkAllocateDescriptorSets(graph->gpu->device, &dai, &st->ds) != VK_SUCCESS) {
        AP_ERROR("graph: %s: descriptor set alloc failed", module->name);
        return -1;
    }
    return 0;
}

static void wire_descriptors(ap_pipeline_graph *graph, graph_stage *st,
                             VkImageView in_view, VkImageView out_view)
{
    VkDescriptorImageInfo in_info  = { .imageView = in_view,  .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo out_info = { .imageView = out_view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet writes[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = st->ds, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo = &in_info },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = st->ds, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo = &out_info },
    };
    vkUpdateDescriptorSets(graph->gpu->device, 2, writes, 0, NULL);
}

static void wire_chain(ap_pipeline_graph *graph, VkImageView input_view)
{
    VkImageView prev_out = VK_NULL_HANDLE;
    for (int i = 0; i < graph->stage_count; i++) {
        VkImageView in_view  = (i == 0) ? input_view : prev_out;
        VkImageView out_view;
        VkImage     out_image;

        bool is_last = (i == graph->stage_count - 1);
        if (is_last) {
            out_view  = graph->display_view_unorm;
            out_image = graph->display_image;
        } else {
            // Pick A or B such that we don't read+write the same buffer.
            if (i == 0 || prev_out == graph->stage_b_view) {
                out_view  = graph->stage_a_view;
                out_image = graph->stage_a_image;
            } else {
                out_view  = graph->stage_b_view;
                out_image = graph->stage_b_image;
            }
        }

        wire_descriptors(graph, &graph->stages[i], in_view, out_view);
        graph->stages[i].output_image = out_image;
        prev_out = out_view;
    }
}

static int initial_layout_transitions(ap_pipeline_graph *graph)
{
    VkCommandBufferAllocateInfo cba = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = graph->gpu->command_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(graph->gpu->device, &cba, &cmd);

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &bi);

    VkImage targets[3] = { graph->stage_a_image, graph->stage_b_image, graph->display_image };
    VkImageMemoryBarrier2 barriers[3];
    for (int i = 0; i < 3; i++) {
        barriers[i] = (VkImageMemoryBarrier2){
            .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .srcAccessMask = 0,
            .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = targets[i],
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        };
    }
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 3,
        .pImageMemoryBarriers    = barriers,
    };
    vkCmdPipelineBarrier2(cmd, &dep);

    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmd_si = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSubmitInfo2 submit = {
        .sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos    = &cmd_si,
    };
    vkQueueSubmit2(graph->gpu->graphics_queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(graph->gpu->graphics_queue);
    vkFreeCommandBuffers(graph->gpu->device, graph->gpu->command_pool, 1, &cmd);
    return 0;
}

ap_pipeline_graph *ap_pipeline_graph_create(ap_gpu *g, ap_texture *input,
                                            int output_width,
                                            int output_height,
                                            const ap_module *const *modules,
                                            int module_count,
                                            const ap_raw_metadata *meta)
{
    if (!g || !input || !modules || module_count <= 0
        || output_width <= 0 || output_height <= 0) {
        AP_ERROR("ap_pipeline_graph_create: invalid args");
        return NULL;
    }
    if (module_count > MAX_MODULES) {
        AP_ERROR("ap_pipeline_graph_create: %d modules exceeds MAX_MODULES (%d)",
                 module_count, MAX_MODULES);
        return NULL;
    }
    for (int i = 0; i < module_count; i++) {
        if (!modules[i]) {
            AP_ERROR("ap_pipeline_graph_create: module[%d] is NULL", i);
            return NULL;
        }
    }

    ap_pipeline_graph *graph = calloc(1, sizeof(*graph));
    if (!graph) {
        AP_ERROR("ap_pipeline_graph_create: out of memory");
        return NULL;
    }
    graph->gpu    = g;
    graph->width  = output_width;
    graph->height = output_height;
    graph->stage_count = module_count;
    if (meta) {
        graph->meta     = *meta;
        graph->has_meta = true;
    }

    if (create_buffers(graph, graph->width, graph->height)        < 0) goto fail;
    if (create_descriptor_pool(graph, module_count)               < 0) goto fail;

    for (int i = 0; i < module_count; i++) {
        if (create_stage(graph, &graph->stages[i], modules[i])    < 0) goto fail;
    }
    wire_chain(graph, ap_texture_view(input));

    if (initial_layout_transitions(graph)                         < 0) goto fail;

    return graph;

fail:
    ap_pipeline_graph_destroy(graph);
    return NULL;
}

void ap_pipeline_graph_destroy(ap_pipeline_graph *graph)
{
    if (!graph) return;
    VkDevice dev = graph->gpu->device;

    if (graph->display_sampler)    vkDestroySampler(dev, graph->display_sampler, NULL);
    if (graph->display_view_srgb)  vkDestroyImageView(dev, graph->display_view_srgb, NULL);
    if (graph->display_view_unorm) vkDestroyImageView(dev, graph->display_view_unorm, NULL);
    if (graph->display_image)      vkDestroyImage(dev, graph->display_image, NULL);
    if (graph->display_memory)     vkFreeMemory(dev, graph->display_memory, NULL);

    if (graph->stage_b_view)   vkDestroyImageView(dev, graph->stage_b_view, NULL);
    if (graph->stage_b_image)  vkDestroyImage(dev, graph->stage_b_image, NULL);
    if (graph->stage_b_memory) vkFreeMemory(dev, graph->stage_b_memory, NULL);

    if (graph->stage_a_view)   vkDestroyImageView(dev, graph->stage_a_view, NULL);
    if (graph->stage_a_image)  vkDestroyImage(dev, graph->stage_a_image, NULL);
    if (graph->stage_a_memory) vkFreeMemory(dev, graph->stage_a_memory, NULL);

    for (int i = 0; i < graph->stage_count; i++) {
        graph_stage *st = &graph->stages[i];
        if (st->pipeline) vkDestroyPipeline(dev, st->pipeline, NULL);
        if (st->pl)       vkDestroyPipelineLayout(dev, st->pl, NULL);
        if (st->dsl)      vkDestroyDescriptorSetLayout(dev, st->dsl, NULL);
    }

    if (graph->descriptor_pool) vkDestroyDescriptorPool(dev, graph->descriptor_pool, NULL);

    free(graph);
}

static void compute_to_compute_barrier(VkCommandBuffer cmd, VkImage image)
{
    VkImageMemoryBarrier2 b = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &b,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

static void compute_to_sample_barrier(VkCommandBuffer cmd, VkImage image)
{
    VkImageMemoryBarrier2 b = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &b,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

int ap_pipeline_graph_record(ap_pipeline_graph *graph, VkCommandBuffer cmd,
                             const ap_edit_state *edit)
{
    if (!graph || graph->stage_count == 0) {
        return 0;
    }

    uint32_t gx = (uint32_t)((graph->width  + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE);
    uint32_t gy = (uint32_t)((graph->height + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE);

    // Push-constant scratch buffer; modules pack into here. 256 bytes is
    // the conservative Vulkan-spec minimum for max push constant size.
    uint8_t push_buf[256];

    for (int i = 0; i < graph->stage_count; i++) {
        graph_stage *st = &graph->stages[i];
        const ap_module *m = st->module;

        if (m->push_size > sizeof(push_buf)) {
            AP_ERROR("graph: %s push_size %zu exceeds scratch %zu",
                     m->name, m->push_size, sizeof(push_buf));
            return -1;
        }

        if (m->pack_push) {
            const ap_raw_metadata *meta = graph->has_meta ? &graph->meta : NULL;
            int rc = m->pack_push(m, edit, meta, push_buf);
            if (rc != 0) {
                continue; // module signaled "skip"
            }
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, st->pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, st->pl,
                                0, 1, &st->ds, 0, NULL);
        if (m->push_size > 0) {
            vkCmdPushConstants(cmd, st->pl, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, (uint32_t)m->push_size, push_buf);
        }
        vkCmdDispatch(cmd, gx, gy, 1);

        bool is_last = (i == graph->stage_count - 1);
        if (is_last) {
            compute_to_sample_barrier(cmd, st->output_image);
        } else {
            compute_to_compute_barrier(cmd, st->output_image);
        }
    }
    return 0;
}

VkImageView   ap_pipeline_graph_output_view(const ap_pipeline_graph *g)    { return g->display_view_srgb; }
VkSampler     ap_pipeline_graph_output_sampler(const ap_pipeline_graph *g) { return g->display_sampler; }
VkImageLayout ap_pipeline_graph_output_layout(const ap_pipeline_graph *g)  { (void)g; return VK_IMAGE_LAYOUT_GENERAL; }
int           ap_pipeline_graph_output_width(const ap_pipeline_graph *g)   { return g->width; }
int           ap_pipeline_graph_output_height(const ap_pipeline_graph *g)  { return g->height; }
