#include "compute.h"

#include "gpu_internal.h"

#include "core/log.h"

#include "process_comp_spv.h"
#include "tone_comp_spv.h"
#include "encode_comp_spv.h"

#include <stdlib.h>

#define WORKGROUP_SIZE 16

#define TRANSFER_SRGB 0u

typedef struct {
    float exposure_ev;
} process_push_t;

typedef struct {
    float contrast;
    float pivot;
} tone_push_t;

typedef struct {
    uint32_t transfer_function;
} encode_push_t;

struct ap_compute {
    struct ap_gpu *gpu;

    int width;
    int height;

    VkDescriptorPool descriptor_pool;

    VkDescriptorSetLayout process_dsl;
    VkPipelineLayout      process_pl;
    VkPipeline            process_pipeline;
    VkDescriptorSet       process_ds;

    VkDescriptorSetLayout tone_dsl;
    VkPipelineLayout      tone_pl;
    VkPipeline            tone_pipeline;
    VkDescriptorSet       tone_ds;

    VkDescriptorSetLayout encode_dsl;
    VkPipelineLayout      encode_pl;
    VkPipeline            encode_pipeline;
    VkDescriptorSet       encode_ds;

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
        AP_ERROR("compute: vkCreateImage failed");
        return -1;
    }

    VkMemoryRequirements mreq;
    vkGetImageMemoryRequirements(device, *out_image, &mreq);
    int mt = find_memory_type(physical, mreq.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) {
        AP_ERROR("compute: no device-local memory type");
        return -1;
    }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mreq.size,
        .memoryTypeIndex = (uint32_t)mt,
    };
    if (vkAllocateMemory(device, &mai, NULL, out_memory) != VK_SUCCESS) {
        AP_ERROR("compute: image memory allocation failed");
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
        AP_ERROR("compute: image view create failed (format %d)", format);
        return -1;
    }
    return 0;
}

static int create_pipeline_object(ap_compute *c,
                                  const uint32_t *spv, size_t spv_size,
                                  uint32_t push_size,
                                  VkDescriptorSetLayout *out_dsl,
                                  VkPipelineLayout      *out_pl,
                                  VkPipeline            *out_pipeline)
{
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
    if (vkCreateDescriptorSetLayout(c->gpu->device, &dslci, NULL, out_dsl) != VK_SUCCESS) {
        AP_ERROR("compute: descriptor set layout failed");
        return -1;
    }

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = push_size,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = out_dsl,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    if (vkCreatePipelineLayout(c->gpu->device, &plci, NULL, out_pl) != VK_SUCCESS) {
        AP_ERROR("compute: pipeline layout failed");
        return -1;
    }

    VkShaderModuleCreateInfo smci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spv_size,
        .pCode    = spv,
    };
    VkShaderModule sm;
    if (vkCreateShaderModule(c->gpu->device, &smci, NULL, &sm) != VK_SUCCESS) {
        AP_ERROR("compute: shader module failed");
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
        .layout = *out_pl,
    };
    VkResult r = vkCreateComputePipelines(c->gpu->device, VK_NULL_HANDLE,
                                          1, &cpci, NULL, out_pipeline);
    vkDestroyShaderModule(c->gpu->device, sm, NULL);
    if (r != VK_SUCCESS) {
        AP_ERROR("compute: vkCreateComputePipelines -> %d", r);
        return -1;
    }
    return 0;
}

static int allocate_descriptors(ap_compute *c)
{
    VkDescriptorPoolSize pool_size = {
        .type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 6,
    };
    VkDescriptorPoolCreateInfo pci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = 3,
        .poolSizeCount = 1,
        .pPoolSizes    = &pool_size,
    };
    if (vkCreateDescriptorPool(c->gpu->device, &pci, NULL, &c->descriptor_pool) != VK_SUCCESS) {
        AP_ERROR("compute: descriptor pool failed");
        return -1;
    }

    VkDescriptorSetLayout layouts[3] = { c->process_dsl, c->tone_dsl, c->encode_dsl };
    VkDescriptorSet       sets[3]    = { 0 };
    VkDescriptorSetAllocateInfo dai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = c->descriptor_pool,
        .descriptorSetCount = 3,
        .pSetLayouts        = layouts,
    };
    if (vkAllocateDescriptorSets(c->gpu->device, &dai, sets) != VK_SUCCESS) {
        AP_ERROR("compute: descriptor sets alloc failed");
        return -1;
    }
    c->process_ds = sets[0];
    c->tone_ds    = sets[1];
    c->encode_ds  = sets[2];
    return 0;
}

