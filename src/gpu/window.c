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

// Pick the monitor whose work-area contains the window's center.
// Falls back to the primary monitor when nothing matches (window
// off-screen, no monitors, etc.).
static GLFWmonitor *monitor_for_window(GLFWwindow *win)
{
    if (!win) return glfwGetPrimaryMonitor();

    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(win,  &wx, &wy);
    glfwGetWindowSize(win, &ww, &wh);
    int cx = wx + ww / 2;
    int cy = wy + wh / 2;

    int mcount = 0;
    GLFWmonitor **mons = glfwGetMonitors(&mcount);
    for (int i = 0; i < mcount; i++) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(mons[i], &mx, &my, &mw, &mh);
        if (cx >= mx && cx < mx + mw && cy >= my && cy < my + mh) {
            return mons[i];
        }
    }
    return glfwGetPrimaryMonitor();
}

void ap_gpu_toggle_fullscreen(ap_gpu *g)
{
    if (!g || !g->window) return;

    if (g->window_fullscreen) {
        glfwSetWindowMonitor(g->window, NULL,
                             g->windowed_x, g->windowed_y,
                             g->windowed_w, g->windowed_h,
                             GLFW_DONT_CARE);
        g->window_fullscreen = false;
        return;
    }

    glfwGetWindowPos(g->window,  &g->windowed_x, &g->windowed_y);
    glfwGetWindowSize(g->window, &g->windowed_w, &g->windowed_h);

    GLFWmonitor *mon = monitor_for_window(g->window);
    if (!mon) return;
    const GLFWvidmode *mode = glfwGetVideoMode(mon);
    if (!mode) return;

    glfwSetWindowMonitor(g->window, mon, 0, 0,
                         mode->width, mode->height, mode->refreshRate);
    g->window_fullscreen = true;
}
