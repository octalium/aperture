#include "gpu_internal.h"

#include "core/log.h"

static void framebuffer_resize_cb(GLFWwindow *win, int w, int h)
{
    (void)w;
    (void)h;
    struct ap_gpu *g = glfwGetWindowUserPointer(win);
    g->framebuffer_resized = true;
}

static void key_cb(GLFWwindow *win, int key, int scancode, int action, int mods)
{
    (void)scancode;
    (void)mods;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        glfwSetWindowShouldClose(win, GLFW_TRUE);
    }
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

    g->window = glfwCreateWindow(width, height, title, NULL, NULL);
    if (!g->window) {
        AP_ERROR("glfwCreateWindow failed");
        glfwTerminate();
        return -1;
    }

    glfwSetWindowUserPointer(g->window, g);
    glfwSetFramebufferSizeCallback(g->window, framebuffer_resize_cb);
    glfwSetKeyCallback(g->window, key_cb);

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
