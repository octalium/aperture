#include "pipeline_graph_priv.h"

#define WORKGROUP_SIZE 16
#define MAX_MODULES    16

// Max long edge of the GPU-side thumb image. The close path blits the
// rendered output into here (linear filter, basically free on the GPU)
// and reads back from the small image instead of the full-resolution
// display image. Kept in line with thumbnail.c's CPU downsample target
// so the JPEG encoder's CPU downsample step becomes a no-op copy.
#define THUMB_MAX_EDGE 384

int graph_create_image(VkDevice device, VkPhysicalDevice physical,
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
    int mt = gpu_find_memory_type(physical, mreq.memoryTypeBits,
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
    VkResult ar = vkAllocateMemory(device, &mai, NULL, out_memory);
    if (ar != VK_SUCCESS) {
        AP_ERROR("graph: image memory alloc failed (%s, size=%llu, fmt=%d, %dx%d)",
                 gpu_vk_result_str(ar), (unsigned long long)mreq.size,
                 (int)format, width, height);
        return -1;
    }
    vkBindImageMemory(device, *out_image, *out_memory, 0);
    return 0;
}

int graph_create_view(VkDevice device, VkImage image, VkFormat format,
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
    if (graph_create_image(graph->gpu->device, graph->gpu->physical, width, height,
                     VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT,
                     0, NULL, 0,
                     &graph->stage_a_image, &graph->stage_a_memory) < 0) return -1;
    if (graph_create_view(graph->gpu->device, graph->stage_a_image,
                    VK_FORMAT_R16G16B16A16_SFLOAT, 0,
                    &graph->stage_a_view) < 0) return -1;

    if (graph_create_image(graph->gpu->device, graph->gpu->physical, width, height,
                     VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT,
                     0, NULL, 0,
                     &graph->stage_b_image, &graph->stage_b_memory) < 0) return -1;
    if (graph_create_view(graph->gpu->device, graph->stage_b_image,
                    VK_FORMAT_R16G16B16A16_SFLOAT, 0,
                    &graph->stage_b_view) < 0) return -1;

    // Scratch buffers for multi-pass modules (graph->scratch_count set
    // by the caller before create_buffers). Same RGBA16F format as the
    // ping-pong working buffers.
    for (int i = 0; i < graph->scratch_count; i++) {
        if (graph_create_image(graph->gpu->device, graph->gpu->physical, width, height,
                         VK_FORMAT_R16G16B16A16_SFLOAT,
                         VK_IMAGE_USAGE_STORAGE_BIT,
                         0, NULL, 0,
                         &graph->scratch_image[i],
                         &graph->scratch_memory[i]) < 0) return -1;
        if (graph_create_view(graph->gpu->device, graph->scratch_image[i],
                        VK_FORMAT_R16G16B16A16_SFLOAT, 0,
                        &graph->scratch_view[i]) < 0) return -1;
    }

    VkFormat display_view_formats[2] = {
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
    };
    if (graph_create_image(graph->gpu->device, graph->gpu->physical, width, height,
                     VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_STORAGE_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                     display_view_formats, 2,
                     &graph->display_image, &graph->display_memory) < 0) return -1;
    if (graph_create_view(graph->gpu->device, graph->display_image,
                    VK_FORMAT_R8G8B8A8_UNORM, 0,
                    &graph->display_view_unorm) < 0) return -1;
    if (graph_create_view(graph->gpu->device, graph->display_image,
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

    // Aspect-preserving downscale to THUMB_MAX_EDGE on the long side.
    // If the rendered output is already smaller, just use its dims so
    // the blit doesn't upscale. Always at least 1x1 for safety.
    int longest = width > height ? width : height;
    if (longest <= THUMB_MAX_EDGE) {
        graph->thumb_width  = width;
        graph->thumb_height = height;
    } else {
        double s = (double)THUMB_MAX_EDGE / (double)longest;
        graph->thumb_width  = (int)(width  * s + 0.5);
        graph->thumb_height = (int)(height * s + 0.5);
    }
    if (graph->thumb_width  < 1) graph->thumb_width  = 1;
    if (graph->thumb_height < 1) graph->thumb_height = 1;

    if (graph_create_image(graph->gpu->device, graph->gpu->physical,
                     graph->thumb_width, graph->thumb_height,
                     VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     0, NULL, 0,
                     &graph->thumb_image, &graph->thumb_memory) < 0) return -1;
    return 0;
}

static int create_descriptor_pool(ap_pipeline_graph *graph, int stage_count)
{
    // Three storage-image bindings per stage (read0, write, aux read).
    VkDescriptorPoolSize pool_size = {
        .type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = (uint32_t)(stage_count * 3),
    };
    VkDescriptorPoolCreateInfo pci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = (uint32_t)stage_count,
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

// Build the Vulkan state for one stage. The stage's module / push /
// pack_push and its three image views must already be filled in by
// the chain assembly. Binding layout: 0 = read0, 1 = write,
// 2 = read1 (aux). A single-pass stage sets read1 == read0.
static int create_stage(ap_pipeline_graph *graph, graph_stage *st,
                        const uint32_t *spv_data, size_t spv_size)
{
    const char *name = st->module ? st->module->name : "?";

    VkDescriptorSetLayoutBinding bindings[3] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3,
        .pBindings    = bindings,
    };
    if (vkCreateDescriptorSetLayout(graph->gpu->device, &dslci, NULL, &st->dsl) != VK_SUCCESS) {
        AP_ERROR("graph: %s: descriptor set layout failed", name);
        return -1;
    }

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = (uint32_t)st->push_size,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &st->dsl,
        .pushConstantRangeCount = st->push_size > 0 ? 1u : 0u,
        .pPushConstantRanges    = st->push_size > 0 ? &push : NULL,
    };
    if (vkCreatePipelineLayout(graph->gpu->device, &plci, NULL, &st->pl) != VK_SUCCESS) {
        AP_ERROR("graph: %s: pipeline layout failed", name);
        return -1;
    }

    VkShaderModuleCreateInfo smci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spv_size,
        .pCode    = spv_data,
    };
    VkShaderModule sm;
    if (vkCreateShaderModule(graph->gpu->device, &smci, NULL, &sm) != VK_SUCCESS) {
        AP_ERROR("graph: %s: shader module failed", name);
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
        AP_ERROR("graph: %s: vkCreateComputePipelines -> %d", name, r);
        return -1;
    }

    VkDescriptorSetAllocateInfo dai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = graph->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &st->dsl,
    };
    if (vkAllocateDescriptorSets(graph->gpu->device, &dai, &st->ds) != VK_SUCCESS) {
        AP_ERROR("graph: %s: descriptor set alloc failed", name);
        return -1;
    }

    VkDescriptorImageInfo r0 = { .imageView = st->read0_view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo w  = { .imageView = st->write_view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo r1 = { .imageView = st->read1_view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet writes[3] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = st->ds, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &r0 },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = st->ds, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &w  },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = st->ds, .dstBinding = 2,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &r1 },
    };
    vkUpdateDescriptorSets(graph->gpu->device, 3, writes, 0, NULL);
    return 0;
}

