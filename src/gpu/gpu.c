#include "gpu_internal.h"

#include "core/log.h"
#include "gpu/canvas.h"
#include "gpu/pipeline_graph.h"
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

static void retire_entry_destroy(struct ap_gpu *g, gpu_retire_entry *e)
{
    if (e->graph) {
        ap_pipeline_graph_destroy(e->graph);
    } else {
        if (e->sampler) vkDestroySampler(g->device, e->sampler, NULL);
        if (e->view)    vkDestroyImageView(g->device, e->view, NULL);
        if (e->image)   vkDestroyImage(g->device, e->image, NULL);
        if (e->memory)  vkFreeMemory(g->device, e->memory, NULL);
    }
    free(e);
}

void gpu_retire_push(struct ap_gpu *g, struct ap_pipeline_graph *graph,
                     VkSampler sampler, VkImageView view,
                     VkImage image, VkDeviceMemory memory)
{
    gpu_retire_entry *e = calloc(1, sizeof(*e));
    if (!e) {
        // destroying inline would race the gpu; leaking is the only safe
        // failure mode here, so make it loud.
        AP_ERROR("gpu: retire entry alloc failed; leaking a gpu resource");
        return;
    }
    e->graph   = graph;
    e->sampler = sampler;
    e->view    = view;
    e->image   = image;
    e->memory  = memory;
    // every compositor submit issued so far may sample the resource; work
    // submitted after this call records against the rewritten descriptors.
    e->frame_gate  = g->frame_submit_count;
    e->render_gate = (g->render_inflight_slot >= 0)
                   ? g->render_slot_value[g->render_inflight_slot] : 0;
    e->next = g->retire_list;
    g->retire_list = e;
}

void gpu_retire_pump(struct ap_gpu *g)
{
    gpu_retire_entry **p = &g->retire_list;
    while (*p) {
        gpu_retire_entry *e = *p;
        if (g->frame_complete_count >= e->frame_gate &&
            g->render_complete_value >= e->render_gate) {
            *p = e->next;
            retire_entry_destroy(g, e);
        } else {
            p = &e->next;
        }
    }
}

void gpu_retire_flush(struct ap_gpu *g)
{
    // device idle: everything issued has completed, even if the per-frame
    // observation points were skipped (e.g. a wait_idle between frames).
    g->frame_complete_count  = g->frame_submit_count;
    g->render_complete_value = g->render_value;
    while (g->retire_list) {
        gpu_retire_entry *e = g->retire_list;
        g->retire_list = e->next;
        retire_entry_destroy(g, e);
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

    // Session pipeline cache: a NULL handle is still valid to pass to
    // vkCreate*Pipelines, so a failure here just means no caching, not a
    // hard error.
    VkPipelineCacheCreateInfo pcci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
    };
    if (vkCreatePipelineCache(g->device, &pcci, NULL,
                              &g->pipeline_cache) != VK_SUCCESS) {
        AP_WARN("gpu: pipeline cache create failed; rebuilds won't be cached");
        g->pipeline_cache = VK_NULL_HANDLE;
    }

    if (gpu_swapchain_create(g) < 0)                      goto fail;
    if (gpu_frames_create(g) < 0)                         goto fail;
    if (gpu_render_create(g) < 0)                         goto fail;

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

    // teardown owns whatever the deferred paths still hold: a pending swap
    // means the gpu owns the outgoing current_graph; the retire list holds
    // everything else. The device is idle, so destroy it all now.
    if (g->next_graph) {
        ap_pipeline_graph_destroy(g->current_graph);
        g->current_graph = NULL;
        g->next_graph    = NULL;
        g->next_canvas   = NULL;
    }
    gpu_retire_flush(g);

    ap_imgui_shutdown();

    gpu_render_destroy(g);
    gpu_frames_destroy(g);
    gpu_swapchain_destroy(g);
    if (g->pipeline_cache) {
        vkDestroyPipelineCache(g->device, g->pipeline_cache, NULL);
        g->pipeline_cache = VK_NULL_HANDLE;
    }
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

int ap_gpu_render_frame(ap_gpu *g, const ap_edit_stack *stack)
{
    // Pump the background render (promote a finished one, kick a new one if
    // the edits changed) — never blocking — then composite the latest
    // finished slot in the swapchain frame. Retired resources are freed
    // last, after this frame's fence wait advanced the observed counters.
    gpu_render_pump(g, stack);
    int rc = gpu_frame_render(g);
    gpu_retire_pump(g);
    return rc;
}

void ap_gpu_set_graph(ap_gpu *g, ap_pipeline_graph *graph)
{
    // Callers swap the graph only with the device idle (close / open), so
    // any in-flight render has finished. A pending deferred swap means the
    // gpu owns the outgoing current_graph — destroy it now (idle), and drop
    // the pending graph reference (its owner is about to destroy it).
    if (g->next_graph) {
        ap_pipeline_graph_destroy(g->current_graph);
        g->next_graph  = NULL;
        g->next_canvas = NULL;
    }
    g->current_graph = graph;
    gpu_render_reset(g);
}

void ap_gpu_swap_graph(ap_gpu *g, ap_pipeline_graph *graph,
                       ap_canvas *canvas, ap_pipeline_graph *old)
{
    if (!g || !graph) return;

    if (g->next_graph) {
        // a prior pending swap never promoted: the compositor never sampled
        // it, but its first render may still be in flight — retire it.
        if (old == g->next_graph) old = NULL;
        gpu_retire_push(g, g->next_graph, VK_NULL_HANDLE, VK_NULL_HANDLE,
                        VK_NULL_HANDLE, VK_NULL_HANDLE);
        g->next_graph  = NULL;
        g->next_canvas = NULL;
    }
    if (old && old != g->current_graph) {
        // relinquished graph was never the bound one; just retire it.
        gpu_retire_push(g, old, VK_NULL_HANDLE, VK_NULL_HANDLE,
                        VK_NULL_HANDLE, VK_NULL_HANDLE);
        old = NULL;
    }
    if (!g->current_graph) {
        // nothing bound (and therefore no render in flight): bind directly;
        // the scheduler is already idle.
        g->current_graph = graph;
        if (canvas) ap_canvas_bind_graph(canvas, graph);
        return;
    }
    // keep displaying current_graph (now gpu-owned, == old) until the
    // pump promotes graph's first render; see gpu_render_pump.
    g->next_graph  = graph;
    g->next_canvas = canvas;
}

void ap_gpu_set_canvas(ap_gpu *g, ap_canvas *canvas)
{
    g->current_canvas = canvas;
}

void ap_gpu_set_grid(ap_gpu *g, ap_grid *grid)
{
    g->current_grid = grid;
}

void ap_gpu_set_window_title(ap_gpu *g, const char *title)
{
    if (g && g->window && title) {
        glfwSetWindowTitle(g->window, title);
    }
}

void ap_gpu_wait_idle(ap_gpu *g)
{
    if (g && g->device) {
        vkDeviceWaitIdle(g->device);
        // idle means every retire gate is satisfied; flush so callers that
        // follow up with inline destruction (library swap/close, photo
        // close) see the deferred resources actually gone.
        gpu_retire_flush(g);
    }
}
