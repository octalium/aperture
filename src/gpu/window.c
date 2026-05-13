#include "gpu_internal.h"

#include "core/log.h"

static void framebuffer_resize_cb(GLFWwindow *win, int w, int h)
{
    (void)w;
    (void)h;
    struct ap_gpu *g = glfwGetWindowUserPointer(win);
    g->framebuffer_resized = true;
}

static void glfw_error_cb(int code, const char *desc)
{
    AP_ERROR("glfw[%d]: %s", code, desc);
}

int gpu_window_create(struct ap_gpu *g, int width, int height, const char *title)
{
    glfwSetErrorCallback(glfw_error_cb);

    if (!glfwInit()) {
        AP_ERROR("glfwInit failed");
        return -1;
    }

    if (!glfwVulkanSupported()) {
        AP_ERROR("glfw reports Vulkan unsupported on this system");
        glfwTerminate();
        return -1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);

    g->window = glfwCreateWindow(width, height, title, NULL, NULL);
    if (!g->window) {
        AP_ERROR("glfwCreateWindow failed");
        glfwTerminate();
        return -1;
    }

    glfwSetWindowUserPointer(g->window, g);
    glfwSetFramebufferSizeCallback(g->window, framebuffer_resize_cb);

    return 0;
}

void gpu_window_destroy(struct ap_gpu *g)
{
    if (g->window) {
        glfwDestroyWindow(g->window);
        g->window = NULL;
    }
    glfwTerminate();
}

bool ap_gpu_is_fullscreen(const ap_gpu *g)
{
    return g ? g->window_fullscreen : false;
}

void ap_gpu_toggle_fullscreen(ap_gpu *g)
{
    if (!g || !g->window) return;

    if (g->window_fullscreen) {
        AP_INFO("fullscreen: restoring");
        glfwRestoreWindow(g->window);
        glfwSetWindowAttrib(g->window, GLFW_DECORATED, GLFW_TRUE);
        g->window_fullscreen = false;
        return;
    }

    AP_INFO("fullscreen: entering (compositor places the window)");
    glfwSetWindowAttrib(g->window, GLFW_DECORATED, GLFW_FALSE);
    glfwMaximizeWindow(g->window);
    g->window_fullscreen = true;
}
