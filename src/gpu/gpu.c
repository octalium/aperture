#include "gpu_internal.h"

#include "core/log.h"
#include "ui/imgui.h"

#include <stdlib.h>

const char *gpu_vk_result_str(VkResult r)
{
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_EVENT_SET: return "VK_EVENT_SET";
        case VK_EVENT_RESET: return "VK_EVENT_RESET";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
        case VK_ERROR_VALIDATION_FAILED_EXT: return "VK_ERROR_VALIDATION_FAILED_EXT";
        default: return "VK_<unknown>";
    }
}

ap_gpu *ap_gpu_create(int width, int height, const char *title)
{
    struct ap_gpu *g = calloc(1, sizeof(*g));
    if (!g) {
        AP_ERROR("ap_gpu_create: out of memory");
        return NULL;
    }

    if (gpu_window_create(g, width, height, title) < 0)   goto fail;
    if (gpu_instance_create(g, title) < 0)                goto fail;

    VkResult r = glfwCreateWindowSurface(g->instance, g->window, NULL, &g->surface);
    if (r != VK_SUCCESS) {
        AP_ERROR("glfwCreateWindowSurface -> %s", gpu_vk_result_str(r));
        goto fail;
    }

    if (gpu_device_create(g) < 0)                         goto fail;
    if (gpu_swapchain_create(g) < 0)                      goto fail;
    if (gpu_frames_create(g) < 0)                         goto fail;

    if (!ap_imgui_init(g->window, g->instance, g->physical, g->device,
                       g->graphics_family, g->graphics_queue,
                       g->swapchain_image_count, g->swapchain_format)) {
        goto fail;
    }

    return g;

fail:
    ap_gpu_destroy(g);
    return NULL;
}

void ap_gpu_destroy(ap_gpu *g)
{
    if (!g) return;

    if (g->device) {
        vkDeviceWaitIdle(g->device);
    }

    ap_imgui_shutdown();

    gpu_frames_destroy(g);
    gpu_swapchain_destroy(g);
    gpu_device_destroy(g);

    if (g->surface) {
        vkDestroySurfaceKHR(g->instance, g->surface, NULL);
        g->surface = VK_NULL_HANDLE;
    }

    gpu_instance_destroy(g);
    gpu_window_destroy(g);
    free(g);
}

bool ap_gpu_should_run(ap_gpu *g)
{
    glfwPollEvents();
    return !glfwWindowShouldClose(g->window);
}

int ap_gpu_render_frame(ap_gpu *g)
{
    return gpu_frame_render(g);
}

void ap_gpu_wait_idle(ap_gpu *g)
{
    if (g && g->device) {
        vkDeviceWaitIdle(g->device);
    }
}
