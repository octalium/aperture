#include "grid.h"

#include "gpu_internal.h"

#include "core/log.h"

#include "grid_vert_spv.h"
#include "grid_frag_spv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GRID_DEFAULT_CELL_SIZE  192
#define GRID_DEFAULT_CELL_GAP_X 8
#define GRID_DEFAULT_CELL_GAP_Y 4
#define GRID_DEFAULT_BORDER     4
#define GRID_MARGIN             16
#define GRID_MIN_CELL_SIZE      64
#define GRID_MAX_CELL_SIZE      512
#define GRID_THUMB_CAP_INIT     4096   // starting descriptor array size
#define GRID_THUMB_CAP_MAX      65536  // hard cap; matches MAX_THUMBS in grid.frag

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
    int   hover_idx;
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
    int thumb_capacity;           // current descriptor array size (>= photo_count)
    int selected_idx;
    int hover_idx;                // -1 when no cell is hovered

    uint8_t *selected_bitmap;     // photo_count bits, allocated in
                                  // set_photo_count; NULL when empty

    int   cell_size;        // integer target (clamped, canonical)
    float cell_size_f;      // fractional working value, lerped toward cell_size
    int   cell_gap_x;
    int   cell_gap_y;
    int   border_px;

    float scroll_y;         // current rendered scroll position
    float scroll_y_target;  // target the easing is converging to

    // Sub-rect of the framebuffer to render + lay out within. Zero
    // size means "fall back to the swapchain extent passed to
    // ap_grid_record".
    int rect_x;
    int rect_y;
    int rect_w;
    int rect_h;
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

// Resolve the effective render rect: the stored sub-rect when set,
// otherwise the full-window fallback. Origin coords are in framebuffer
// space; w/h are the inner extents the grid lays out into.
static void effective_rect(const ap_grid *g, int win_w, int win_h,
                           int *rx, int *ry, int *rw, int *rh)
{
    if (g->rect_w > 0 && g->rect_h > 0) {
        *rx = g->rect_x;
        *ry = g->rect_y;
        *rw = g->rect_w;
        *rh = g->rect_h;
    } else {
        *rx = 0;
        *ry = 0;
        *rw = win_w;
        *rh = win_h;
    }
}

int ap_grid_rows_per_page(const ap_grid *grid, int win_width, int win_height)
{
    if (!grid) return 1;
    int rx, ry, rw, rh;
    effective_rect(grid, win_width, win_height, &rx, &ry, &rw, &rh);
    (void)rx; (void)ry; (void)rw;
    int pitch_y = grid->cell_size + grid->cell_gap_y;
    int rows    = (pitch_y > 0) ? (rh / pitch_y) : 1;
    return (rows > 0) ? rows : 1;
}

void ap_grid_set_render_rect(ap_grid *grid, int x, int y, int w, int h)
{
    if (!grid) return;
    if (w <= 0 || h <= 0) {
        grid->rect_x = 0;
        grid->rect_y = 0;
        grid->rect_w = 0;
        grid->rect_h = 0;
        return;
    }
    grid->rect_x = x;
    grid->rect_y = y;
    grid->rect_w = w;
    grid->rect_h = h;
}

