#include "grid.h"

#include "gpu_internal.h"

#include "core/log.h"

#include "grid_vert_spv.h"
#include "grid_frag_spv.h"

#include <stdlib.h>
#include <string.h>

#define GRID_DEFAULT_CELL_SIZE  192
#define GRID_DEFAULT_CELL_GAP_X 8
#define GRID_DEFAULT_CELL_GAP_Y 4
#define GRID_DEFAULT_BORDER     4
#define GRID_MARGIN             16
#define GRID_MIN_CELL_SIZE      64
#define GRID_MAX_CELL_SIZE      512
#define GRID_MAX_THUMBS         4096   // matches MAX_THUMBS in grid.frag

typedef struct {
    float window_size_px[2];
    float origin_px[2];
    float bg_color[4];
    float selected_color[4];
    int   photo_count;
    int   selected_idx;
    int   cells_per_row;
    int   cell_size_px;
    int   cell_gap_x_px;
    int   cell_gap_y_px;
    int   border_px;
    int   _pad0;
} grid_push;

struct ap_grid {
    ap_gpu *gpu;

    VkPipelineLayout pl;
    VkPipeline       pipeline;

    VkDescriptorSetLayout dsl;
    VkDescriptorPool      pool;
    VkDescriptorSet       ds;

    // 1x1 fallback bound into every empty slot.
    VkImage        placeholder_image;
    VkDeviceMemory placeholder_memory;
    VkImageView    placeholder_view;
    VkSampler      placeholder_sampler;

    int photo_count;
    int selected_idx;

    uint8_t *selected_bitmap;     // photo_count bits, allocated in
                                  // set_photo_count; NULL when empty

    int cell_size;
    int cell_gap_x;
    int cell_gap_y;
    int border_px;

    float scroll_y;
};

#define BIT_GET(bm, i) (((bm)[(i) >> 3] >> ((i) & 7)) & 1u)
#define BIT_SET(bm, i) ((bm)[(i) >> 3] |=  (1u << ((i) & 7)))
#define BIT_CLR(bm, i) ((bm)[(i) >> 3] &= ~(1u << ((i) & 7)))

typedef struct {
    int origin_x, origin_y;
    int cells_per_row;
    int cell_size;
    int cell_gap_x;
    int cell_gap_y;
} grid_layout;

static grid_layout layout_for(const ap_grid *g, int win_w, int win_h)
{
    grid_layout L = {
        .origin_x   = GRID_MARGIN,
        .origin_y   = GRID_MARGIN - (int)g->scroll_y,
        .cell_size  = g->cell_size,
        .cell_gap_x = g->cell_gap_x,
        .cell_gap_y = g->cell_gap_y,
    };
    int avail = win_w - 2 * GRID_MARGIN;
    int pitch_x = L.cell_size + L.cell_gap_x;
    L.cells_per_row = pitch_x > 0 ? (avail + L.cell_gap_x) / pitch_x : 1;
    if (L.cells_per_row < 1) L.cells_per_row = 1;
    (void)win_h;
    return L;
}

static int total_rows(const ap_grid *g, int cells_per_row)
{
    if (g->photo_count <= 0 || cells_per_row <= 0) return 0;
    return (g->photo_count + cells_per_row - 1) / cells_per_row;
}

static float max_scroll_for(const ap_grid *g, int win_w, int win_h)
{
    grid_layout L = layout_for(g, win_w, win_h);
    int rows = total_rows(g, L.cells_per_row);
    int pitch_y = L.cell_size + L.cell_gap_y;
    int content_h = rows * pitch_y + GRID_MARGIN;
    if (content_h <= win_h) return 0.0f;
    return (float)(content_h - win_h);
}

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

