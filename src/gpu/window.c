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

static const char *platform_name(void)
{
    switch (glfwGetPlatform()) {
        case GLFW_PLATFORM_WIN32:   return "win32";
        case GLFW_PLATFORM_COCOA:   return "cocoa";
        case GLFW_PLATFORM_WAYLAND: return "wayland";
        case GLFW_PLATFORM_X11:     return "x11";
        default:                    return "unknown";
    }
}

// Pick the monitor whose work-area contains the window's center.
// Falls back to the primary monitor when nothing matches (window
// off-screen, position unreliable on the current backend, no
// monitors). Verbose-logs the decision so multi-monitor / Wayland
// surprises are easy to diagnose.
static GLFWmonitor *monitor_for_window(GLFWwindow *win)
{
    GLFWmonitor *primary = glfwGetPrimaryMonitor();
    if (!win) return primary;

    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(win,  &wx, &wy);
    glfwGetWindowSize(win, &ww, &wh);
    int cx = wx + ww / 2;
    int cy = wy + wh / 2;

    AP_INFO("monitor pick: platform=%s window pos=(%d,%d) size=(%dx%d) center=(%d,%d)",
            platform_name(), wx, wy, ww, wh, cx, cy);

    int mcount = 0;
    GLFWmonitor **mons = glfwGetMonitors(&mcount);
    GLFWmonitor *picked = NULL;
    int picked_idx = -1;
    for (int i = 0; i < mcount; i++) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(mons[i], &mx, &my, &mw, &mh);
        const char *name = glfwGetMonitorName(mons[i]);
        bool hit = (cx >= mx && cx < mx + mw && cy >= my && cy < my + mh);
        AP_INFO("monitor[%d] %s workarea=(%d,%d %dx%d)%s",
                i, name ? name : "(unnamed)",
                mx, my, mw, mh,
                hit ? "  HIT" : "");
        if (hit && !picked) {
            picked = mons[i];
            picked_idx = i;
        }
    }

    if (!picked) {
        AP_INFO("monitor pick: no work-area contained the window center; "
                "falling back to primary monitor");
        return primary;
    }
    AP_INFO("monitor pick: chose monitor[%d]", picked_idx);
    return picked;
}

void ap_gpu_toggle_fullscreen(ap_gpu *g)
{
    if (!g || !g->window) return;

    if (g->window_fullscreen) {
        AP_INFO("fullscreen: restoring windowed pos=(%d,%d) size=(%dx%d)",
                g->windowed_x, g->windowed_y,
                g->windowed_w, g->windowed_h);
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