// Resolve a pass buffer reference to a (view, image) pair, given the
// module's IN / OUT buffers and the graph's scratch pool.
static void resolve_buf(ap_pipeline_graph *graph, ap_pass_buf b,
                        VkImageView in_view,
                        VkImageView out_view, VkImage out_image,
                        VkImageView *res_view, VkImage *res_image)
{
    if (b == AP_PASS_BUF_IN) {
        *res_view  = in_view;
        *res_image = VK_NULL_HANDLE;   // IN is only ever read
    } else if (b == AP_PASS_BUF_OUT) {
        *res_view  = out_view;
        *res_image = out_image;
    } else {
        int n = (int)b - (int)AP_PASS_BUF_SCRATCH0;
        if (n >= 0 && n < graph->scratch_count) {
            *res_view  = graph->scratch_view[n];
            *res_image = graph->scratch_image[n];
        } else {
            *res_view  = out_view;     // defensive — module mis-declared
            *res_image = out_image;
        }
    }
}

// Walk the resolved chain, expanding multi-pass variants into one
// stage per pass, assigning each stage its IN / OUT / scratch buffers,
// and building the Vulkan state. The ping-pong of stage_a / stage_b
// across modules is unchanged; multi-pass modules additionally route
// through the scratch pool. Contract: a multi-pass variant's final
// pass must write AP_PASS_BUF_OUT; passes never write IN.
static int assemble_stages(ap_pipeline_graph *graph,
                           const ap_module *const *chain_modules,
                           const int *chain_entry_idx,
                           const ap_module_active *chain_active,
                           const bool *chain_skip,
                           int chain_len, VkImageView input_view,
                           const ap_edit_stack *stack)
{
    VkImageView cur_view  = input_view;
    VkImage     cur_image = VK_NULL_HANDLE; // unknown for the raw input
    int s = 0;

    for (int c = 0; c < chain_len; c++) {
        const ap_module        *m = chain_modules[c];
        const ap_module_active *a = &chain_active[c];
        bool last_module = (c == chain_len - 1);
        bool stage_skip  = chain_skip[c];

        VkImageView out_view;
        VkImage     out_image;
        if (last_module) {
            out_view  = graph->display_view_unorm;
            out_image = graph->display_image;
        } else if (cur_view == graph->stage_a_view) {
            out_view  = graph->stage_b_view;
            out_image = graph->stage_b_image;
        } else {
            out_view  = graph->stage_a_view;
            out_image = graph->stage_a_image;
        }

        int n_passes = (a->pass_count > 0) ? a->pass_count : 1;
        if (s + n_passes > MAX_STAGES) {
            AP_ERROR("graph: stage count exceeds MAX_STAGES");
            return -1;
        }

        for (int p = 0; p < n_passes; p++) {
            graph_stage *st = &graph->stages[s];
            st->module    = m;
            st->entry_idx = chain_entry_idx[c];
            st->skip      = stage_skip;
            st->is_last_pass   = (p == n_passes - 1);
            st->module_in_image = cur_image;

            const uint32_t *spv;
            size_t spv_size;
            if (a->pass_count > 0) {
                const ap_module_pass *pass = &a->passes[p];
                st->push_size = pass->push_size;
                st->pack_push = pass->pack_push;
                spv      = pass->spv_data;
                spv_size = pass->spv_size;
                VkImage ignore;
                resolve_buf(graph, pass->read0, cur_view, out_view,
                            out_image, &st->read0_view, &ignore);
                resolve_buf(graph, pass->read1, cur_view, out_view,
                            out_image, &st->read1_view, &ignore);
                resolve_buf(graph, pass->write, cur_view, out_view,
                            out_image, &st->write_view, &st->write_image);
            } else {
                st->push_size  = a->push_size;
                st->pack_push  = a->pack_push;
                spv            = a->spv_data;
                spv_size       = a->spv_size;
                st->read0_view = cur_view;
                st->read1_view = cur_view;   // single-pass: aux unused
                st->write_view = out_view;
                st->write_image = out_image;

                // A LUT variant routes a baked colour LUT into the aux
                // binding instead of the (unused) duplicate of read0.
                if (a->bake_lut) {
                    if (build_stage_lut(graph, st, a, stack) < 0) {
                        return -1;
                    }
                    st->read1_view = st->lut_view;
                }
            }

            if (create_stage(graph, st, spv, spv_size) < 0) return -1;
            s++;
        }
        cur_view  = out_view;
        cur_image = out_image;
    }
    graph->stage_count = s;
    return 0;
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

    VkImage targets[3 + AP_MODULE_MAX_SCRATCH] = {
        graph->stage_a_image, graph->stage_b_image, graph->display_image,
    };
    int target_count = 3;
    for (int i = 0; i < graph->scratch_count; i++) {
        targets[target_count++] = graph->scratch_image[i];
    }
    VkImageMemoryBarrier2 barriers[3 + AP_MODULE_MAX_SCRATCH];
    for (int i = 0; i < target_count; i++) {
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
        .imageMemoryBarrierCount = (uint32_t)target_count,
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
                                            const ap_edit_stack *stack,
                                            const ap_raw_metadata *meta)
{
    if (!g || !input || output_width <= 0 || output_height <= 0) {
        AP_ERROR("ap_pipeline_graph_create: invalid args");
        return NULL;
    }

    // Only Output Transfer is auto-appended by the graph; every other
    // module (including Demosaic and Color) is a regular user-visible
    // entry on the stack. Output Transfer's job (linear -> sRGB EOTF)
    // is a hard requirement of the display surface, so it stays as
    // transport.
    //
    // The downstream modules (Color, Exposure, Tone, Output Transfer)
    // all expect an RGBA16F input. The graph's source texture is R16
    // (Bayer). So *something* has to perform the R16 -> RGBA16F step
    // at the head of the chain. Normally Demosaic does that. When the
    // user has disabled / removed Demosaic, we insert
    // `raw_passthrough` — a grayscale R16 -> RGBA16F module — instead.
    const ap_module *m_out  = ap_module_find("output_transfer");
    const ap_module *m_pass = ap_module_find("raw_passthrough");
    if (!m_out || !m_pass) {
        AP_ERROR("ap_pipeline_graph_create: transport modules missing");
        return NULL;
    }

    const ap_module *chain_modules[MAX_MODULES];
    int              chain_entry_idx[MAX_MODULES];
    bool             chain_skip[MAX_MODULES];
    int              chain_len = 0;

    // Does the stack have any enabled Demosaic entry? If not, we'll
    // prepend the raw passthrough so format dimensions line up.
    // Disabled demosaic entries also count as absent here: demosaic
    // reads from the raw R16 input texture and its skip path would
    // need a format-converting copy, so disabled demosaic falls back
    // to raw_passthrough exactly as if it were removed.
    bool need_passthrough = true;
    int  n = stack ? ap_edit_stack_count(stack) : 0;
    for (int i = 0; i < n; i++) {
        const ap_edit_entry *e = ap_edit_stack_at_const(stack, i);
        if (e && e->enabled && strcmp(e->module_name, "demosaic") == 0) {
            need_passthrough = false;
            break;
        }
    }
    if (need_passthrough) {
        chain_modules[chain_len]   = m_pass;
        chain_entry_idx[chain_len] = -1;
        chain_skip[chain_len]      = false;
        chain_len++;
    }

    for (int i = 0; i < n; i++) {
        const ap_edit_entry *e = ap_edit_stack_at_const(stack, i);
        if (!e) continue;
        const ap_module *m = ap_module_find(e->module_name);
        if (!m) {
            AP_ERROR("ap_pipeline_graph_create: unknown module '%s' at stack[%d]",
                     e->module_name, i);
            return NULL;
        }
        // Metadata-only modules (e.g. Transform — a framing operation
        // the canvas + export consume, not a pixel stage) carry no
        // shader. They occupy a stack slot but produce no stage.
        if (!m->spv_data && (m->variant_count == 0 || !m->variants)) {
            continue;
        }
        // Demosaic reads from the raw R16 input texture. Skipping it
        // via a same-format copy is not possible; treat disabled demosaic
        // entries as absent and rely on the raw_passthrough inserted above.
        if (!e->enabled && strcmp(e->module_name, "demosaic") == 0) {
            continue;
        }
        if (chain_len >= MAX_MODULES - 1) {
            AP_ERROR("ap_pipeline_graph_create: stack exceeds MAX_MODULES");
            return NULL;
        }
        chain_modules[chain_len]   = m;
        chain_entry_idx[chain_len] = i;
        chain_skip[chain_len]      = !e->enabled;
        chain_len++;
    }

    chain_modules[chain_len]   = m_out;
    chain_entry_idx[chain_len] = -1;
    chain_skip[chain_len]      = false;
    chain_len++;

    // Resolve each chain module's active variant up front. Variant
    // choice (incl. whether a variant is multi-pass) is baked into the
    // graph at build time; variant changes trigger a rebuild. From the
    // resolved actives, total the expanded stage count + the scratch
    // pool size a multi-pass variant needs.
    ap_module_active chain_active[MAX_MODULES];
    int total_stages = 0;
    int max_scratch  = 0;
    for (int c = 0; c < chain_len; c++) {
        const float *params = NULL;
        if (chain_entry_idx[c] >= 0 && stack) {
            const ap_edit_entry *e =
                ap_edit_stack_at_const(stack, chain_entry_idx[c]);
            if (e) params = e->params;
        }
        ap_module_resolve(chain_modules[c], params, &chain_active[c]);
        total_stages += (chain_active[c].pass_count > 0)
                      ? chain_active[c].pass_count : 1;
        if (chain_active[c].scratch_count > max_scratch) {
            max_scratch = chain_active[c].scratch_count;
        }
    }
    if (total_stages > MAX_STAGES) {
        AP_ERROR("ap_pipeline_graph_create: %d stages exceeds MAX_STAGES",
                 total_stages);
        return NULL;
    }
    if (max_scratch > AP_MODULE_MAX_SCRATCH) max_scratch = AP_MODULE_MAX_SCRATCH;

    ap_pipeline_graph *graph = calloc(1, sizeof(*graph));
    if (!graph) {
        AP_ERROR("ap_pipeline_graph_create: out of memory");
        return NULL;
    }
    graph->gpu           = g;
    graph->width         = output_width;
    graph->height        = output_height;
    graph->scratch_count = max_scratch;
    if (meta) {
        graph->meta     = *meta;
        graph->has_meta = true;
    }

    if (create_buffers(graph, graph->width, graph->height)   < 0) goto fail;
    if (create_descriptor_pool(graph, total_stages)          < 0) goto fail;
    if (assemble_stages(graph, chain_modules, chain_entry_idx,
                        chain_active, chain_skip, chain_len,
                        ap_texture_view(input), stack)       < 0) goto fail;
    if (initial_layout_transitions(graph)                    < 0) goto fail;

    return graph;

fail:
    ap_pipeline_graph_destroy(graph);
    return NULL;
}

void ap_pipeline_graph_destroy(ap_pipeline_graph *graph)
{
    if (!graph) return;
    VkDevice dev = graph->gpu->device;

    if (graph->thumb_image)        vkDestroyImage(dev, graph->thumb_image, NULL);
    if (graph->thumb_memory)       vkFreeMemory(dev, graph->thumb_memory, NULL);

    if (graph->display_sampler)    vkDestroySampler(dev, graph->display_sampler, NULL);
    if (graph->display_view_srgb)  vkDestroyImageView(dev, graph->display_view_srgb, NULL);
    if (graph->display_view_unorm) vkDestroyImageView(dev, graph->display_view_unorm, NULL);
    if (graph->display_image)      vkDestroyImage(dev, graph->display_image, NULL);
    if (graph->display_memory)     vkFreeMemory(dev, graph->display_memory, NULL);

    for (int i = 0; i < AP_MODULE_MAX_SCRATCH; i++) {
        if (graph->scratch_view[i])   vkDestroyImageView(dev, graph->scratch_view[i], NULL);
        if (graph->scratch_image[i])  vkDestroyImage(dev, graph->scratch_image[i], NULL);
        if (graph->scratch_memory[i]) vkFreeMemory(dev, graph->scratch_memory[i], NULL);
    }

    if (graph->stage_b_view)   vkDestroyImageView(dev, graph->stage_b_view, NULL);
    if (graph->stage_b_image)  vkDestroyImage(dev, graph->stage_b_image, NULL);
    if (graph->stage_b_memory) vkFreeMemory(dev, graph->stage_b_memory, NULL);

    if (graph->stage_a_view)   vkDestroyImageView(dev, graph->stage_a_view, NULL);
    if (graph->stage_a_image)  vkDestroyImage(dev, graph->stage_a_image, NULL);
    if (graph->stage_a_memory) vkFreeMemory(dev, graph->stage_a_memory, NULL);

    for (int i = 0; i < graph->stage_count; i++) {
        graph_stage *st = &graph->stages[i];
        if (st->pipeline)   vkDestroyPipeline(dev, st->pipeline, NULL);
        if (st->pl)         vkDestroyPipelineLayout(dev, st->pl, NULL);
        if (st->dsl)        vkDestroyDescriptorSetLayout(dev, st->dsl, NULL);
        if (st->lut_view)   vkDestroyImageView(dev, st->lut_view, NULL);
        if (st->lut_image)  vkDestroyImage(dev, st->lut_image, NULL);
        if (st->lut_memory) vkFreeMemory(dev, st->lut_memory, NULL);
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

// True when two edit stacks would produce the same pixel-pipeline
// output: same modules in the same order carrying the same parameters.
// The enabled flag is deliberately excluded: enable/disable toggles
// now update the stage skip flag via ap_pipeline_graph_set_stage_skip,
// which resets has_recorded so the next record call re-dispatches.
// Focus, display name and config-window visibility are UI-only and
// ignored for the same reason.
static bool stack_render_equal(const ap_edit_stack *a,
                               const ap_edit_stack *b)
{
    if (a->count != b->count) {
        return false;
    }
    for (int i = 0; i < a->count; i++) {
        const ap_edit_entry *ea = &a->entries[i];
        const ap_edit_entry *eb = &b->entries[i];
        if (strcmp(ea->module_name, eb->module_name) != 0) {
            return false;
        }
        if (memcmp(ea->params, eb->params, sizeof(ea->params)) != 0) {
            return false;
        }
        if (memcmp(ea->str_params, eb->str_params,
                   sizeof(ea->str_params)) != 0) {
            return false;
        }
    }
    return true;
}

int ap_pipeline_graph_record(ap_pipeline_graph *graph, VkCommandBuffer cmd,
                             const ap_edit_stack *stack)
{
    if (!graph || graph->stage_count == 0) {
        return 0;
    }

    // Skip the compute pass when nothing that feeds it changed. The
    // display image from the last dispatch is still valid — the canvas
    // keeps sampling it — so an unchanged edit stack costs no GPU work.
    // Viewport / crop changes are display-side and never reach here.
    // Enable/disable toggles are handled via ap_pipeline_graph_set_stage_skip
    // which resets has_recorded, so they always force a re-record.
    if (graph->has_recorded && stack &&
        stack_render_equal(&graph->record_snapshot, stack)) {
        return 0;
    }
    if (stack) {
        graph->record_snapshot = *stack;
    }
    graph->has_recorded = true;

    uint32_t gx = (uint32_t)((graph->width  + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE);
    uint32_t gy = (uint32_t)((graph->height + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE);

    // Push-constant scratch buffer; modules pack into here. 256 bytes is
    // the conservative Vulkan-spec minimum for max push constant size.
    uint8_t push_buf[256];

    for (int i = 0; i < graph->stage_count; i++) {
        graph_stage *st = &graph->stages[i];
        const ap_module *m = st->module;
        bool is_last = (i == graph->stage_count - 1);

        if (st->skip) {
            // Skipped stage: the shader is not dispatched. For the last
            // pass of a module, copy the module's input image straight
            // through to the output image so the next stage's read sees
            // valid data. Intermediate passes of a multi-pass module are
            // silently dropped — they only write to module-private scratch
            // buffers that nobody reads when the module is skipped.
            if (st->is_last_pass && st->module_in_image != VK_NULL_HANDLE) {
                VkImageMemoryBarrier2 pre = {
                    .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    .dstStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
                    .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                    .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = st->module_in_image,
                    .subresourceRange = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel = 0, .levelCount = 1,
                        .baseArrayLayer = 0, .layerCount = 1,
                    },
                };
                VkImageMemoryBarrier2 pre_dst = {
                    .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                                   | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    .dstStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
                    .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = st->write_image,
                    .subresourceRange = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel = 0, .levelCount = 1,
                        .baseArrayLayer = 0, .layerCount = 1,
                    },
                };
                VkImageMemoryBarrier2 pre_barriers[2] = { pre, pre_dst };
                VkDependencyInfo dep_pre = {
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .imageMemoryBarrierCount = 2,
                    .pImageMemoryBarriers    = pre_barriers,
                };
                vkCmdPipelineBarrier2(cmd, &dep_pre);

                VkImageCopy region = {
                    .srcSubresource = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
                    },
                    .srcOffset = { 0, 0, 0 },
                    .dstSubresource = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
                    },
                    .dstOffset = { 0, 0, 0 },
                    .extent    = { (uint32_t)graph->width,
                                   (uint32_t)graph->height, 1 },
                };
                vkCmdCopyImage(cmd,
                               st->module_in_image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               st->write_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &region);

                VkImageMemoryBarrier2 post_src = {
                    .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
                    .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                    .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                    .oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = st->module_in_image,
                    .subresourceRange = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel = 0, .levelCount = 1,
                        .baseArrayLayer = 0, .layerCount = 1,
                    },
                };
                VkImageMemoryBarrier2 post_dst;
                VkDependencyInfo dep_post;
                if (is_last) {
                    post_dst = (VkImageMemoryBarrier2){
                        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                        .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
                        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        .dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        .oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = st->write_image,
                        .subresourceRange = {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .baseMipLevel = 0, .levelCount = 1,
                            .baseArrayLayer = 0, .layerCount = 1,
                        },
                    };
                } else {
                    post_dst = (VkImageMemoryBarrier2){
                        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                        .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
                        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                        .oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = st->write_image,
                        .subresourceRange = {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .baseMipLevel = 0, .levelCount = 1,
                            .baseArrayLayer = 0, .layerCount = 1,
                        },
                    };
                }
                VkImageMemoryBarrier2 post_barriers[2] = { post_src, post_dst };
                dep_post = (VkDependencyInfo){
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .imageMemoryBarrierCount = 2,
                    .pImageMemoryBarriers    = post_barriers,
                };
                vkCmdPipelineBarrier2(cmd, &dep_post);
            }
            continue;
        }

        if (st->push_size > sizeof(push_buf)) {
            AP_ERROR("graph: %s push_size %zu exceeds scratch %zu",
                     m ? m->name : "?", st->push_size, sizeof(push_buf));
            return -1;
        }

        if (st->pack_push) {
            const ap_raw_metadata *meta = graph->has_meta ? &graph->meta : NULL;
            const float *params = NULL;
            const char (*str_params)[AP_EDIT_STR_LEN] = NULL;
            if (st->entry_idx >= 0 && stack) {
                const ap_edit_entry *e = ap_edit_stack_at_const(stack, st->entry_idx);
                if (e) { params = e->params; str_params = e->str_params; }
            }
            int rc = st->pack_push(m, params, str_params, meta, push_buf);
            if (rc != 0) {
                continue; // module signaled "skip"
            }
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, st->pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, st->pl,
                                0, 1, &st->ds, 0, NULL);
        if (st->push_size > 0) {
            vkCmdPushConstants(cmd, st->pl, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, (uint32_t)st->push_size, push_buf);
        }
        vkCmdDispatch(cmd, gx, gy, 1);

        if (is_last) {
            compute_to_sample_barrier(cmd, st->write_image);
        } else {
            compute_to_compute_barrier(cmd, st->write_image);
        }
    }
    return 0;
}