static int create_placeholder(ap_grid *grid)
{
    VkDevice dev = grid->gpu->device;

    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8G8B8A8_UNORM,
        .extent        = { 1, 1, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev, &ici, NULL, &grid->placeholder_image) != VK_SUCCESS) {
        AP_ERROR("grid: placeholder image create failed");
        return -1;
    }

    VkMemoryRequirements mreq;
    vkGetImageMemoryRequirements(dev, grid->placeholder_image, &mreq);
    int mt = find_memory_type(grid->gpu->physical, mreq.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) {
        AP_ERROR("grid: no device-local memory for placeholder");
        return -1;
    }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mreq.size,
        .memoryTypeIndex = (uint32_t)mt,
    };
    if (vkAllocateMemory(dev, &mai, NULL, &grid->placeholder_memory) != VK_SUCCESS) {
        AP_ERROR("grid: placeholder memory alloc failed");
        return -1;
    }
    vkBindImageMemory(dev, grid->placeholder_image, grid->placeholder_memory, 0);

    // Clear once to the cell placeholder color, transition to
    // SHADER_READ_ONLY_OPTIMAL, and leave it there for the lifetime.
    VkCommandBufferAllocateInfo cba = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = grid->gpu->command_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &cba, &cmd);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier2 to_dst = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_CLEAR_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = grid->placeholder_image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkDependencyInfo dep_a = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_dst,
    };
    vkCmdPipelineBarrier2(cmd, &dep_a);

    VkClearColorValue color = { .float32 = { 0.22f, 0.24f, 0.27f, 1.0f } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, grid->placeholder_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &color, 1, &range);

    VkImageMemoryBarrier2 to_read = to_dst;
    to_read.srcStageMask  = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_read.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_read.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDependencyInfo dep_b = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read,
    };
    vkCmdPipelineBarrier2(cmd, &dep_b);

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
    vkQueueSubmit2(grid->gpu->graphics_queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(grid->gpu->graphics_queue);
    vkFreeCommandBuffers(dev, grid->gpu->command_pool, 1, &cmd);

    VkImageViewCreateInfo vci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = grid->placeholder_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (vkCreateImageView(dev, &vci, NULL, &grid->placeholder_view) != VK_SUCCESS) {
        AP_ERROR("grid: placeholder view create failed");
        return -1;
    }

    VkSamplerCreateInfo sci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    if (vkCreateSampler(dev, &sci, NULL, &grid->placeholder_sampler) != VK_SUCCESS) {
        AP_ERROR("grid: placeholder sampler create failed");
        return -1;
    }
    return 0;
}

static int create_descriptors(ap_grid *grid)
{
    VkDescriptorSetLayoutBinding binding = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = GRID_MAX_THUMBS,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorBindingFlags binding_flag = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount  = 1,
        .pBindingFlags = &binding_flag,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext        = &bf,
        .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = 1,
        .pBindings    = &binding,
    };
    if (vkCreateDescriptorSetLayout(grid->gpu->device, &dslci, NULL,
                                    &grid->dsl) != VK_SUCCESS) {
        AP_ERROR("grid: descriptor set layout failed");
        return -1;
    }

    VkDescriptorPoolSize pool_size = {
        .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = GRID_MAX_THUMBS,
    };
    VkDescriptorPoolCreateInfo pci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets       = 1,
        .poolSizeCount = 1,
        .pPoolSizes    = &pool_size,
    };
    if (vkCreateDescriptorPool(grid->gpu->device, &pci, NULL,
                               &grid->pool) != VK_SUCCESS) {
        AP_ERROR("grid: descriptor pool failed");
        return -1;
    }

    VkDescriptorSetAllocateInfo dai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = grid->pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &grid->dsl,
    };
    if (vkAllocateDescriptorSets(grid->gpu->device, &dai, &grid->ds) != VK_SUCCESS) {
        AP_ERROR("grid: descriptor set alloc failed");
        return -1;
    }

    // Bind the placeholder into every slot up-front so unloaded cells
    // sample a defined texel.
    VkDescriptorImageInfo *infos = calloc(GRID_MAX_THUMBS, sizeof(*infos));
    if (!infos) {
        AP_ERROR("grid: placeholder bind alloc failed");
        return -1;
    }
    for (int i = 0; i < GRID_MAX_THUMBS; i++) {
        infos[i].sampler     = grid->placeholder_sampler;
        infos[i].imageView   = grid->placeholder_view;
        infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = grid->ds,
        .dstBinding      = 0,
        .dstArrayElement = 0,
        .descriptorCount = GRID_MAX_THUMBS,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = infos,
    };
    vkUpdateDescriptorSets(grid->gpu->device, 1, &write, 0, NULL);
    free(infos);
    return 0;
}

