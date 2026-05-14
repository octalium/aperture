#include "canvas.h"

#include "gpu_internal.h"

#include "core/log.h"

#include "canvas_vert_spv.h"
#include "canvas_frag_spv.h"

#include <stdlib.h>
#include <string.h>

// Default user-zoom when a photo binds (1.0 = fit-to-window). Slightly
// under 1.0 leaves a margin around the image instead of butting it
// against the viewport edges.
#define AP_CANVAS_DEFAULT_ZOOM 0.9f

typedef struct {
    float image_size_px[2];
    float window_size_px[2];
    float pan_px[2];
    float zoom;
    float fit_scale;
    float bg_color[4];
} canvas_push;

struct ap_canvas {
    ap_gpu  *gpu;
    VkFormat color_format;

    VkDescriptorSetLayout dsl;
    VkPipelineLayout      pl;
    VkPipeline            pipeline;
    VkDescriptorPool      pool;
    VkDescriptorSet       ds;

    bool         has_input;
    int          image_width;
    int          image_height;

    float zoom;
    float pan_x;
    float pan_y;
};

static int create_descriptor(ap_canvas *canvas)
{
    VkDescriptorSetLayoutBinding binding = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings    = &binding,
    };
    if (vkCreateDescriptorSetLayout(canvas->gpu->device, &dslci, NULL, &canvas->dsl) != VK_SUCCESS) {
        AP_ERROR("canvas: descriptor set layout failed");
        return -1;
    }

    VkDescriptorPoolSize pool_size = {
        .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
    };
    VkDescriptorPoolCreateInfo pci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = 1,
        .poolSizeCount = 1,
        .pPoolSizes    = &pool_size,
    };
    if (vkCreateDescriptorPool(canvas->gpu->device, &pci, NULL, &canvas->pool) != VK_SUCCESS) {
        AP_ERROR("canvas: descriptor pool failed");
        return -1;
    }

    VkDescriptorSetAllocateInfo dai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = canvas->pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &canvas->dsl,
    };
    if (vkAllocateDescriptorSets(canvas->gpu->device, &dai, &canvas->ds) != VK_SUCCESS) {
        AP_ERROR("canvas: descriptor set alloc failed");
        return -1;
    }
    return 0;
}

static int create_pipeline(ap_canvas *canvas)
{
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(canvas_push),
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &canvas->dsl,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    if (vkCreatePipelineLayout(canvas->gpu->device, &plci, NULL, &canvas->pl) != VK_SUCCESS) {
        AP_ERROR("canvas: pipeline layout failed");
        return -1;
    }

    VkShaderModuleCreateInfo vsci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = canvas_vert_spv_size,
        .pCode    = canvas_vert_spv,
    };
    VkShaderModule vert = VK_NULL_HANDLE;
    if (vkCreateShaderModule(canvas->gpu->device, &vsci, NULL, &vert) != VK_SUCCESS) {
        AP_ERROR("canvas: vertex shader module failed");
        return -1;
    }

    VkShaderModuleCreateInfo fsci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = canvas_frag_spv_size,
        .pCode    = canvas_frag_spv,
    };
    VkShaderModule frag = VK_NULL_HANDLE;
    if (vkCreateShaderModule(canvas->gpu->device, &fsci, NULL, &frag) != VK_SUCCESS) {
        AP_ERROR("canvas: fragment shader module failed");
        vkDestroyShaderModule(canvas->gpu->device, vert, NULL);
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
    VkPipelineRenderingCreateInfo rci = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &canvas->color_format,
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
        .layout              = canvas->pl,
    };

    VkResult r = vkCreateGraphicsPipelines(canvas->gpu->device, VK_NULL_HANDLE,
                                           1, &gpci, NULL, &canvas->pipeline);
    vkDestroyShaderModule(canvas->gpu->device, vert, NULL);
    vkDestroyShaderModule(canvas->gpu->device, frag, NULL);
    if (r != VK_SUCCESS) {
        AP_ERROR("canvas: vkCreateGraphicsPipelines -> %s", gpu_vk_result_str(r));
        return -1;
    }
    return 0;
}

ap_canvas *ap_canvas_create(ap_gpu *g)
{
    if (!g) {
        AP_ERROR("ap_canvas_create: invalid args");
        return NULL;
    }

    ap_canvas *canvas = calloc(1, sizeof(*canvas));
    if (!canvas) {
        AP_ERROR("ap_canvas_create: out of memory");
        return NULL;
    }
    canvas->gpu          = g;
    canvas->color_format = g->swapchain_format;
    canvas->zoom         = AP_CANVAS_DEFAULT_ZOOM;

    if (create_descriptor(canvas) < 0) goto fail;
    if (create_pipeline(canvas)   < 0) goto fail;

    return canvas;

fail:
    ap_canvas_destroy(canvas);
    return NULL;
}