static grid_layout layout_for(const ap_grid *g, int win_w, int win_h)
{
    grid_layout L = {
        .origin_x   = GRID_MARGIN,
        .origin_y   = GRID_MARGIN - (int)g->scroll_y,
        .cell_size  = (int)g->cell_size_f,
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

static int create_pipeline(ap_grid *grid);

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

static int create_descriptors(ap_grid *grid, int capacity)
{
    VkDescriptorSetLayoutBinding binding = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = (uint32_t)capacity,
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
        .descriptorCount = (uint32_t)capacity,
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
    VkDescriptorImageInfo *infos = calloc((size_t)capacity, sizeof(*infos));
    if (!infos) {
        AP_ERROR("grid: placeholder bind alloc failed");
        return -1;
    }
    for (int i = 0; i < capacity; i++) {
        infos[i].sampler     = grid->placeholder_sampler;
        infos[i].imageView   = grid->placeholder_view;
        infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = grid->ds,
        .dstBinding      = 0,
        .dstArrayElement = 0,
        .descriptorCount = (uint32_t)capacity,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = infos,
    };
    vkUpdateDescriptorSets(grid->gpu->device, 1, &write, 0, NULL);
    free(infos);
    grid->thumb_capacity = capacity;
    return 0;
}

// Tear down descriptors + pipeline layout + pipeline (but not the
// placeholder or GPU) and rebuild with a larger capacity. Called when
// set_photo_count receives a count exceeding the current capacity.
// The caller must have the GPU idle before invoking this.
static int regrow_descriptors(ap_grid *grid, int new_capacity)
{
    VkDevice dev = grid->gpu->device;
    if (grid->pipeline) {
        vkDestroyPipeline(dev, grid->pipeline, NULL);
        grid->pipeline = VK_NULL_HANDLE;
    }
    if (grid->pl) {
        vkDestroyPipelineLayout(dev, grid->pl, NULL);
        grid->pl = VK_NULL_HANDLE;
    }
    if (grid->pool) {
        vkDestroyDescriptorPool(dev, grid->pool, NULL);
        grid->pool = VK_NULL_HANDLE;
    }
    if (grid->dsl) {
        vkDestroyDescriptorSetLayout(dev, grid->dsl, NULL);
        grid->dsl = VK_NULL_HANDLE;
    }
    grid->ds = VK_NULL_HANDLE;

    if (create_descriptors(grid, new_capacity) < 0) return -1;
    if (create_pipeline(grid)                  < 0) return -1;
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
    grid->cell_size_f  = (float)GRID_DEFAULT_CELL_SIZE;
    grid->cell_gap_x   = GRID_DEFAULT_CELL_GAP_X;
    grid->cell_gap_y   = GRID_DEFAULT_CELL_GAP_Y;
    grid->border_px    = GRID_DEFAULT_BORDER;
    grid->selected_idx = 0;
    grid->hover_idx    = -1;

    if (create_placeholder(grid)                    < 0) goto fail;
    if (create_descriptors(grid, GRID_THUMB_CAP_INIT) < 0) goto fail;
    if (create_pipeline(grid)                       < 0) goto fail;
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
    if (!grid || idx < 0 || idx >= grid->thumb_capacity) return;

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

    // Grow the descriptor capacity when the new photo count exceeds it.
    // Cap at GRID_THUMB_CAP_MAX (matches MAX_THUMBS in grid.frag).
    if (count > grid->thumb_capacity) {
        if (count > GRID_THUMB_CAP_MAX) {
            AP_WARN("grid: library has %d photos; only %d will be shown "
                    "(GRID_THUMB_CAP_MAX)", count, GRID_THUMB_CAP_MAX);
            count = GRID_THUMB_CAP_MAX;
        }
        // Double the capacity (clamped to GRID_THUMB_CAP_MAX) so repeated
        // small grows don't each trigger a full rebuild.
        int new_cap = grid->thumb_capacity * 2;
        if (new_cap < count)         new_cap = count;
        if (new_cap > GRID_THUMB_CAP_MAX) new_cap = GRID_THUMB_CAP_MAX;
        AP_INFO("grid: growing descriptor capacity %d -> %d",
                grid->thumb_capacity, new_cap);
        if (regrow_descriptors(grid, new_cap) != 0) {
            AP_ERROR("grid: descriptor regrow failed; keeping old capacity");
            if (count > grid->thumb_capacity) count = grid->thumb_capacity;
        }
    }

    grid->photo_count = count;
    if (grid->selected_idx >= count) {
        grid->selected_idx = count > 0 ? count - 1 : 0;
    }
    grid->scroll_y        = 0.0f;
    grid->scroll_y_target = 0.0f;

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
    VkDescriptorImageInfo *infos = calloc((size_t)grid->thumb_capacity,
                                          sizeof(*infos));
    if (!infos) return;
    for (int i = 0; i < grid->thumb_capacity; i++) {
        infos[i].sampler     = grid->placeholder_sampler;
        infos[i].imageView   = grid->placeholder_view;
        infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = grid->ds,
        .dstBinding      = 0,
        .dstArrayElement = 0,
        .descriptorCount = (uint32_t)grid->thumb_capacity,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = infos,
    };
    vkUpdateDescriptorSets(grid->gpu->device, 1, &write, 0, NULL);
    free(infos);
}

void ap_grid_set_hover(ap_grid *grid, int idx)
{
    if (!grid) return;
    grid->hover_idx = idx;
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
    int rx, ry, rw, rh;
    effective_rect(grid, win_width, win_height, &rx, &ry, &rw, &rh);
    (void)rx; (void)ry;
    grid_layout L = layout_for(grid, rw, rh);
    return L.cells_per_row;
}

int ap_grid_cell_size(const ap_grid *grid)
{
    return grid ? grid->cell_size : 0;
}

void ap_grid_zoom_at(ap_grid *grid, int new_cell_px,
                     float screen_x, float screen_y,
                     int win_width, int win_height)
{
    if (!grid || grid->photo_count <= 0) return;
    (void)screen_x;

    if (new_cell_px < GRID_MIN_CELL_SIZE) new_cell_px = GRID_MIN_CELL_SIZE;
    if (new_cell_px > GRID_MAX_CELL_SIZE) new_cell_px = GRID_MAX_CELL_SIZE;

    int rx, ry, rw, rh;
    effective_rect(grid, win_width, win_height, &rx, &ry, &rw, &rh);
    (void)rw; (void)rh;

    // old pitch uses the fractional cell size (what's currently rendered)
    float old_pitch = grid->cell_size_f + (float)grid->cell_gap_y;
    float new_pitch = (float)new_cell_px  + (float)grid->cell_gap_y;
    if (old_pitch <= 0.0f || new_pitch <= 0.0f) {
        grid->cell_size = new_cell_px;
        return;
    }

    // Y distance from the render-rect top to the cursor, adjusted for
    // the current scroll target. This is the position in the unscrolled
    // content space (relative to GRID_MARGIN origin) that the cursor
    // currently points at.
    float viewport_y      = screen_y - (float)ry - (float)GRID_MARGIN;
    float content_y       = viewport_y + grid->scroll_y_target;

    // After rescaling, the same content fraction maps to a new content_y.
    float new_content_y   = content_y * (new_pitch / old_pitch);

    // Adjust scroll so the cursor stays over the same content point.
    float new_scroll      = new_content_y - viewport_y;

    grid->cell_size   = new_cell_px;

    // Clamp the new scroll to the max for the new layout.
    // Temporarily set cell_size_f to compute max_scroll correctly.
    float saved_f = grid->cell_size_f;
    grid->cell_size_f = (float)new_cell_px;
    float ms = max_scroll_for(grid, rw, rh);
    grid->cell_size_f = saved_f;

    if (new_scroll < 0.0f) new_scroll = 0.0f;
    if (new_scroll > ms)   new_scroll = ms;
    grid->scroll_y_target = new_scroll;
}

void ap_grid_set_cell_size(ap_grid *grid, int px)
{
    if (!grid) return;
    if (px < GRID_MIN_CELL_SIZE) px = GRID_MIN_CELL_SIZE;
    if (px > GRID_MAX_CELL_SIZE) px = GRID_MAX_CELL_SIZE;
    grid->cell_size = px;
    // cell_size_f lerps toward cell_size each frame via ap_grid_update.
}

void ap_grid_reset_cell_size(ap_grid *grid)
{
    if (!grid) return;
    grid->cell_size = GRID_DEFAULT_CELL_SIZE;
    // Let the lerp animate the reset rather than snapping.
}

void ap_grid_update(ap_grid *grid, float dt)
{
    if (!grid || dt <= 0.0f) return;

    // Exponential smoothing: value reaches ~63% of target per half-life.
    // factor = 1 - e^(-dt / half_life)
    const float scroll_half_life  = 0.10f;   // seconds
    const float zoom_half_life    = 0.08f;   // seconds
    float sf = 1.0f - expf(-dt / scroll_half_life);
    float zf = 1.0f - expf(-dt / zoom_half_life);

    grid->scroll_y    += (grid->scroll_y_target - grid->scroll_y)    * sf;
    grid->cell_size_f += ((float)grid->cell_size - grid->cell_size_f) * zf;

    // Snap to target when close enough to avoid permanent micro-drift.
    if (fabsf(grid->scroll_y    - grid->scroll_y_target) < 0.5f)
        grid->scroll_y    = grid->scroll_y_target;
    if (fabsf(grid->cell_size_f - (float)grid->cell_size) < 0.5f)
        grid->cell_size_f = (float)grid->cell_size;
}

void ap_grid_scroll(ap_grid *grid, float dy, int win_width, int win_height)
{
    if (!grid) return;
    int rx, ry, rw, rh;
    effective_rect(grid, win_width, win_height, &rx, &ry, &rw, &rh);
    (void)rx; (void)ry;
    float ms = max_scroll_for(grid, rw, rh);
    grid->scroll_y_target += dy;
    if (grid->scroll_y_target < 0.0f) grid->scroll_y_target = 0.0f;
    if (grid->scroll_y_target > ms)   grid->scroll_y_target = ms;
}

void ap_grid_ensure_visible(ap_grid *grid, int idx,
                            int win_width, int win_height)
{
    if (!grid || idx < 0 || idx >= grid->photo_count) return;

    int rx, ry, rw, rh;
    effective_rect(grid, win_width, win_height, &rx, &ry, &rw, &rh);
    (void)rx; (void)ry;
    grid_layout L = layout_for(grid, rw, rh);
    if (L.cells_per_row <= 0) return;
    int pitch_y = L.cell_size + L.cell_gap_y;
    int row     = idx / L.cells_per_row;

    // Where the cell *would* sit within the render rect at the current
    // scroll target. Adjust the target (not scroll_y) so the easing
    // carries it smoothly into view.
    float cell_top    = (float)GRID_MARGIN + (float)(row * pitch_y)
                      - grid->scroll_y_target;
    float cell_bottom = cell_top + (float)L.cell_size;

    if (cell_top < (float)GRID_MARGIN) {
        grid->scroll_y_target -= (float)GRID_MARGIN - cell_top;
    } else if (cell_bottom > (float)(rh - GRID_MARGIN)) {
        grid->scroll_y_target += cell_bottom - (float)(rh - GRID_MARGIN);
    }
    float ms = max_scroll_for(grid, rw, rh);
    if (grid->scroll_y_target < 0.0f) grid->scroll_y_target = 0.0f;
    if (grid->scroll_y_target > ms)   grid->scroll_y_target = ms;
}

int ap_grid_hit_test(const ap_grid *grid,
                     float screen_x, float screen_y,
                     int win_width, int win_height)
{
    if (!grid || grid->photo_count <= 0) return -1;

    int rx, ry, rw, rh;
    effective_rect(grid, win_width, win_height, &rx, &ry, &rw, &rh);
    grid_layout L = layout_for(grid, rw, rh);
    int pitch_x = L.cell_size + L.cell_gap_x;
    int pitch_y = L.cell_size + L.cell_gap_y;

    // Caller passes window-absolute coords; subtract the render-rect
    // origin first so the layout math operates in render-rect space.
    float x = screen_x - (float)rx - (float)L.origin_x;
    float y = screen_y - (float)ry - (float)L.origin_y;
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

    int rx, ry, rw, rh;
    effective_rect(grid, win_width, win_height, &rx, &ry, &rw, &rh);
    grid_layout L = layout_for(grid, rw, rh);
    int pitch_x = L.cell_size + L.cell_gap_x;
    int pitch_y = L.cell_size + L.cell_gap_y;
    int row = idx / L.cells_per_row;
    int col = idx % L.cells_per_row;

    // Returned coords are window-absolute (drawList overlays live in
    // window coords). Add the render-rect origin.
    if (out_x) *out_x = (float)(rx + L.origin_x + col * pitch_x);
    if (out_y) *out_y = (float)(ry + L.origin_y + row * pitch_y);
    if (out_w) *out_w = (float)L.cell_size;
    if (out_h) *out_h = (float)L.cell_size;
    return 0;
}

void ap_grid_record(ap_grid *grid, VkCommandBuffer cmd,
                    int win_width, int win_height)
{
    if (!grid) return;
    if (win_width <= 0 || win_height <= 0) return;

    int rx, ry, rw, rh;
    effective_rect(grid, win_width, win_height, &rx, &ry, &rw, &rh);
    if (rw <= 0 || rh <= 0) return;

    VkViewport viewport = {
        .x = (float)rx, .y = (float)ry,
        .width  = (float)rw,
        .height = (float)rh,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    VkRect2D scissor = {
        .offset = { rx, ry },
        .extent = { (uint32_t)rw, (uint32_t)rh },
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    grid_layout L = layout_for(grid, rw, rh);
    // window_size_px feeds the fragment shader's render-rect-relative
    // coordinate math; passing the render-rect extent (not the
    // swapchain extent) keeps NDC→pixel correspondence consistent
    // with the viewport set above.
    grid_push pc = {
        .window_size_px  = { (float)rw, (float)rh },
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
        .hover_idx       = grid->hover_idx,
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, grid->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, grid->pl,
                            0, 1, &grid->ds, 0, NULL);
    vkCmdPushConstants(cmd, grid->pl, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}
