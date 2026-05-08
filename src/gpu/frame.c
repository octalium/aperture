#include "gpu_internal.h"

#include "core/log.h"
#include "gpu/canvas.h"
#include "gpu/pipeline_graph.h"
#include "ui/imgui.h"

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
                        uint32_t image_index, const ap_edit_state *edit)
{
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    if (g->current_graph) {
        ap_pipeline_graph_record(g->current_graph, cmd, edit);
    }

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
    if (g->current_canvas) {
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

int gpu_frame_render(struct ap_gpu *g, const ap_edit_state *edit)
{
    gpu_frame *f = &g->frames[g->current_frame];

    VK_CHECK(vkWaitForFences(g->device, 1, &f->in_flight, VK_TRUE, UINT64_MAX));

    uint32_t image_index = 0;
    VkResult acq = vkAcquireNextImageKHR(g->device, g->swapchain, UINT64_MAX,
                                         f->image_available, VK_NULL_HANDLE,
                                         &image_index);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        return gpu_swapchain_recreate(g);
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        AP_ERROR("vkAcquireNextImageKHR -> %s", gpu_vk_result_str(acq));
        return -1;
    }

    VK_CHECK(vkResetFences(g->device, 1, &f->in_flight));
    VK_CHECK(vkResetCommandBuffer(f->cmd, 0));

    if (record_frame(g, f->cmd, image_index, edit) < 0) {
        return -1;
    }

    VkSemaphore render_finished = g->swapchain_images[image_index].render_finished;

    VkSemaphoreSubmitInfo wait_si = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = f->image_available,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
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
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &wait_si,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cmd_si,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal_si,
    };
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