static int create_pipeline(ap_grid *grid)
{
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(grid_push),
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &grid->dsl,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    if (vkCreatePipelineLayout(grid->gpu->device, &plci, NULL, &grid->pl) != VK_SUCCESS) {
        AP_ERROR("grid: pipeline layout failed");
        return -1;
    }

    VkShaderModuleCreateInfo vsci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = grid_vert_spv_size,
        .pCode    = grid_vert_spv,
    };
    VkShaderModule vert = VK_NULL_HANDLE;
    if (vkCreateShaderModule(grid->gpu->device, &vsci, NULL, &vert) != VK_SUCCESS) {
        AP_ERROR("grid: vertex shader module failed");
        return -1;
    }

    VkShaderModuleCreateInfo fsci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = grid_frag_spv_size,
        .pCode    = grid_frag_spv,
    };
    VkShaderModule frag = VK_NULL_HANDLE;
    if (vkCreateShaderModule(grid->gpu->device, &fsci, NULL, &frag) != VK_SUCCESS) {
        AP_ERROR("grid: fragment shader module failed");
        vkDestroyShaderModule(grid->gpu->device, vert, NULL);
        return -1;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage  = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vert, .pName = "main" },
        { .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = frag, .pName = "main" },
    };

    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount  = 1,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth   = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState att = {
        .blendEnable    = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &att,
    };
    VkDynamicState dyn_states[2] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dyn_states,
    };
    VkFormat color_fmt = grid->gpu->swapchain_format;
    VkPipelineRenderingCreateInfo rci = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &color_fmt,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &rci,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState      = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState   = &ms,
        .pColorBlendState    = &cb,
        .pDynamicState       = &dyn,
        .layout              = grid->pl,
    };

    VkResult r = vkCreateGraphicsPipelines(grid->gpu->device, VK_NULL_HANDLE,
                                           1, &gpci, NULL, &grid->pipeline);
    vkDestroyShaderModule(grid->gpu->device, vert, NULL);
    vkDestroyShaderModule(grid->gpu->device, frag, NULL);
    if (r != VK_SUCCESS) {
        AP_ERROR("grid: vkCreateGraphicsPipelines -> %s", gpu_vk_result_str(r));
        return -1;
    }
    return 0;
}

ap_grid *ap_grid_create(ap_gpu *g)
{
    if (!g) {
        AP_ERROR("ap_grid_create: invalid args");
        return NULL;
    }

    ap_grid *grid = calloc(1, sizeof(*grid));
    if (!grid) {
        AP_ERROR("ap_grid_create: out of memory");
        return NULL;
    }
    grid->gpu          = g;
    grid->cell_size    = GRID_DEFAULT_CELL_SIZE;
    grid->cell_gap_x   = GRID_DEFAULT_CELL_GAP_X;
    grid->cell_gap_y   = GRID_DEFAULT_CELL_GAP_Y;
    grid->border_px    = GRID_DEFAULT_BORDER;
    grid->selected_idx = 0;

    if (create_placeholder(grid)  < 0) goto fail;
    if (create_descriptors(grid)  < 0) goto fail;
    if (create_pipeline(grid)     < 0) goto fail;
    return grid;

fail:
    ap_grid_destroy(grid);
    return NULL;
}

