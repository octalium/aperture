#include "texture.h"

#include "gpu_internal.h"

#include "core/log.h"

#include <stdlib.h>
#include <string.h>

struct ap_texture {
    struct ap_gpu *gpu;
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    VkSampler      sampler;
    int            width;
    int            height;
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

static void transition(VkCommandBuffer cmd, VkImage image,
                       VkImageLayout old_layout, VkImageLayout new_layout,
                       VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                       VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
    VkImageMemoryBarrier2 b = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask  = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout     = old_layout,
        .newLayout     = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0, .levelCount     = 1,
            .baseArrayLayer = 0, .layerCount     = 1,
        },
    };
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &b,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

ap_texture *ap_texture_create_rgba8(ap_gpu *g, const uint8_t *pixels,
                                    int width, int height)
{
    if (width <= 0 || height <= 0 || !pixels) {
        AP_ERROR("ap_texture_create_rgba8: invalid args (%dx%d, pixels=%p)",
                 width, height, (void *)pixels);
        return NULL;
    }

    ap_texture *t = calloc(1, sizeof(*t));
    if (!t) {
        AP_ERROR("ap_texture_create_rgba8: out of memory");
        return NULL;
    }
    t->gpu    = g;
    t->width  = width;
    t->height = height;

    VkDeviceSize buffer_size = (VkDeviceSize)width * (VkDeviceSize)height * 4;

    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = buffer_size,
        .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer       staging        = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    if (vkCreateBuffer(g->device, &bci, NULL, &staging) != VK_SUCCESS) {
        AP_ERROR("staging buffer create failed");
        goto fail;
    }

    VkMemoryRequirements bmreq;
    vkGetBufferMemoryRequirements(g->device, staging, &bmreq);
    int staging_type = find_memory_type(g->physical, bmreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (staging_type < 0) {
        AP_ERROR("no host-visible coherent memory type available");
        goto fail;
    }
    VkMemoryAllocateInfo bmai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = bmreq.size,
        .memoryTypeIndex = (uint32_t)staging_type,
    };
    if (vkAllocateMemory(g->device, &bmai, NULL, &staging_memory) != VK_SUCCESS) {
        AP_ERROR("staging memory allocation failed");
        goto fail;
    }
    vkBindBufferMemory(g->device, staging, staging_memory, 0);

    void *map = NULL;
    vkMapMemory(g->device, staging_memory, 0, buffer_size, 0, &map);
    memcpy(map, pixels, (size_t)buffer_size);
    vkUnmapMemory(g->device, staging_memory);

    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8G8B8A8_SRGB,
        .extent        = { (uint32_t)width, (uint32_t)height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(g->device, &ici, NULL, &t->image) != VK_SUCCESS) {
        AP_ERROR("vkCreateImage failed");
        goto fail;
    }

    VkMemoryRequirements imreq;
    vkGetImageMemoryRequirements(g->device, t->image, &imreq);
    int image_type = find_memory_type(g->physical, imreq.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (image_type < 0) {
        AP_ERROR("no device-local memory type available");
        goto fail;
    }
    VkMemoryAllocateInfo imai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = imreq.size,
        .memoryTypeIndex = (uint32_t)image_type,
    };
    if (vkAllocateMemory(g->device, &imai, NULL, &t->memory) != VK_SUCCESS) {
        AP_ERROR("image memory allocation failed");
        goto fail;
    }
    vkBindImageMemory(g->device, t->image, t->memory, 0);

    VkCommandBufferAllocateInfo cba = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = g->command_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(g->device, &cba, &cmd);

    VkCommandBufferBeginInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &cbi);

    transition(cmd, t->image,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region = {
        .imageSubresource = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel       = 0,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
        .imageExtent = { (uint32_t)width, (uint32_t)height, 1 },
    };
    vkCmdCopyBufferToImage(cmd, staging, t->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transition(cmd, t->image,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

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
    vkQueueSubmit2(g->graphics_queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(g->graphics_queue);

    vkFreeCommandBuffers(g->device, g->command_pool, 1, &cmd);
    vkDestroyBuffer(g->device, staging, NULL);
    vkFreeMemory(g->device, staging_memory, NULL);
    staging        = VK_NULL_HANDLE;
    staging_memory = VK_NULL_HANDLE;

    VkImageViewCreateInfo vci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = t->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = VK_FORMAT_R8G8B8A8_SRGB,
        .components = {
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    if (vkCreateImageView(g->device, &vci, NULL, &t->view) != VK_SUCCESS) {
        AP_ERROR("vkCreateImageView failed");
        goto fail;
    }

    VkSamplerCreateInfo sci = {
        .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter               = VK_FILTER_LINEAR,
        .minFilter               = VK_FILTER_LINEAR,
        .mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .anisotropyEnable        = VK_FALSE,
        .borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
        .compareEnable           = VK_FALSE,
    };
    if (vkCreateSampler(g->device, &sci, NULL, &t->sampler) != VK_SUCCESS) {
        AP_ERROR("vkCreateSampler failed");
        goto fail;
    }

    return t;

fail:
    if (staging_memory) vkFreeMemory(g->device, staging_memory, NULL);
    if (staging)        vkDestroyBuffer(g->device, staging, NULL);
    ap_texture_destroy(t);
    return NULL;
}

void ap_texture_destroy(ap_texture *t)
{
    if (!t) return;
    VkDevice dev = t->gpu->device;
    if (t->sampler) vkDestroySampler(dev, t->sampler, NULL);
    if (t->view)    vkDestroyImageView(dev, t->view, NULL);
    if (t->image)   vkDestroyImage(dev, t->image, NULL);
    if (t->memory)  vkFreeMemory(dev, t->memory, NULL);
    free(t);
}

VkImageView   ap_texture_view(const ap_texture *t)    { return t->view; }
VkSampler     ap_texture_sampler(const ap_texture *t) { return t->sampler; }
VkImageLayout ap_texture_layout(const ap_texture *t)  { (void)t; return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
int           ap_texture_width(const ap_texture *t)   { return t->width; }
int           ap_texture_height(const ap_texture *t)  { return t->height; }
