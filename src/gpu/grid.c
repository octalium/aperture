#include "grid.h"

#include "gpu_internal.h"

#include "core/log.h"

#include "grid_vert_spv.h"
#include "grid_frag_spv.h"

#include <stdlib.h>
#include <string.h>

#define GRID_DEFAULT_CELL_SIZE  192
#define GRID_DEFAULT_CELL_GAP   12
#define GRID_DEFAULT_BORDER     4
#define GRID_MARGIN             24

typedef struct {
    float window_size_px[2];
    float origin_px[2];
    float bg_color[4];
    float cell_color[4];
    float selected_color[4];
    int   photo_count;
    int   selected_idx;
    int   cells_per_row;
    int   cell_size_px;
    int   cell_gap_px;
    int   border_px;
    int   _pad0;
    int   _pad1;
} grid_push;

struct ap_grid {
    ap_gpu *gpu;

    VkPipelineLayout pl;
    VkPipeline       pipeline;

    int photo_count;
    int selected_idx;

    int cell_size;
    int cell_gap;
    int border_px;
};

typedef struct {
    int origin_x, origin_y;
    int cells_per_row;
    int cell_size;
    int cell_gap;
} grid_layout;

static grid_layout layout_for(const ap_grid *g, int win_w, int win_h)
{
    grid_layout L = {
        .origin_x = GRID_MARGIN,
        .origin_y = GRID_MARGIN,
        .cell_size = g->cell_size,
        .cell_gap  = g->cell_gap,
    };
    int avail = win_w - 2 * GRID_MARGIN;
    int pitch = L.cell_size + L.cell_gap;
    L.cells_per_row = pitch > 0 ? (avail + L.cell_gap) / pitch : 1;
    if (L.cells_per_row < 1) L.cells_per_row = 1;
    (void)win_h;
    return L;
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
    grid->cell_gap     = GRID_DEFAULT_CELL_GAP;
    grid->border_px    = GRID_DEFAULT_BORDER;
    grid->selected_idx = 0;

    if (create_pipeline(grid) < 0) goto fail;
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
    free(grid);
}

void ap_grid_set_photo_count(ap_grid *grid, int count)
{
    if (!grid) return;
    if (count < 0) count = 0;
    grid->photo_count = count;
    if (grid->selected_idx >= count) {
        grid->selected_idx = count > 0 ? count - 1 : 0;
    }
}

void ap_grid_set_selected(ap_grid *grid, int idx)
{
    if (!grid || grid->photo_count <= 0) return;
    if (idx < 0) idx = 0;
    if (idx >= grid->photo_count) idx = grid->photo_count - 1;
    grid->selected_idx = idx;
}

int ap_grid_selected(const ap_grid *grid)
{
    return grid ? grid->selected_idx : 0;
}

int ap_grid_photo_count(const ap_grid *grid)
{
    return grid ? grid->photo_count : 0;
}

int ap_grid_hit_test(const ap_grid *grid,
                     float screen_x, float screen_y,
                     int win_width, int win_height)
{
    if (!grid || grid->photo_count <= 0) return -1;

    grid_layout L = layout_for(grid, win_width, win_height);
    int pitch = L.cell_size + L.cell_gap;

    float x = screen_x - (float)L.origin_x;
    float y = screen_y - (float)L.origin_y;
    if (x < 0.0f || y < 0.0f) return -1;

    int col = (int)(x / (float)pitch);
    int row = (int)(y / (float)pitch);
    if (col < 0 || col >= L.cells_per_row) return -1;

    float in_x = x - (float)(col * pitch);
    float in_y = y - (float)(row * pitch);
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
    int pitch = L.cell_size + L.cell_gap;
    int row = idx / L.cells_per_row;
    int col = idx % L.cells_per_row;

    if (out_x) *out_x = (float)(L.origin_x + col * pitch);
    if (out_y) *out_y = (float)(L.origin_y + row * pitch);
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
        .cell_color      = { 0.22f, 0.24f, 0.27f, 1.0f },
        .selected_color  = { 0.95f, 0.78f, 0.25f, 1.0f },
        .photo_count     = grid->photo_count,
        .selected_idx    = grid->selected_idx,
        .cells_per_row   = L.cells_per_row,
        .cell_size_px    = L.cell_size,
        .cell_gap_px     = L.cell_gap,
        .border_px       = grid->border_px,
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, grid->pipeline);
    vkCmdPushConstants(cmd, grid->pl, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}
