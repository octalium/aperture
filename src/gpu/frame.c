#include "gpu_internal.h"

#include "core/log.h"
#include "gpu/canvas.h"
#include "gpu/grid.h"
#include "gpu/pipeline_graph.h"
#include "ui/imgui.h"

// render_slot_value[] (sized APERTURE_FRAMES_IN_FLIGHT + 1 in ap_gpu) is
// indexed by the graph's present slot, which round-robins over
// AP_DISPLAY_SLOTS. Keep the array large enough to index every slot.
_Static_assert(AP_DISPLAY_SLOTS <= APERTURE_FRAMES_IN_FLIGHT + 1,
               "render_slot_value[] too small for AP_DISPLAY_SLOTS");

int gpu_frames_create(struct ap_gpu *g)
{
    VkCommandPoolCreateInfo pool_ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g->graphics_family,
    };
    VK_CHECK(vkCreateCommandPool(g->device, &pool_ci, NULL, &g->command_pool));

    VkCommandBufferAllocateInfo cb_ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    VkSemaphoreCreateInfo sem_ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fence_ci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    for (uint32_t i = 0; i < APERTURE_FRAMES_IN_FLIGHT; i++) {
        VK_CHECK(vkAllocateCommandBuffers(g->device, &cb_ai, &g->frames[i].cmd));
        VK_CHECK(vkCreateSemaphore(g->device, &sem_ci, NULL, &g->frames[i].image_available));
        VK_CHECK(vkCreateFence(g->device, &fence_ci, NULL, &g->frames[i].in_flight));
    }
    return 0;
}

void gpu_frames_destroy(struct ap_gpu *g)
{
    for (uint32_t i = 0; i < APERTURE_FRAMES_IN_FLIGHT; i++) {
        if (g->frames[i].in_flight) {
            vkDestroyFence(g->device, g->frames[i].in_flight, NULL);
            g->frames[i].in_flight = VK_NULL_HANDLE;
        }
        if (g->frames[i].image_available) {
            vkDestroySemaphore(g->device, g->frames[i].image_available, NULL);
            g->frames[i].image_available = VK_NULL_HANDLE;
        }
    }
    if (g->command_pool) {
        vkDestroyCommandPool(g->device, g->command_pool, NULL);
        g->command_pool = VK_NULL_HANDLE;
    }
}

int gpu_render_create(struct ap_gpu *g)
{
    VkCommandPoolCreateInfo pool_ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g->graphics_family,
    };
    VK_CHECK(vkCreateCommandPool(g->device, &pool_ci, NULL, &g->render_pool));

    VkCommandBufferAllocateInfo cb_ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g->render_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VK_CHECK(vkAllocateCommandBuffers(g->device, &cb_ai, &g->render_cmd));

    VkFenceCreateInfo fence_ci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_CHECK(vkCreateFence(g->device, &fence_ci, NULL, &g->render_fence));

    VkSemaphoreTypeCreateInfo type_ci = {
        .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue  = 0,
    };
    VkSemaphoreCreateInfo sem_ci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &type_ci,
    };
    VK_CHECK(vkCreateSemaphore(g->device, &sem_ci, NULL, &g->render_timeline));

    g->render_value         = 0;
    g->render_inflight_slot = -1;
    g->render_front_slot    = -1;
    g->render_front_value   = 0;
    return 0;
}

void gpu_render_destroy(struct ap_gpu *g)
{
    if (g->render_timeline) {
        vkDestroySemaphore(g->device, g->render_timeline, NULL);
        g->render_timeline = VK_NULL_HANDLE;
    }
    if (g->render_fence) {
        vkDestroyFence(g->device, g->render_fence, NULL);
        g->render_fence = VK_NULL_HANDLE;
    }
    if (g->render_pool) {
        vkDestroyCommandPool(g->device, g->render_pool, NULL);
        g->render_pool = VK_NULL_HANDLE;
    }
}

void gpu_render_reset(struct ap_gpu *g)
{
    // The bound graph is changing; the device is idle, so any in-flight
    // render has completed. Discard it rather than promote a slot of the
    // outgoing graph, and forget the front slot (the new graph's slots
    // hold no content until its first render promotes).
    g->render_inflight_slot = -1;
    g->render_front_slot    = -1;
    g->render_front_value   = 0;
}

int gpu_render_pump(struct ap_gpu *g, const ap_edit_stack *stack)
{
    if (!g->current_graph) return 0;

    // 1. Promote a finished in-flight render to the compositor. Poll the
    //    fence — never wait. While a render is in flight, do not kick
    //    another (one in flight; the next picks up the latest stack, which
    //    coalesces a fast slider drag down to a single follow-up render).
    if (g->render_inflight_slot >= 0) {
        VkResult fs = vkGetFenceStatus(g->device, g->render_fence);
        if (fs == VK_NOT_READY) {
            return 0;
        }
        if (fs != VK_SUCCESS) {
            AP_ERROR("render: vkGetFenceStatus -> %s", gpu_vk_result_str(fs));
            return -1;
        }
        g->render_front_slot  = g->render_inflight_slot;
        g->render_front_value = g->render_slot_value[g->render_front_slot];
        if (g->current_canvas) {
            ap_canvas_bind_slot(g->current_canvas, g->render_front_slot);
        }
        g->render_inflight_slot = -1;
    }

    // 2. Kick a new render only when the edit stack actually changed.
    if (!ap_pipeline_graph_needs_render(g->current_graph, stack)) {
        return 0;
    }

    VkCommandBuffer cmd = g->render_cmd;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    int rc = ap_pipeline_graph_record(g->current_graph, cmd, stack);
    if (rc != 1) {
        VK_CHECK(vkEndCommandBuffer(cmd));
        return rc < 0 ? -1 : 0;
    }
    int slot = ap_pipeline_graph_next_slot(g->current_graph);
    ap_pipeline_graph_present_copy(g->current_graph, cmd, slot);
    VK_CHECK(vkEndCommandBuffer(cmd));

    uint64_t value = ++g->render_value;
    g->render_slot_value[slot] = value;

    VkCommandBufferSubmitInfo cmd_si = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSemaphoreSubmitInfo signal_si = {
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = g->render_timeline,
        .value     = value,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &cmd_si,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos    = &signal_si,
    };
    VK_CHECK(vkResetFences(g->device, 1, &g->render_fence));
    VK_CHECK(vkQueueSubmit2(g->graphics_queue, 1, &submit, g->render_fence));
    g->render_inflight_slot = slot;
    return 0;
}

