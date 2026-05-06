#ifndef APERTURE_UI_IMGUI_H
#define APERTURE_UI_IMGUI_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GLFWwindow GLFWwindow;

bool ap_imgui_init(GLFWwindow *window,
                   VkInstance instance,
                   VkPhysicalDevice physical,
                   VkDevice device,
                   uint32_t queue_family,
                   VkQueue queue,
                   uint32_t image_count,
                   VkFormat color_format);

void ap_imgui_shutdown(void);
void ap_imgui_new_frame(void);
void ap_imgui_render(VkCommandBuffer cmd);

void ap_imgui_demo_window(const char *title, const char *version);

#ifdef __cplusplus
}
#endif

#endif
