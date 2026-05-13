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

static GLFWmonitor *pick_target_monitor(ap_gpu *g)
{
    int mcount = 0;
    GLFWmonitor **mons = glfwGetMonitors(&mcount);
    if (mcount == 0) return glfwGetPrimaryMonitor();

    int idx = g->fullscreen_monitor_idx;
    if (idx < 0 || idx >= mcount) idx = 0;
    return mons[idx];
}

int ap_gpu_monitor_count(ap_gpu *g)
{
    (void)g;
    int n = 0;
    glfwGetMonitors(&n);
    return n;
}

const char *ap_gpu_monitor_name(ap_gpu *g, int idx)
{
    (void)g;
    int n = 0;
    GLFWmonitor **mons = glfwGetMonitors(&n);
    if (idx < 0 || idx >= n) return NULL;
    return glfwGetMonitorName(mons[idx]);
}

int ap_gpu_fullscreen_monitor(const ap_gpu *g)
{
    return g ? g->fullscreen_monitor_idx : 0;
}

void ap_gpu_set_fullscreen_monitor(ap_gpu *g, int idx)
{
    if (!g) return;
    int n = ap_gpu_monitor_count(g);
    if (idx < 0)      idx = 0;
    if (n > 0 && idx >= n) idx = n - 1;
    g->fullscreen_monitor_idx = idx;
}

void ap_gpu_toggle_fullscreen(ap_gpu *g)
{
    if (!g || !g->window) return;

    if (g->window_fullscreen) {
        AP_INFO("fullscreen: restoring windowed");
        glfwSetWindowMonitor(g->window, NULL,
                             g->windowed_x, g->windowed_y,
                             g->windowed_w, g->windowed_h,
                             GLFW_DONT_CARE);
        g->window_fullscreen = false;
        return;
    }

    // Wayland doesn't expose window position to clients, so we can't
    // detect which monitor the window is on. Fall back to the user's
    // configured target (default: monitor 0). See gpu.h.
    glfwGetWindowPos(g->window,  &g->windowed_x, &g->windowed_y);
    glfwGetWindowSize(g->window, &g->windowed_w, &g->windowed_h);

    GLFWmonitor *mon = pick_target_monitor(g);
    if (!mon) {
        AP_WARN("fullscreen: no monitor available, staying windowed");
        return;
    }
    const GLFWvidmode *mode = glfwGetVideoMode(mon);
    if (!mode) {
        AP_WARN("fullscreen: monitor returned no video mode, staying windowed");
        return;
    }

    AP_INFO("fullscreen: entering on %s %dx%d@%dHz",
            glfwGetMonitorName(mon) ? glfwGetMonitorName(mon) : "(unnamed)",
            mode->width, mode->height, mode->refreshRate);
    glfwSetWindowMonitor(g->window, mon, 0, 0,
                         mode->width, mode->height, mode->refreshRate);
    g->window_fullscreen = true;
}