int ap_pipeline_graph_set_stage_skip(ap_pipeline_graph *graph,
                                     int entry_idx, bool skip)
{
    if (!graph) return -1;
    for (int i = 0; i < graph->stage_count; i++) {
        if (graph->stages[i].entry_idx == entry_idx) {
            // Update all stages that belong to this entry (multi-pass
            // modules expand into multiple consecutive stages).
            while (i < graph->stage_count &&
                   graph->stages[i].entry_idx == entry_idx) {
                graph->stages[i].skip = skip;
                i++;
            }
            graph->has_recorded = false;
            return 0;
        }
    }
    return -1;
}

VkImageView   ap_pipeline_graph_output_view(const ap_pipeline_graph *g)    { return g->display_view_srgb; }
VkSampler     ap_pipeline_graph_output_sampler(const ap_pipeline_graph *g) { return g->display_sampler; }
VkImageLayout ap_pipeline_graph_output_layout(const ap_pipeline_graph *g)  { (void)g; return VK_IMAGE_LAYOUT_GENERAL; }
int           ap_pipeline_graph_output_width(const ap_pipeline_graph *g)   { return g->width; }
int           ap_pipeline_graph_output_height(const ap_pipeline_graph *g)  { return g->height; }

int ap_pipeline_graph_readback(ap_pipeline_graph *graph,
                               void *out_pixels, size_t out_size)
{
    if (!graph || !out_pixels) {
        AP_ERROR("readback: invalid args");
        return -1;
    }
    size_t needed = (size_t)graph->width * (size_t)graph->height * 4u;
    if (out_size < needed) {
        AP_ERROR("readback: out_size %zu < required %zu", out_size, needed);
        return -1;
    }

    vkDeviceWaitIdle(graph->gpu->device);

    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = needed,
        .usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer       staging  = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    int rc = -1;

    if (vkCreateBuffer(graph->gpu->device, &bci, NULL, &staging) != VK_SUCCESS) {
        AP_ERROR("readback: vkCreateBuffer failed");
        goto out;
    }

    VkMemoryRequirements mreq;
    vkGetBufferMemoryRequirements(graph->gpu->device, staging, &mreq);
    int mt = gpu_find_memory_type(graph->gpu->physical, mreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) {
        AP_ERROR("readback: no host-visible memory type");
        goto out;
    }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mreq.size,
        .memoryTypeIndex = (uint32_t)mt,
    };
    if (vkAllocateMemory(graph->gpu->device, &mai, NULL, &staging_mem) != VK_SUCCESS) {
        AP_ERROR("readback: vkAllocateMemory failed");
        goto out;
    }
    vkBindBufferMemory(graph->gpu->device, staging, staging_mem, 0);

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

    VkImageMemoryBarrier2 to_src = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                       | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = graph->display_image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    VkDependencyInfo dep_to_src = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &to_src,
    };
    vkCmdPipelineBarrier2(cmd, &dep_to_src);

    VkBufferImageCopy region = {
        .imageSubresource = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel       = 0,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
        .imageExtent = { (uint32_t)graph->width, (uint32_t)graph->height, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, graph->display_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging, 1, &region);

    VkImageMemoryBarrier2 back = to_src;
    back.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
    back.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    back.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    back.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                       | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    back.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    VkDependencyInfo dep_back = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &back,
    };
    vkCmdPipelineBarrier2(cmd, &dep_back);

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

    void *map = NULL;
    if (vkMapMemory(graph->gpu->device, staging_mem, 0, needed, 0, &map) != VK_SUCCESS) {
        AP_ERROR("readback: vkMapMemory failed");
        goto out;
    }
    memcpy(out_pixels, map, needed);
    vkUnmapMemory(graph->gpu->device, staging_mem);
    rc = 0;