static void image_barrier(VkCommandBuffer cmd, VkImage image,
                          VkImageLayout old_layout, VkImageLayout new_layout,
                          VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                          VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
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
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

static int record_frame(struct ap_gpu *g, VkCommandBuffer cmd,
                        uint32_t image_index)
{
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    // The photo graph's compute chain is no longer recorded here — it runs
    // on its own render submission (gpu_render_pump) before this compositor
    // frame, which only samples the finished presentation slot.
    VkImage target  = g->swapchain_images[image_index].image;
    VkImageView vw  = g->swapchain_images[image_index].view;

    image_barrier(cmd, target,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = vw,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = { .color = { .float32 = { 0.18f, 0.18f, 0.18f, 1.0f } } },
    };

    VkRenderingInfo rendering = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = { .offset = {0, 0}, .extent = g->swapchain_extent },
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color,
    };

    vkCmdBeginRendering(cmd, &rendering);
    if (g->current_grid) {
        ap_grid_record(g->current_grid, cmd,
                       (int)g->swapchain_extent.width,
                       (int)g->swapchain_extent.height);
    } else if (g->current_canvas) {
        ap_canvas_record(g->current_canvas, cmd,
                         (int)g->swapchain_extent.width,
                         (int)g->swapchain_extent.height);
    }
    ap_imgui_render(cmd);
    vkCmdEndRendering(cmd);

    image_barrier(cmd, target,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

    VK_CHECK(vkEndCommandBuffer(cmd));
    return 0;
}

int gpu_frame_render(struct ap_gpu *g)
{
    gpu_frame *f = &g->frames[g->current_frame];

    VK_CHECK(vkWaitForFences(g->device, 1, &f->in_flight, VK_TRUE, UINT64_MAX));

    uint32_t image_index = 0;
    VkResult acq = vkAcquireNextImageKHR(g->device, g->swapchain, UINT64_MAX,
                                         f->image_available, VK_NULL_HANDLE,
                                         &image_index);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        // We've already begun an ImGui frame in this iteration but are
        // about to skip the record/submit path entirely. End the frame
        // so the next ap_imgui_new_frame doesn't trip ImGui's
        // FrameCountEnded == FrameCount assertion.
        ap_imgui_discard_frame();
        return gpu_swapchain_recreate(g);
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        ap_imgui_discard_frame();
        AP_ERROR("vkAcquireNextImageKHR -> %s", gpu_vk_result_str(acq));
        return -1;
    }

    VK_CHECK(vkResetCommandBuffer(f->cmd, 0));

    if (record_frame(g, f->cmd, image_index) < 0) {
        // record failed before any submit. in_flight is still signaled
        // (it's reset just before the submit below), so a retry or
        // teardown never waits on a fence nothing will signal.
        return -1;
    }

    VkSemaphore render_finished = g->swapchain_images[image_index].render_finished;

    // Wait for the acquired swapchain image, and — once a render has been
    // promoted — for that render to be done on the GPU before the canvas
    // fragment shader samples its slot. The timeline value is already
    // reached (we only promote after the render fence signalled), so this
    // wait never blocks; it makes the render->sample dependency explicit.
    VkSemaphoreSubmitInfo waits[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = f->image_available,
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        },
    };
    uint32_t wait_count = 1;
    if (g->render_front_slot >= 0) {
        waits[1] = (VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = g->render_timeline,
            .value     = g->render_front_value,
            .stageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        };
        wait_count = 2;
    }
    VkSemaphoreSubmitInfo signal_si = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = render_finished,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkCommandBufferSubmitInfo cmd_si = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = f->cmd,
    };
    VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = wait_count,
        .pWaitSemaphoreInfos = waits,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cmd_si,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal_si,
    };
    // Reset the fence to unsignaled only now that we are committed to
    // submitting (acquire + record both succeeded) — the early returns
    // above intentionally leave it signaled so a skipped frame doesn't
    // deadlock the next wait. Without this reset the per-frame wait at the
    // top is a permanent no-op (the fence is created SIGNALED and never
    // reset) and the submit hands the GPU an already-signaled fence (UB),
    // so two frames never truly pipeline.
    VK_CHECK(vkResetFences(g->device, 1, &f->in_flight));
    VK_CHECK(vkQueueSubmit2(g->graphics_queue, 1, &submit, f->in_flight));

    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &render_finished,
        .swapchainCount = 1,
        .pSwapchains = &g->swapchain,
        .pImageIndices = &image_index,
    };
    VkResult pres = vkQueuePresentKHR(g->present_queue, &present);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR ||
        g->framebuffer_resized) {
        g->framebuffer_resized = false;
        if (gpu_swapchain_recreate(g) < 0) return -1;
    } else if (pres != VK_SUCCESS) {
        AP_ERROR("vkQueuePresentKHR -> %s", gpu_vk_result_str(pres));
        return -1;
    }

    g->current_frame = (g->current_frame + 1) % APERTURE_FRAMES_IN_FLIGHT;
    return 0;
}
