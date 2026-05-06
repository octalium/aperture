#include "compute.h"

#include "gpu_internal.h"

#include "core/log.h"

#include "exposure_comp_spv.h"

#include <stdlib.h>

#define WORKGROUP_SIZE 16

struct ap_compute {
    struct ap_gpu *gpu;

    VkDescriptorSetLayout dsl;
    VkPipelineLayout      pl;
    VkPipeline            pipeline;
    VkDescriptorPool      descriptor_pool;
    VkDescriptorSet       descriptor_set;

    VkImage         output_image;
    VkDeviceMemory  output_memory;
    VkImageView     output_view_unorm;
    VkImageView     output_view_srgb;
    VkSampler       output_sampler;
    int             width;
    int             height;
};

typedef struct {
    float exposure_ev;
} push_constants_t;

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

static int create_output_image(ap_compute *c, int width, int height)
{
    VkFormat formats[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB };
    VkImageFormatListCreateInfo fmt_list = {
        .sType           = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = 2,
        .pViewFormats    = formats,
    };

    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext         = &fmt_list,
        .flags         = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8G8B8A8_UNORM,
        .extent        = { (uint32_t)width, (uint32_t)height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(c->gpu->device, &ici, NULL, &c->output_image) != VK_SUCCESS) {
        AP_ERROR("compute: vkCreateImage failed");
        return -1;
    }

    VkMemoryRequirements mreq;
    vkGetImageMemoryRequirements(c->gpu->device, c->output_image, &mreq);
    int mt = find_memory_type(c->gpu->physical, mreq.memoryTypeBits,
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
    if (vkAllocateMemory(c->gpu->device, &mai, NULL, &c->output_memory) != VK_SUCCESS) {
        AP_ERROR("compute: image memory allocation failed");
        return -1;
    }
    vkBindImageMemory(c->gpu->device, c->output_image, c->output_memory, 0);

    VkImageViewCreateInfo vci_unorm = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = c->output_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    if (vkCreateImageView(c->gpu->device, &vci_unorm, NULL,
                          &c->output_view_unorm) != VK_SUCCESS) {
        AP_ERROR("compute: UNORM view create failed");
        return -1;
    }

    VkImageViewUsageCreateInfo srgb_usage = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    VkImageViewCreateInfo vci_srgb = vci_unorm;
    vci_srgb.pNext  = &srgb_usage;
    vci_srgb.format = VK_FORMAT_R8G8B8A8_SRGB;
    if (vkCreateImageView(c->gpu->device, &vci_srgb, NULL,
                          &c->output_view_srgb) != VK_SUCCESS) {
        AP_ERROR("compute: SRGB view create failed");
        return -1;
    }

    VkSamplerCreateInfo sci = {
        .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter               = VK_FILTER_LINEAR,
        .minFilter               = VK_FILTER_LINEAR,
        .mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    if (vkCreateSampler(c->gpu->device, &sci, NULL, &c->output_sampler) != VK_SUCCESS) {
        AP_ERROR("compute: sampler create failed");
        return -1;
    }

    return 0;
}

static int create_pipeline(ap_compute *c)
{
    VkDescriptorSetLayoutBinding bindings[2] = {
        {
            .binding         = 0,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding         = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings    = bindings,
    };
    if (vkCreateDescriptorSetLayout(c->gpu->device, &dslci, NULL, &c->dsl) != VK_SUCCESS) {
        AP_ERROR("compute: descriptor set layout create failed");
        return -1;
    }

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(push_constants_t),
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &c->dsl,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    if (vkCreatePipelineLayout(c->gpu->device, &plci, NULL, &c->pl) != VK_SUCCESS) {
        AP_ERROR("compute: pipeline layout create failed");
        return -1;
    }

    VkShaderModuleCreateInfo smci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = exposure_comp_spv_size,
        .pCode    = exposure_comp_spv,
    };
    VkShaderModule sm;
    if (vkCreateShaderModule(c->gpu->device, &smci, NULL, &sm) != VK_SUCCESS) {
        AP_ERROR("compute: shader module create failed");
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
        .layout = c->pl,
    };
    VkResult r = vkCreateComputePipelines(c->gpu->device, VK_NULL_HANDLE,
                                          1, &cpci, NULL, &c->pipeline);
    vkDestroyShaderModule(c->gpu->device, sm, NULL);
    if (r != VK_SUCCESS) {
        AP_ERROR("compute: vkCreateComputePipelines -> %d", r);
        return -1;
    }

    VkDescriptorPoolSize pool_size = {
        .type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 2,
    };
    VkDescriptorPoolCreateInfo pci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = 1,
        .poolSizeCount = 1,
        .pPoolSizes    = &pool_size,
    };
    if (vkCreateDescriptorPool(c->gpu->device, &pci, NULL, &c->descriptor_pool) != VK_SUCCESS) {
        AP_ERROR("compute: descriptor pool create failed");
        return -1;
    }

    VkDescriptorSetAllocateInfo dai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = c->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &c->dsl,
    };
    if (vkAllocateDescriptorSets(c->gpu->device, &dai, &c->descriptor_set) != VK_SUCCESS) {
        AP_ERROR("compute: descriptor set alloc failed");
        return -1;
    }
    return 0;
}

