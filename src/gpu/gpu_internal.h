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
    uint64_t submit_value;  // frame_submit_count value of the last submit on this slot
} gpu_frame;

// Deferred-retire entry: a GPU resource unbound from all descriptors but
// possibly still referenced by submitted work. Holds either an owned
// pipeline graph or a raw texture handle set (thumbnails). Destroyed by
// gpu_retire_pump once both gates pass:
//   frame_gate  — every compositor submit issued at retire time has been
//                 observed complete (frame_complete_count >= frame_gate);
//   render_gate — the render timeline value in flight at retire time has
//                 been observed complete (render_complete_value >= render_gate,
//                 0 when no render was in flight).
// Gates are monotonic counters, never fence handles: the per-frame
// in_flight fences and render_fence are reset and reused every frame, so a
// snapshotted handle would re-arm when the fence is recycled.
typedef struct gpu_retire_entry {
    struct ap_pipeline_graph *graph;   // owned graph, or NULL
    VkSampler      sampler;            // owned raw handles, or VK_NULL_HANDLE
    VkImageView    view;
    VkImage        image;
    VkDeviceMemory memory;
    uint64_t frame_gate;
    uint64_t render_gate;
    struct gpu_retire_entry *next;
} gpu_retire_entry;

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

    // Pipeline-render submission, decoupled from the swapchain compositor
    // frame. The photo graph's compute chain is recorded + submitted on
    // its own command buffer; the compositor never blocks on it. The pump
    // keeps exactly one render in flight, polls its fence, and promotes the
    // finished slot to the compositor — so a heavy full-res render runs in
    // the background while the UI stays at refresh rate.
    VkCommandPool   render_pool;
    VkCommandBuffer render_cmd;
    VkFence         render_fence;        // in-flight render (polled, not waited)

    // render->composite ordering: each render signals this timeline to a
    // new value; the compositor waits the value of the slot it samples.
    VkSemaphore render_timeline;
    uint64_t    render_value;            // last signaled timeline value
    uint64_t    render_slot_value[APERTURE_FRAMES_IN_FLIGHT + 1]; // per slot

    int      render_inflight_slot;       // slot being rendered, -1 if idle
    int      render_front_slot;          // slot the compositor samples, -1 if none
    uint64_t render_front_value;         // timeline value of render_front_slot

    // Deferred retirement (see gpu_retire_entry). frame_submit_count is
    // bumped per compositor submit; frame_complete_count records the highest
    // submit observed complete (piggybacking on the per-frame fence wait at
    // the top of gpu_frame_render); render_complete_value records the last
    // render timeline value the pump observed complete.
    gpu_retire_entry *retire_list;
    uint64_t frame_submit_count;
    uint64_t frame_complete_count;
    uint64_t render_complete_value;

    // Pending structural graph swap. While next_graph is set the gpu OWNS
    // current_graph (the caller's photo already swapped to next_graph): the
    // compositor keeps sampling the old graph's front slot until the pump
    // promotes next_graph's first render, then rebinds next_canvas, makes
    // next_graph current and retires the old one — no stall, no blank frame.
    struct ap_pipeline_graph *next_graph;
    struct ap_canvas         *next_canvas;
    struct ap_pipeline_graph *render_graph; // graph the in-flight render belongs to

    // Session-wide compute-pipeline cache. Each unique stage shader is
    // compiled by the driver once; subsequent graph rebuilds (every edit)
    // reuse the cached pipeline instead of recompiling from scratch.
    VkPipelineCache pipeline_cache;

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
int  gpu_frame_render(struct ap_gpu *g);

int  gpu_render_create(struct ap_gpu *g);
void gpu_render_destroy(struct ap_gpu *g);
// Non-blocking render pump, called once per frame before the compositor.
// Promotes a finished in-flight render to the compositor (binds its slot),
// then — if the edit stack changed and no render is in flight — kicks a new
// render (record + present-copy + submit signalling the timeline + fence)
// WITHOUT waiting. No-op when no graph is bound. The UI thread never blocks
// on the render. Returns 0 on success.
int  gpu_render_pump(struct ap_gpu *g, const ap_edit_stack *stack);
// Reset the render scheduler to idle (no in-flight render, no front slot).
// Called when the bound graph changes; the device must be idle.
void gpu_render_reset(struct ap_gpu *g);

// Queue a resource for deferred destruction. Pass an owned graph OR raw
// texture handles (the unused side zeroed). Gates are snapshotted from the
// current counters; see gpu_retire_entry.
void gpu_retire_push(struct ap_gpu *g, struct ap_pipeline_graph *graph,
                     VkSampler sampler, VkImageView view,
                     VkImage image, VkDeviceMemory memory);
// Destroy every retire entry whose gates have been observed complete.
// Non-blocking; called once per frame from ap_gpu_render_frame.
void gpu_retire_pump(struct ap_gpu *g);
// Destroy every retire entry unconditionally. The device must be idle;
// also reconciles the observed counters to the issued ones.
void gpu_retire_flush(struct ap_gpu *g);

#define VK_CHECK(call)                                                  \
    do {                                                                \
        VkResult _r = (call);                                           \
        if (_r != VK_SUCCESS) {                                         \
            AP_ERROR("%s -> %s", #call, gpu_vk_result_str(_r));         \
            return -1;                                                  \
        }                                                               \
    } while (0)

static inline int gpu_find_memory_type(VkPhysicalDevice phys,
                                       uint32_t type_bits,
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

#endif