void ap_grid_destroy(ap_grid *grid)
{
    if (!grid) return;
    VkDevice dev = grid->gpu->device;
    if (grid->pipeline) vkDestroyPipeline(dev, grid->pipeline, NULL);
    if (grid->pl)       vkDestroyPipelineLayout(dev, grid->pl, NULL);
    if (grid->pool)     vkDestroyDescriptorPool(dev, grid->pool, NULL);
    if (grid->dsl)      vkDestroyDescriptorSetLayout(dev, grid->dsl, NULL);
    if (grid->placeholder_sampler) vkDestroySampler(dev, grid->placeholder_sampler, NULL);
    if (grid->placeholder_view)    vkDestroyImageView(dev, grid->placeholder_view, NULL);
    if (grid->placeholder_image)   vkDestroyImage(dev, grid->placeholder_image, NULL);
    if (grid->placeholder_memory)  vkFreeMemory(dev, grid->placeholder_memory, NULL);
    free(grid->selected_bitmap);
    free(grid);
}

void ap_grid_set_thumbnail(ap_grid *grid, int idx,
                           VkImageView view, VkSampler sampler)
{
    if (!grid || idx < 0 || idx >= GRID_MAX_THUMBS) return;

    VkDescriptorImageInfo info = {
        .sampler     = (view && sampler) ? sampler : grid->placeholder_sampler,
        .imageView   = (view && sampler) ? view    : grid->placeholder_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = grid->ds,
        .dstBinding      = 0,
        .dstArrayElement = (uint32_t)idx,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &info,
    };
    vkUpdateDescriptorSets(grid->gpu->device, 1, &write, 0, NULL);
}

void ap_grid_set_photo_count(ap_grid *grid, int count)
{
    if (!grid) return;
    if (count < 0) count = 0;
    grid->photo_count = count;
    if (grid->selected_idx >= count) {
        grid->selected_idx = count > 0 ? count - 1 : 0;
    }
    grid->scroll_y = 0.0f;

    // Reallocate the selection bitmap to match the new photo count.
    free(grid->selected_bitmap);
    grid->selected_bitmap = NULL;
    if (count > 0) {
        size_t bytes = (size_t)((count + 7) / 8);
        grid->selected_bitmap = calloc(bytes, 1);
        if (grid->selected_bitmap && count > 0) {
            BIT_SET(grid->selected_bitmap, grid->selected_idx);
        }
    }

    // Reset the entire descriptor array back to the placeholder. The
    // previous library's ap_thumbnail textures are about to be (or
    // have just been) destroyed; their views can't stay bound.
    VkDescriptorImageInfo *infos = calloc(GRID_MAX_THUMBS, sizeof(*infos));
    if (!infos) return;
    for (int i = 0; i < GRID_MAX_THUMBS; i++) {
        infos[i].sampler     = grid->placeholder_sampler;
        infos[i].imageView   = grid->placeholder_view;
        infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = grid->ds,
        .dstBinding      = 0,
        .dstArrayElement = 0,
        .descriptorCount = GRID_MAX_THUMBS,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = infos,
    };
    vkUpdateDescriptorSets(grid->gpu->device, 1, &write, 0, NULL);
    free(infos);
}

void ap_grid_set_selected(ap_grid *grid, int idx)
{
    if (!grid || grid->photo_count <= 0) return;
    if (idx < 0) idx = 0;
    if (idx >= grid->photo_count) idx = grid->photo_count - 1;
    grid->selected_idx = idx;
    if (grid->selected_bitmap) {
        BIT_SET(grid->selected_bitmap, idx);
    }
}

static void selection_clear(ap_grid *grid)
{
    if (!grid->selected_bitmap || grid->photo_count <= 0) return;
    memset(grid->selected_bitmap, 0, (size_t)((grid->photo_count + 7) / 8));
}

