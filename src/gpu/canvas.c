#include "canvas.h"

#include "gpu_internal.h"

#include "core/log.h"
#include "edit/viewport.h"

#include "canvas_vert_spv.h"
#include "canvas_frag_spv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


typedef struct {
    float image_size_px[2];   // framed output size (after the viewport)
    float window_size_px[2];
    float pan_px[2];
    float zoom;
    float fit_scale;
    float bg_color[4];
    float crop_rect[4];       // x0, y0, x1, y1 (normalized)
    float vp_params[4];       // rotation_rad, flip_x, flip_y, autozoom
    float source_size_px[2];  // full rendered image size
    float scale[2];           // per-axis content stretch (in place)
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
    int          image_width;     // full rendered (source) image
    int          image_height;

    // Viewport — crop / rotation / flip / scale. The pipeline renders
    // the full frame; the canvas displays it through this transform.
    // Defaults to identity. Set by ap_canvas_set_viewport from the
    // photo's Transform module. See src/edit/viewport.h.
    ap_viewport  viewport;

    float zoom;
    float pan_x;
    float pan_y;

    // Sub-rect of the swapchain the canvas fits + renders within (the
    // dockspace central node). Zero size means "use the full window".
    int rect_x, rect_y, rect_w, rect_h;
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
    canvas->viewport     = ap_viewport_identity();

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
    // Note: view state (zoom/pan) is intentionally preserved across
    // input rebinds. Graph rebuilds on edit-stack changes go through
    // here every time, and the user's zoom/pan should not snap back
    // to the default on each edit. Call ap_canvas_reset_view at the
    // genuine "new photo" sites instead.
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

// Framed output dimensions the canvas displays — the viewport applied
// to the source image. Equals the full image for the identity viewport.
static void canvas_effective_size(const ap_canvas *canvas,
                                  int *out_w, int *out_h)
{
    ap_viewport_output_size(&canvas->viewport,
                            canvas->image_width, canvas->image_height,
                            out_w, out_h);
}

// Resolve the effective render rect: the stored sub-rect (the dockspace
// central node) when set, otherwise the full window. Mirrors ap_grid's
// effective_rect so the canvas fits beside the panels, not behind them.
static void canvas_effective_rect(const ap_canvas *canvas,
                                  int win_w, int win_h,
                                  int *rx, int *ry, int *rw, int *rh)
{
    if (canvas->rect_w > 0 && canvas->rect_h > 0) {
        *rx = canvas->rect_x;
        *ry = canvas->rect_y;
        *rw = canvas->rect_w;
        *rh = canvas->rect_h;
    } else {
        *rx = 0;
        *ry = 0;
        *rw = win_w;
        *rh = win_h;
    }
}

void ap_canvas_set_render_rect(ap_canvas *canvas, int x, int y, int w, int h)
{
    if (!canvas) return;
    if (w <= 0 || h <= 0) {
        canvas->rect_x = canvas->rect_y = 0;
        canvas->rect_w = canvas->rect_h = 0;
        return;
    }
    canvas->rect_x = x;
    canvas->rect_y = y;
    canvas->rect_w = w;
    canvas->rect_h = h;
}

void ap_canvas_set_viewport(ap_canvas *canvas, const ap_viewport *vp)
{
    if (!canvas) return;
    canvas->viewport = vp ? *vp : ap_viewport_identity();
}

void ap_canvas_reset_view(ap_canvas *canvas)
{
    if (!canvas) return;
    canvas->zoom  = AP_CANVAS_DEFAULT_ZOOM;
    canvas->pan_x = 0.0f;
    canvas->pan_y = 0.0f;
}

void ap_canvas_pan(ap_canvas *canvas, float dx, float dy,
                  int win_width, int win_height)
{
    if (!canvas || !canvas->has_input) return;

    canvas->pan_x += dx;
    canvas->pan_y += dy;

    if (win_width <= 0 || win_height <= 0) return;

    int rx, ry, rw, rh;
    canvas_effective_rect(canvas, win_width, win_height, &rx, &ry, &rw, &rh);
    (void)rx; (void)ry;

    int eff_w, eff_h;
    canvas_effective_size(canvas, &eff_w, &eff_h);
    float fs = fit_scale_for(eff_w, eff_h, rw, rh);
    if (fs <= 0.0f) return;

    // Displayed half-extents in screen pixels. At least a quarter of
    // the scaled image must remain on-screen: limit the pan magnitude
    // to (displayed_half - rect_quarter). When the image is smaller
    // than the rect in that axis the limit is zero, preventing any
    // pan at all (the image is already fully visible).
    float displayed_half_x = (float)eff_w * fs * canvas->zoom * 0.5f;
    float displayed_half_y = (float)eff_h * fs * canvas->zoom * 0.5f;
    float win_quarter_x    = (float)rw * 0.25f;
    float win_quarter_y    = (float)rh * 0.25f;

    float limit_x = displayed_half_x - win_quarter_x;
    float limit_y = displayed_half_y - win_quarter_y;
    if (limit_x < 0.0f) limit_x = 0.0f;
    if (limit_y < 0.0f) limit_y = 0.0f;

    if (canvas->pan_x >  limit_x) canvas->pan_x =  limit_x;
    if (canvas->pan_x < -limit_x) canvas->pan_x = -limit_x;
    if (canvas->pan_y >  limit_y) canvas->pan_y =  limit_y;
    if (canvas->pan_y < -limit_y) canvas->pan_y = -limit_y;
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

    int rx, ry, rw, rh;
    canvas_effective_rect(canvas, win_width, win_height, &rx, &ry, &rw, &rh);
    float cx = (float)rx + (float)rw * 0.5f;
    float cy = (float)ry + (float)rh * 0.5f;
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
    int rx, ry, rw, rh;
    canvas_effective_rect(canvas, win_width, win_height, &rx, &ry, &rw, &rh);
    (void)rx; (void)ry;
    int eff_w, eff_h;
    canvas_effective_size(canvas, &eff_w, &eff_h);
    float fs = fit_scale_for(eff_w, eff_h, rw, rh);
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

float ap_canvas_effective_scale(const ap_canvas *canvas,
                                int win_width, int win_height)
{
    if (!canvas || !canvas->has_input) return 0.0f;
    int rx, ry, rw, rh;
    canvas_effective_rect(canvas, win_width, win_height, &rx, &ry, &rw, &rh);
    (void)rx; (void)ry;
    int eff_w, eff_h;
    canvas_effective_size(canvas, &eff_w, &eff_h);
    float fs = fit_scale_for(eff_w, eff_h, rw, rh);
    return fs * canvas->zoom;
}

void ap_canvas_record(ap_canvas *canvas, VkCommandBuffer cmd,
                      int win_width, int win_height)
{
    if (!canvas || !canvas->has_input) return;
    if (win_width <= 0 || win_height <= 0) return;

    int rx, ry, rw, rh;
    canvas_effective_rect(canvas, win_width, win_height, &rx, &ry, &rw, &rh);

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

    int eff_w, eff_h;
    canvas_effective_size(canvas, &eff_w, &eff_h);

    const ap_viewport *vp = &canvas->viewport;
    float autozoom = ap_viewport_autozoom(vp, canvas->image_width,
                                          canvas->image_height);

    canvas_push pc = {
        .image_size_px  = { (float)eff_w, (float)eff_h },
        .window_size_px = { (float)rw, (float)rh },
        .pan_px         = { canvas->pan_x, canvas->pan_y },
        .zoom           = canvas->zoom,
        .fit_scale      = fit_scale_for(eff_w, eff_h, rw, rh),
        .bg_color       = { 0.10f, 0.10f, 0.10f, 1.0f },
        .crop_rect      = { vp->crop_x0, vp->crop_y0,
                            vp->crop_x1, vp->crop_y1 },
        .vp_params      = { vp->rotation_deg * (float)(M_PI / 180.0),
                            vp->flip_x ? 1.0f : 0.0f,
                            vp->flip_y ? 1.0f : 0.0f,
                            autozoom },
        .source_size_px = { (float)canvas->image_width,
                            (float)canvas->image_height },
        .scale          = { vp->scale_x, vp->scale_y },
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, canvas->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, canvas->pl,
                            0, 1, &canvas->ds, 0, NULL);
    vkCmdPushConstants(cmd, canvas->pl, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}
