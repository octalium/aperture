#include "pipeline_graph_priv.h"

// Identity LUT — out = in. Used when a stage declares bake_lut but the
// module's bake fails, so the stage degrades to a pass-through.
static void fill_identity_lut(float *lut)
{
    const int dim = AP_ICC_LUT_DIM;
    int idx = 0;
    for (int b = 0; b < dim; b++) {
        for (int g = 0; g < dim; g++) {
            for (int r = 0; r < dim; r++) {
                lut[idx * 4 + 0] = (float)r / (float)(dim - 1);
                lut[idx * 4 + 1] = (float)g / (float)(dim - 1);
                lut[idx * 4 + 2] = (float)b / (float)(dim - 1);
                lut[idx * 4 + 3] = 1.0f;
                idx++;
            }
        }
    }
}

// Create the stage's LUT image — a DIM x DIM*DIM RGBA32F image, the
// row-major form of the DIM^3 colour grid — and upload `data` into it,
// leaving it in GENERAL layout for the shader to imageLoad.
static int create_lut_image(ap_pipeline_graph *graph, graph_stage *st,
                            const float *data)
{
    VkDevice dev = graph->gpu->device;
    const int       w     = AP_ICC_LUT_DIM;
    const int       h     = AP_ICC_LUT_DIM * AP_ICC_LUT_DIM;
    const VkDeviceSize bytes =
        (VkDeviceSize)w * (VkDeviceSize)h * 4u * sizeof(float);

    if (graph_create_image(dev, graph->gpu->physical, w, h,
                           VK_FORMAT_R32G32B32A32_SFLOAT,
                           VK_IMAGE_USAGE_STORAGE_BIT
                             | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           0, NULL, 0,
                           &st->lut_image, &st->lut_memory) < 0) {
        return -1;
    }
    if (graph_create_view(dev, st->lut_image, VK_FORMAT_R32G32B32A32_SFLOAT,
                          0, &st->lut_view) < 0) {
        return -1;
    }

    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = bytes,
        .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer       staging     = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    int rc = -1;

    if (vkCreateBuffer(dev, &bci, NULL, &staging) != VK_SUCCESS) {
        AP_ERROR("graph: LUT staging buffer create failed");
        return -1;
    }
    VkMemoryRequirements mreq;
    vkGetBufferMemoryRequirements(dev, staging, &mreq);
    int mt = gpu_find_memory_type(graph->gpu->physical, mreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) {
        AP_ERROR("graph: no host-visible memory for LUT staging");
        goto out;
    }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mreq.size,
        .memoryTypeIndex = (uint32_t)mt,
    };
    if (vkAllocateMemory(dev, &mai, NULL, &staging_mem) != VK_SUCCESS) {
        AP_ERROR("graph: LUT staging memory alloc failed");
        goto out;
    }
    vkBindBufferMemory(dev, staging, staging_mem, 0);

    void *map = NULL;
    if (vkMapMemory(dev, staging_mem, 0, bytes, 0, &map) != VK_SUCCESS) {
        AP_ERROR("graph: LUT staging map failed");
        goto out;
    }
    memcpy(map, data, (size_t)bytes);
    vkUnmapMemory(dev, staging_mem);

    VkCommandBufferAllocateInfo cba = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = graph->gpu->command_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &cba, &cmd);
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier2 to_dst = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = st->lut_image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    VkDependencyInfo dep_dst = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_dst,
    };
    vkCmdPipelineBarrier2(cmd, &dep_dst);

    VkBufferImageCopy region = {
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
        },
        .imageExtent = { (uint32_t)w, (uint32_t)h, 1 },
    };
    vkCmdCopyBufferToImage(cmd, staging, st->lut_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier2 to_gen = to_dst;
    to_gen.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_gen.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_gen.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    to_gen.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    to_gen.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_gen.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    VkDependencyInfo dep_gen = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_gen,
    };
    vkCmdPipelineBarrier2(cmd, &dep_gen);

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
    vkFreeCommandBuffers(dev, graph->gpu->command_pool, 1, &cmd);
    rc = 0;

out:
    if (staging_mem) vkFreeMemory(dev, staging_mem, NULL);
    if (staging)     vkDestroyBuffer(dev, staging, NULL);
    return rc;
}

// Bake the stage's LUT (from the module variant's bake_lut callback)
// and upload it. On a bake failure an identity LUT is substituted so
// the stage passes through.
int build_stage_lut(ap_pipeline_graph *graph, graph_stage *st,
                    const ap_module_active *a,
                    const ap_edit_stack *stack)
{
    const float *params = NULL;
    const char (*str_params)[AP_EDIT_STR_LEN] = NULL;
    if (st->entry_idx >= 0 && stack) {
        const ap_edit_entry *e = ap_edit_stack_at_const(stack, st->entry_idx);
        if (e) { params = e->params; str_params = e->str_params; }
    }
    const ap_raw_metadata *meta = graph->has_meta ? &graph->meta : NULL;

    size_t n = (size_t)AP_ICC_LUT_DIM * AP_ICC_LUT_DIM
             * AP_ICC_LUT_DIM * 4u;
    float *lut = malloc(n * sizeof(float));
    if (!lut) {
        AP_ERROR("graph: LUT buffer allocation failed");
        return -1;
    }
    if (a->bake_lut(params, str_params, meta, lut) != 0) {
        fill_identity_lut(lut);
    }
    int rc = create_lut_image(graph, st, lut);
    free(lut);
    return rc;
}