void ap_grid_select_only(ap_grid *grid, int idx)
{
    if (!grid || grid->photo_count <= 0) return;
    if (idx < 0) idx = 0;
    if (idx >= grid->photo_count) idx = grid->photo_count - 1;
    selection_clear(grid);
    if (grid->selected_bitmap) BIT_SET(grid->selected_bitmap, idx);
    grid->selected_idx = idx;
}

void ap_grid_select_toggle(ap_grid *grid, int idx)
{
    if (!grid || !grid->selected_bitmap
        || idx < 0 || idx >= grid->photo_count) return;
    if (BIT_GET(grid->selected_bitmap, idx)) {
        BIT_CLR(grid->selected_bitmap, idx);
        // Focus stays where it was; if we just cleared the current
        // focus, leave focus alone — it remains visible but
        // unselected, which is fine UX.
    } else {
        BIT_SET(grid->selected_bitmap, idx);
        grid->selected_idx = idx;
    }
}

void ap_grid_select_range(ap_grid *grid, int anchor_idx, int idx)
{
    if (!grid || !grid->selected_bitmap || grid->photo_count <= 0) return;
    if (anchor_idx < 0) anchor_idx = 0;
    if (anchor_idx >= grid->photo_count) anchor_idx = grid->photo_count - 1;
    if (idx < 0) idx = 0;
    if (idx >= grid->photo_count) idx = grid->photo_count - 1;

    int lo = anchor_idx < idx ? anchor_idx : idx;
    int hi = anchor_idx < idx ? idx : anchor_idx;
    selection_clear(grid);
    for (int i = lo; i <= hi; i++) BIT_SET(grid->selected_bitmap, i);
    grid->selected_idx = idx;
}

bool ap_grid_is_selected(const ap_grid *grid, int idx)
{
    if (!grid || !grid->selected_bitmap
        || idx < 0 || idx >= grid->photo_count) return false;
    return BIT_GET(grid->selected_bitmap, idx) != 0;
}

int ap_grid_selection_count(const ap_grid *grid)
{
    if (!grid || !grid->selected_bitmap) return 0;
    int n = 0;
    for (int i = 0; i < grid->photo_count; i++) {
        if (BIT_GET(grid->selected_bitmap, i)) n++;
    }
    return n;
}

int ap_grid_selected(const ap_grid *grid)
{
    return grid ? grid->selected_idx : 0;
}

int ap_grid_photo_count(const ap_grid *grid)
{
    return grid ? grid->photo_count : 0;
}

int ap_grid_cells_per_row(const ap_grid *grid, int win_width, int win_height)
{
    if (!grid) return 1;
    grid_layout L = layout_for(grid, win_width, win_height);
    return L.cells_per_row;
}

int ap_grid_cell_size(const ap_grid *grid)
{
    return grid ? grid->cell_size : 0;
}

void ap_grid_set_cell_size(ap_grid *grid, int px)
{
    if (!grid) return;
    if (px < GRID_MIN_CELL_SIZE) px = GRID_MIN_CELL_SIZE;
    if (px > GRID_MAX_CELL_SIZE) px = GRID_MAX_CELL_SIZE;
    grid->cell_size = px;
}

void ap_grid_scroll(ap_grid *grid, float dy, int win_width, int win_height)
{
    if (!grid) return;
    float ms = max_scroll_for(grid, win_width, win_height);
    grid->scroll_y += dy;
    if (grid->scroll_y < 0.0f) grid->scroll_y = 0.0f;
    if (grid->scroll_y > ms)   grid->scroll_y = ms;
}

