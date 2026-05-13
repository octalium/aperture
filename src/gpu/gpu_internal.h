#ifndef APERTURE_GPU_INTERNAL_H
#define APERTURE_GPU_INTERNAL_H

#include "gpu.h"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>

#define APERTURE_FRAMES_IN_FLIGHT 2

#ifdef NDEBUG
#define APERTURE_VALIDATION 0
#else
#define APERTURE_VALIDATION 1
#endif

typedef struct {
    VkImage image;
    VkImageView view;
    VkSemaphore render_finished;
} gpu_swapchain_image;

typedef struct {
    VkCommandBuffer cmd;
    VkSemaphore image_available;
    VkFence in_flight;
} gpu_frame;

struct ap_gpu {
    GLFWwindow *window;
    bool framebuffer_resized;

    bool window_fullscreen;

    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger;

    VkSurfaceKHR surface;

    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t graphics_family;
    uint32_t present_family;
    VkQueue graphics_queue;
    VkQueue present_queue;

    VkSwapchainKHR swapchain;
    VkFormat swapchain_format;
    VkExtent2D swapchain_extent;
    gpu_swapchain_image *swapchain_images;
    uint32_t swapchain_image_count;

    VkCommandPool command_pool;
    gpu_frame frames[APERTURE_FRAMES_IN_FLIGHT];
    uint32_t current_frame;

    struct ap_pipeline_graph *current_graph;
    struct ap_canvas         *current_canvas;
    struct ap_grid           *current_grid;
};

const char *gpu_vk_result_str(VkResult r);

int  gpu_window_create(struct ap_gpu *g, int width, int height, const char *title);
void gpu_window_destroy(struct ap_gpu *g);

int  gpu_instance_create(struct ap_gpu *g, const char *app_name);
void gpu_instance_destroy(struct ap_gpu *g);

int  gpu_device_create(struct ap_gpu *g);
void gpu_device_destroy(struct ap_gpu *g);

int  gpu_swapchain_create(struct ap_gpu *g);
void gpu_swapchain_destroy(struct ap_gpu *g);
int  gpu_swapchain_recreate(struct ap_gpu *g);

int  gpu_frames_create(struct ap_gpu *g);
void gpu_frames_destroy(struct ap_gpu *g);
int  gpu_frame_render(struct ap_gpu *g, const ap_edit_state *edit);

#define VK_CHECK(call)                                                  \
    do {                                                                \
        VkResult _r = (call);                                           \
        if (_r != VK_SUCCESS) {                                         \
            AP_ERROR("%s -> %s", #call, gpu_vk_result_str(_r));         \
            return -1;                                                  \
        }                                                               \
    } while (0)

#endif
