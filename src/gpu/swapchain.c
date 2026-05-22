#include "gpu_internal.h"

#include "core/log.h"

#include <stdlib.h>
#include <string.h>

static VkSurfaceFormatKHR pick_surface_format(VkPhysicalDevice dev, VkSurfaceKHR surface)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &count, NULL);

    VkSurfaceFormatKHR fallback = {
        .format     = VK_FORMAT_B8G8R8A8_SRGB,
        .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
    };
    if (count == 0) return fallback;

    VkSurfaceFormatKHR *formats = calloc(count, sizeof(*formats));
    if (!formats) {
        AP_ERROR("swapchain: surface format list: out of memory");
        return fallback;
    }
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &count, formats);

    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = formats[i];
            break;
        }
    }
    free(formats);
    return chosen;
}

static VkExtent2D pick_extent(GLFWwindow *window, const VkSurfaceCapabilitiesKHR *caps)
{
    if (caps->currentExtent.width != UINT32_MAX) {
        return caps->currentExtent;
    }

    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);

    VkExtent2D extent = { .width = (uint32_t)w, .height = (uint32_t)h };
    if (extent.width  < caps->minImageExtent.width)  extent.width  = caps->minImageExtent.width;
    if (extent.width  > caps->maxImageExtent.width)  extent.width  = caps->maxImageExtent.width;
    if (extent.height < caps->minImageExtent.height) extent.height = caps->minImageExtent.height;
    if (extent.height > caps->maxImageExtent.height) extent.height = caps->maxImageExtent.height;
    return extent;
}

int gpu_swapchain_create(struct ap_gpu *g)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g->physical, g->surface, &caps));

    VkSurfaceFormatKHR fmt = pick_surface_format(g->physical, g->surface);
    // FIFO (vsync), always available. The UI is mostly static and the
    // pixel pipeline only re-runs on an actual edit, so there is no
    // frame to gain from rendering uncapped — MAILBOX would just spin
    // the loop and keep the GPU warm at rest.
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent = pick_extent(g->window, &caps);

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = g->surface,
        .minImageCount = image_count,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                    | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = mode,
        .clipped = VK_TRUE,
    };

    uint32_t qf[2] = { g->graphics_family, g->present_family };
    if (g->graphics_family != g->present_family) {
        ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices = qf;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VK_CHECK(vkCreateSwapchainKHR(g->device, &ci, NULL, &g->swapchain));

    g->swapchain_format = fmt.format;
    g->swapchain_extent = extent;

    vkGetSwapchainImagesKHR(g->device, g->swapchain, &g->swapchain_image_count, NULL);
    VkImage *raw = calloc(g->swapchain_image_count, sizeof(*raw));
    if (!raw) {
        AP_ERROR("swapchain image list: out of memory");
        return -1;
    }
    vkGetSwapchainImagesKHR(g->device, g->swapchain, &g->swapchain_image_count, raw);

    g->swapchain_images = calloc(g->swapchain_image_count, sizeof(*g->swapchain_images));
    if (!g->swapchain_images) {
        free(raw);
        AP_ERROR("swapchain wrapper list: out of memory");
        return -1;
    }

    VkSemaphoreCreateInfo sem_ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

    for (uint32_t i = 0; i < g->swapchain_image_count; i++) {
        g->swapchain_images[i].image = raw[i];
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = raw[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = fmt.format,
            .components = {
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        };
        VkResult r = vkCreateImageView(g->device, &vci, NULL,
                                       &g->swapchain_images[i].view);
        if (r != VK_SUCCESS) {
            free(raw);
            AP_ERROR("vkCreateImageView -> %s", gpu_vk_result_str(r));
            return -1;
        }
        r = vkCreateSemaphore(g->device, &sem_ci, NULL,
                              &g->swapchain_images[i].render_finished);
        if (r != VK_SUCCESS) {
            free(raw);
            AP_ERROR("vkCreateSemaphore -> %s", gpu_vk_result_str(r));
            return -1;
        }
    }
    free(raw);

    AP_INFO("swapchain: %ux%u, %u images, mode=%d",
            extent.width, extent.height, g->swapchain_image_count, mode);
    return 0;
}

void gpu_swapchain_destroy(struct ap_gpu *g)
{
    if (g->swapchain_images) {
        for (uint32_t i = 0; i < g->swapchain_image_count; i++) {
            if (g->swapchain_images[i].render_finished) {
                vkDestroySemaphore(g->device, g->swapchain_images[i].render_finished, NULL);
            }
            if (g->swapchain_images[i].view) {
                vkDestroyImageView(g->device, g->swapchain_images[i].view, NULL);
            }
        }
        free(g->swapchain_images);
        g->swapchain_images = NULL;
        g->swapchain_image_count = 0;
    }
    if (g->swapchain) {
        vkDestroySwapchainKHR(g->device, g->swapchain, NULL);
        g->swapchain = VK_NULL_HANDLE;
    }
}

int gpu_swapchain_recreate(struct ap_gpu *g)
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(g->window, &w, &h);
    while (w == 0 || h == 0) {
        glfwWaitEvents();
        glfwGetFramebufferSize(g->window, &w, &h);
    }

    vkDeviceWaitIdle(g->device);
    gpu_swapchain_destroy(g);
    return gpu_swapchain_create(g);
}