void ap_grid_ensure_visible(ap_grid *grid, int idx,
                            int win_width, int win_height)
{
    if (!grid || idx < 0 || idx >= grid->photo_count) return;

    grid_layout L = layout_for(grid, win_width, win_height);
    if (L.cells_per_row <= 0) return;
    int pitch_y = L.cell_size + L.cell_gap_y;
    int row     = idx / L.cells_per_row;

    // Where the cell *would* sit on screen at the current scroll.
    float cell_top    = (float)GRID_MARGIN + (float)(row * pitch_y) - grid->scroll_y;
    float cell_bottom = cell_top + (float)L.cell_size;

    if (cell_top < (float)GRID_MARGIN) {
        grid->scroll_y -= (float)GRID_MARGIN - cell_top;
    } else if (cell_bottom > (float)(win_height - GRID_MARGIN)) {
        grid->scroll_y += cell_bottom - (float)(win_height - GRID_MARGIN);
    }
    float ms = max_scroll_for(grid, win_width, win_height);
    if (grid->scroll_y < 0.0f) grid->scroll_y = 0.0f;
    if (grid->scroll_y > ms)   grid->scroll_y = ms;
}

int ap_grid_hit_test(const ap_grid *grid,
                     float screen_x, float screen_y,
                     int win_width, int win_height)
{
    if (!grid || grid->photo_count <= 0) return -1;

    grid_layout L = layout_for(grid, win_width, win_height);
    int pitch_x = L.cell_size + L.cell_gap_x;
    int pitch_y = L.cell_size + L.cell_gap_y;

    float x = screen_x - (float)L.origin_x;
    float y = screen_y - (float)L.origin_y;
    if (x < 0.0f || y < 0.0f) return -1;

    int col = (int)(x / (float)pitch_x);
    int row = (int)(y / (float)pitch_y);
    if (col < 0 || col >= L.cells_per_row) return -1;

    float in_x = x - (float)(col * pitch_x);
    float in_y = y - (float)(row * pitch_y);
    if (in_x >= (float)L.cell_size || in_y >= (float)L.cell_size) return -1;

    int idx = row * L.cells_per_row + col;
    if (idx < 0 || idx >= grid->photo_count) return -1;
    return idx;
}

int ap_grid_cell_rect(const ap_grid *grid, int idx,
                      int win_width, int win_height,
                      float *out_x, float *out_y,
                      float *out_w, float *out_h)
{
    if (!grid || idx < 0 || idx >= grid->photo_count) return -1;

    grid_layout L = layout_for(grid, win_width, win_height);
    int pitch_x = L.cell_size + L.cell_gap_x;
    int pitch_y = L.cell_size + L.cell_gap_y;
    int row = idx / L.cells_per_row;
    int col = idx % L.cells_per_row;

    if (out_x) *out_x = (float)(L.origin_x + col * pitch_x);
    if (out_y) *out_y = (float)(L.origin_y + row * pitch_y);
    if (out_w) *out_w = (float)L.cell_size;
    if (out_h) *out_h = (float)L.cell_size;
    (void)win_height;
    return 0;
}

void ap_grid_record(ap_grid *grid, VkCommandBuffer cmd,
                    int win_width, int win_height)
{
    if (!grid) return;
    if (win_width <= 0 || win_height <= 0) return;

    VkViewport viewport = {
        .x = 0.0f, .y = 0.0f,
        .width  = (float)win_width,
        .height = (float)win_height,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = { (uint32_t)win_width, (uint32_t)win_height },
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    grid_layout L = layout_for(grid, win_width, win_height);
    grid_push pc = {
        .window_size_px  = { (float)win_width, (float)win_height },
        .origin_px       = { (float)L.origin_x, (float)L.origin_y },
        .bg_color        = { 0.10f, 0.10f, 0.10f, 1.0f },
        .selected_color  = { 0.95f, 0.78f, 0.25f, 1.0f },
        .photo_count     = grid->photo_count,
        .selected_idx    = grid->selected_idx,
        .cells_per_row   = L.cells_per_row,
        .cell_size_px    = L.cell_size,
        .cell_gap_x_px   = L.cell_gap_x,
        .cell_gap_y_px   = L.cell_gap_y,
        .border_px       = grid->border_px,
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, grid->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, grid->pl,
                            0, 1, &grid->ds, 0, NULL);
    vkCmdPushConstants(cmd, grid->pl, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}