void ap_canvas_destroy(ap_canvas *canvas)
{
    if (!canvas) return;
    VkDevice dev = canvas->gpu->device;

    if (canvas->pipeline) vkDestroyPipeline(dev, canvas->pipeline, NULL);
    if (canvas->pl)       vkDestroyPipelineLayout(dev, canvas->pl, NULL);
    if (canvas->pool)     vkDestroyDescriptorPool(dev, canvas->pool, NULL);
    if (canvas->dsl)      vkDestroyDescriptorSetLayout(dev, canvas->dsl, NULL);

    free(canvas);
}

void ap_canvas_set_input(ap_canvas *canvas,
                         VkImageView view, VkSampler sampler,
                         int image_width, int image_height)
{
    if (!canvas) return;

    if (!view || !sampler || image_width <= 0 || image_height <= 0) {
        canvas->has_input    = false;
        canvas->image_width  = 0;
        canvas->image_height = 0;
        return;
    }

    VkDescriptorImageInfo info = {
        .sampler     = sampler,
        .imageView   = view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = canvas->ds,
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &info,
    };
    vkUpdateDescriptorSets(canvas->gpu->device, 1, &write, 0, NULL);

    canvas->has_input    = true;
    canvas->image_width  = image_width;
    canvas->image_height = image_height;
    canvas->zoom         = AP_CANVAS_DEFAULT_ZOOM;
    canvas->pan_x        = 0.0f;
    canvas->pan_y        = 0.0f;
}

static float fit_scale_for(int img_w, int img_h, int win_w, int win_h)
{
    if (img_w <= 0 || img_h <= 0 || win_w <= 0 || win_h <= 0) {
        return 1.0f;
    }
    float sx = (float)win_w / (float)img_w;
    float sy = (float)win_h / (float)img_h;
    return sx < sy ? sx : sy;
}

void ap_canvas_reset_view(ap_canvas *canvas)
{
    if (!canvas) return;
    canvas->zoom  = AP_CANVAS_DEFAULT_ZOOM;
    canvas->pan_x = 0.0f;
    canvas->pan_y = 0.0f;
}

void ap_canvas_pan(ap_canvas *canvas, float dx, float dy)
{
    if (!canvas || !canvas->has_input) return;
    canvas->pan_x += dx;
    canvas->pan_y += dy;
}

void ap_canvas_zoom_at(ap_canvas *canvas, float factor,
                       float anchor_screen_x, float anchor_screen_y,
                       int win_width, int win_height)
{
    if (!canvas || !canvas->has_input || factor <= 0.0f) return;

    float new_zoom = canvas->zoom * factor;
    if (new_zoom < 0.05f) new_zoom = 0.05f;
    if (new_zoom > 64.0f) new_zoom = 64.0f;
    float real_factor = new_zoom / canvas->zoom;

    float cx = (float)win_width  * 0.5f;
    float cy = (float)win_height * 0.5f;
    float ax = anchor_screen_x - cx;
    float ay = anchor_screen_y - cy;

    canvas->pan_x = ax * (1.0f - real_factor) + real_factor * canvas->pan_x;
    canvas->pan_y = ay * (1.0f - real_factor) + real_factor * canvas->pan_y;
    canvas->zoom  = new_zoom;
}

void ap_canvas_set_zoom(ap_canvas *canvas, float zoom,
                        int win_width, int win_height)
{
    if (!canvas || !canvas->has_input || zoom <= 0.0f) return;
    float fs = fit_scale_for(canvas->image_width, canvas->image_height,
                             win_width, win_height);
    if (fs <= 0.0f) return;
    // Caller passes effective scale (e.g. 1.0 = 100%, 0 means fit).
    // Translate to internal user-zoom (relative to fit).
    canvas->zoom = zoom / fs;
    if (canvas->zoom < 0.05f) canvas->zoom = 0.05f;
    if (canvas->zoom > 64.0f) canvas->zoom = 64.0f;
}

float ap_canvas_zoom(const ap_canvas *canvas)
{
    return canvas ? canvas->zoom : 1.0f;
}

void ap_canvas_record(ap_canvas *canvas, VkCommandBuffer cmd,
                      int win_width, int win_height)
{
    if (!canvas || !canvas->has_input) return;
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

    canvas_push pc = {
        .image_size_px  = { (float)canvas->image_width, (float)canvas->image_height },
        .window_size_px = { (float)win_width, (float)win_height },
        .pan_px         = { canvas->pan_x, canvas->pan_y },
        .zoom           = canvas->zoom,
        .fit_scale      = fit_scale_for(canvas->image_width, canvas->image_height,
                                        win_width, win_height),
        .bg_color       = { 0.10f, 0.10f, 0.10f, 1.0f },
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, canvas->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, canvas->pl,
                            0, 1, &canvas->ds, 0, NULL);
    vkCmdPushConstants(cmd, canvas->pl, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}