static void update_descriptors(ap_compute *c, VkImageView input_view)
{
    VkDescriptorImageInfo process_in   = { .imageView = input_view,           .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo process_out  = { .imageView = c->stage_a_view,      .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo tone_in      = { .imageView = c->stage_a_view,      .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo tone_out     = { .imageView = c->stage_b_view,      .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo encode_in    = { .imageView = c->stage_b_view,      .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo encode_out   = { .imageView = c->display_view_unorm,.imageLayout = VK_IMAGE_LAYOUT_GENERAL };

    VkWriteDescriptorSet writes[6] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->process_ds, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &process_in },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->process_ds, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &process_out },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->tone_ds,    .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &tone_in },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->tone_ds,    .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &tone_out },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->encode_ds,  .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &encode_in },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->encode_ds,  .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &encode_out },
    };
    vkUpdateDescriptorSets(c->gpu->device, 6, writes, 0, NULL);
}

static int initial_layout_transitions(ap_compute *c)
{
    VkCommandBufferAllocateInfo cba = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = c->gpu->command_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(c->gpu->device, &cba, &cmd);

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &bi);

    VkImage targets[3] = { c->stage_a_image, c->stage_b_image, c->display_image };
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
    vkQueueSubmit2(c->gpu->graphics_queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(c->gpu->graphics_queue);
    vkFreeCommandBuffers(c->gpu->device, c->gpu->command_pool, 1, &cmd);
    return 0;
}

ap_compute *ap_compute_create(ap_gpu *g, ap_texture *input)
{
    if (!g || !input) {
        AP_ERROR("ap_compute_create: null arg");
        return NULL;
    }

    ap_compute *c = calloc(1, sizeof(*c));
    if (!c) {
        AP_ERROR("ap_compute_create: out of memory");
        return NULL;
    }
    c->gpu    = g;
    c->width  = ap_texture_width(input);
    c->height = ap_texture_height(input);

    if (create_image(g->device, g->physical, c->width, c->height,
                     VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT,
                     0, NULL, 0,
                     &c->stage_a_image, &c->stage_a_memory) < 0) goto fail;
    if (create_view(g->device, c->stage_a_image, VK_FORMAT_R16G16B16A16_SFLOAT,
                    0, &c->stage_a_view) < 0) goto fail;

    if (create_image(g->device, g->physical, c->width, c->height,
                     VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT,
                     0, NULL, 0,
                     &c->stage_b_image, &c->stage_b_memory) < 0) goto fail;
    if (create_view(g->device, c->stage_b_image, VK_FORMAT_R16G16B16A16_SFLOAT,
                    0, &c->stage_b_view) < 0) goto fail;

    VkFormat display_view_formats[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB };
    if (create_image(g->device, g->physical, c->width, c->height,
                     VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                     display_view_formats, 2,
                     &c->display_image, &c->display_memory) < 0) goto fail;
    if (create_view(g->device, c->display_image, VK_FORMAT_R8G8B8A8_UNORM,
                    0, &c->display_view_unorm) < 0) goto fail;
    if (create_view(g->device, c->display_image, VK_FORMAT_R8G8B8A8_SRGB,
                    VK_IMAGE_USAGE_SAMPLED_BIT, &c->display_view_srgb) < 0) goto fail;

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
    if (vkCreateSampler(g->device, &sci, NULL, &c->display_sampler) != VK_SUCCESS) {
        AP_ERROR("compute: sampler create failed");
        goto fail;
    }

    if (create_pipeline_object(c, process_comp_spv, process_comp_spv_size,
                               sizeof(process_push_t),
                               &c->process_dsl, &c->process_pl,
                               &c->process_pipeline) < 0) goto fail;
    if (create_pipeline_object(c, tone_comp_spv, tone_comp_spv_size,
                               sizeof(tone_push_t),
                               &c->tone_dsl, &c->tone_pl,
                               &c->tone_pipeline) < 0) goto fail;
    if (create_pipeline_object(c, encode_comp_spv, encode_comp_spv_size,
                               sizeof(encode_push_t),
                               &c->encode_dsl, &c->encode_pl,
                               &c->encode_pipeline) < 0) goto fail;

    if (allocate_descriptors(c)              < 0) goto fail;
    update_descriptors(c, ap_texture_view(input));
    if (initial_layout_transitions(c)        < 0) goto fail;

    return c;

fail:
    ap_compute_destroy(c);
    return NULL;
}