static void update_descriptors(ap_compute *c, VkImageView input_view)
{
    VkDescriptorImageInfo input_info = {
        .imageView   = input_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkDescriptorImageInfo output_info = {
        .imageView   = c->output_view_unorm,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkWriteDescriptorSet writes[2] = {
        {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = c->descriptor_set,
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo      = &input_info,
        },
        {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = c->descriptor_set,
            .dstBinding      = 1,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo      = &output_info,
        },
    };
    vkUpdateDescriptorSets(c->gpu->device, 2, writes, 0, NULL);
}

static int initial_layout_transition(ap_compute *c)
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

    VkImageMemoryBarrier2 b = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = c->output_image,
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

    if (create_output_image(c, c->width, c->height) < 0) goto fail;
    if (create_pipeline(c)                          < 0) goto fail;
    update_descriptors(c, ap_texture_view(input));
    if (initial_layout_transition(c)                < 0) goto fail;

    return c;

fail:
    ap_compute_destroy(c);
    return NULL;
}

void ap_compute_destroy(ap_compute *c)
{
    if (!c) return;
    VkDevice dev = c->gpu->device;
    if (c->output_sampler)    vkDestroySampler(dev, c->output_sampler, NULL);
    if (c->output_view_srgb)  vkDestroyImageView(dev, c->output_view_srgb, NULL);
    if (c->output_view_unorm) vkDestroyImageView(dev, c->output_view_unorm, NULL);
    if (c->output_image)      vkDestroyImage(dev, c->output_image, NULL);
    if (c->output_memory)     vkFreeMemory(dev, c->output_memory, NULL);
    if (c->descriptor_pool)   vkDestroyDescriptorPool(dev, c->descriptor_pool, NULL);
    if (c->pipeline)          vkDestroyPipeline(dev, c->pipeline, NULL);
    if (c->pl)                vkDestroyPipelineLayout(dev, c->pl, NULL);
    if (c->dsl)               vkDestroyDescriptorSetLayout(dev, c->dsl, NULL);
    free(c);
}

VkImageView   ap_compute_output_view(const ap_compute *c)    { return c->output_view_srgb; }
VkSampler     ap_compute_output_sampler(const ap_compute *c) { return c->output_sampler; }
VkImageLayout ap_compute_output_layout(const ap_compute *c)  { (void)c; return VK_IMAGE_LAYOUT_GENERAL; }
int           ap_compute_output_width(const ap_compute *c)   { return c->width; }
int           ap_compute_output_height(const ap_compute *c)  { return c->height; }

void ap_compute_record(ap_compute *c, VkCommandBuffer cmd, const ap_edit_state *edit)
{
    push_constants_t pc = {
        .exposure_ev = edit ? edit->exposure_ev : 0.0f,
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->pl,
                            0, 1, &c->descriptor_set, 0, NULL);
    vkCmdPushConstants(cmd, c->pl, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (uint32_t)((c->width  + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE);
    uint32_t gy = (uint32_t)((c->height + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE);
    vkCmdDispatch(cmd, gx, gy, 1);

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
        .image = c->output_image,
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