out:
    if (staging_mem) vkFreeMemory(graph->gpu->device, staging_mem, NULL);
    if (staging)    vkDestroyBuffer(graph->gpu->device, staging, NULL);
    return rc;
}

int ap_pipeline_graph_thumb_width(const ap_pipeline_graph *g)  { return g->thumb_width; }
int ap_pipeline_graph_thumb_height(const ap_pipeline_graph *g) { return g->thumb_height; }

int ap_pipeline_graph_readback_thumb(ap_pipeline_graph *graph,
                                     void *out_pixels, size_t out_size,
                                     int *out_w, int *out_h)
{
    if (!graph || !out_pixels || !out_w || !out_h) {
        AP_ERROR("readback_thumb: invalid args");
        return -1;
    }
    size_t needed = (size_t)graph->thumb_width
                  * (size_t)graph->thumb_height * 4u;
    if (out_size < needed) {
        AP_ERROR("readback_thumb: out_size %zu < required %zu",
                 out_size, needed);
        return -1;
    }

    vkDeviceWaitIdle(graph->gpu->device);

    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = needed,
        .usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer       staging     = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    int rc = -1;

    if (vkCreateBuffer(graph->gpu->device, &bci, NULL, &staging) != VK_SUCCESS) {
        AP_ERROR("readback_thumb: vkCreateBuffer failed");
        goto out;
    }

    VkMemoryRequirements mreq;
    vkGetBufferMemoryRequirements(graph->gpu->device, staging, &mreq);
    int mt = gpu_find_memory_type(graph->gpu->physical, mreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) {
        AP_ERROR("readback_thumb: no host-visible memory type");
        goto out;
    }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mreq.size,
        .memoryTypeIndex = (uint32_t)mt,
    };
    if (vkAllocateMemory(graph->gpu->device, &mai, NULL, &staging_mem) != VK_SUCCESS) {
        AP_ERROR("readback_thumb: vkAllocateMemory failed");
        goto out;
    }
    vkBindBufferMemory(graph->gpu->device, staging, staging_mem, 0);

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

    // display: GENERAL -> TRANSFER_SRC_OPTIMAL (read for blit).
    // thumb:   UNDEFINED -> TRANSFER_DST_OPTIMAL (written by blit).
    VkImageMemoryBarrier2 pre[2] = {
        {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                           | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = graph->display_image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        },
        {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .srcAccessMask = 0,
            .dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = graph->thumb_image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        },
    };
    VkDependencyInfo dep_pre = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 2,
        .pImageMemoryBarriers    = pre,
    };
    vkCmdPipelineBarrier2(cmd, &dep_pre);

    VkImageBlit blit = {
        .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
        },
        .srcOffsets = {
            { 0, 0, 0 },
            { graph->width, graph->height, 1 },
        },
        .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
        },
        .dstOffsets = {
            { 0, 0, 0 },
            { graph->thumb_width, graph->thumb_height, 1 },
        },
    };
    vkCmdBlitImage(cmd,
                   graph->display_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   graph->thumb_image,   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_LINEAR);

    // thumb: TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL (for copy-to-buffer).
    VkImageMemoryBarrier2 thumb_to_src = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = graph->thumb_image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    VkDependencyInfo dep_mid = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &thumb_to_src,
    };
    vkCmdPipelineBarrier2(cmd, &dep_mid);

    VkBufferImageCopy region = {
        .imageSubresource = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel       = 0,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
        .imageExtent = { (uint32_t)graph->thumb_width,
                         (uint32_t)graph->thumb_height, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, graph->thumb_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging, 1, &region);

    // display: TRANSFER_SRC_OPTIMAL -> GENERAL (so subsequent dispatches
    // see it in the layout they expect).
    VkImageMemoryBarrier2 display_back = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                       | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = graph->display_image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    VkDependencyInfo dep_post = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &display_back,
    };
    vkCmdPipelineBarrier2(cmd, &dep_post);

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

    void *map = NULL;
    if (vkMapMemory(graph->gpu->device, staging_mem, 0, needed, 0, &map) != VK_SUCCESS) {
        AP_ERROR("readback_thumb: vkMapMemory failed");
        goto out;
    }
    memcpy(out_pixels, map, needed);
    vkUnmapMemory(graph->gpu->device, staging_mem);
    *out_w = graph->thumb_width;
    *out_h = graph->thumb_height;
    rc = 0;

out:
    if (staging_mem) vkFreeMemory(graph->gpu->device, staging_mem, NULL);
    if (staging)    vkDestroyBuffer(graph->gpu->device, staging, NULL);
    return rc;
}