void ap_compute_destroy(ap_compute *c)
{
    if (!c) return;
    VkDevice dev = c->gpu->device;

    if (c->display_sampler)    vkDestroySampler(dev, c->display_sampler, NULL);
    if (c->display_view_srgb)  vkDestroyImageView(dev, c->display_view_srgb, NULL);
    if (c->display_view_unorm) vkDestroyImageView(dev, c->display_view_unorm, NULL);
    if (c->display_image)      vkDestroyImage(dev, c->display_image, NULL);
    if (c->display_memory)     vkFreeMemory(dev, c->display_memory, NULL);

    if (c->stage_b_view)   vkDestroyImageView(dev, c->stage_b_view, NULL);
    if (c->stage_b_image)  vkDestroyImage(dev, c->stage_b_image, NULL);
    if (c->stage_b_memory) vkFreeMemory(dev, c->stage_b_memory, NULL);

    if (c->stage_a_view)   vkDestroyImageView(dev, c->stage_a_view, NULL);
    if (c->stage_a_image)  vkDestroyImage(dev, c->stage_a_image, NULL);
    if (c->stage_a_memory) vkFreeMemory(dev, c->stage_a_memory, NULL);

    if (c->descriptor_pool)  vkDestroyDescriptorPool(dev, c->descriptor_pool, NULL);

    if (c->encode_pipeline)  vkDestroyPipeline(dev, c->encode_pipeline, NULL);
    if (c->encode_pl)        vkDestroyPipelineLayout(dev, c->encode_pl, NULL);
    if (c->encode_dsl)       vkDestroyDescriptorSetLayout(dev, c->encode_dsl, NULL);

    if (c->tone_pipeline)    vkDestroyPipeline(dev, c->tone_pipeline, NULL);
    if (c->tone_pl)          vkDestroyPipelineLayout(dev, c->tone_pl, NULL);
    if (c->tone_dsl)         vkDestroyDescriptorSetLayout(dev, c->tone_dsl, NULL);

    if (c->process_pipeline) vkDestroyPipeline(dev, c->process_pipeline, NULL);
    if (c->process_pl)       vkDestroyPipelineLayout(dev, c->process_pl, NULL);
    if (c->process_dsl)      vkDestroyDescriptorSetLayout(dev, c->process_dsl, NULL);

    free(c);
}

VkImageView   ap_compute_output_view(const ap_compute *c)    { return c->display_view_srgb; }
VkSampler     ap_compute_output_sampler(const ap_compute *c) { return c->display_sampler; }
VkImageLayout ap_compute_output_layout(const ap_compute *c)  { (void)c; return VK_IMAGE_LAYOUT_GENERAL; }
int           ap_compute_output_width(const ap_compute *c)   { return c->width; }
int           ap_compute_output_height(const ap_compute *c)  { return c->height; }

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

void ap_compute_record(ap_compute *c, VkCommandBuffer cmd, const ap_edit_state *edit)
{
    uint32_t gx = (uint32_t)((c->width  + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE);
    uint32_t gy = (uint32_t)((c->height + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE);

    process_push_t process_pc = {
        .exposure_ev = edit ? edit->exposure_ev : 0.0f,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->process_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->process_pl,
                            0, 1, &c->process_ds, 0, NULL);
    vkCmdPushConstants(cmd, c->process_pl, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(process_pc), &process_pc);
    vkCmdDispatch(cmd, gx, gy, 1);

    compute_to_compute_barrier(cmd, c->stage_a_image);

    tone_push_t tone_pc = {
        .contrast = edit ? edit->tone_contrast : 1.0f,
        .pivot    = edit ? edit->tone_pivot    : 0.18f,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->tone_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->tone_pl,
                            0, 1, &c->tone_ds, 0, NULL);
    vkCmdPushConstants(cmd, c->tone_pl, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(tone_pc), &tone_pc);
    vkCmdDispatch(cmd, gx, gy, 1);

    compute_to_compute_barrier(cmd, c->stage_b_image);

    encode_push_t encode_pc = {
        .transfer_function = TRANSFER_SRGB,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->encode_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->encode_pl,
                            0, 1, &c->encode_ds, 0, NULL);
    vkCmdPushConstants(cmd, c->encode_pl, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(encode_pc), &encode_pc);
    vkCmdDispatch(cmd, gx, gy, 1);

    compute_to_sample_barrier(cmd, c->display_image);
}
