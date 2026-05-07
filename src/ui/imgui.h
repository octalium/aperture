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

uint64_t ap_imgui_register_texture(VkSampler sampler, VkImageView view, VkImageLayout layout);
void     ap_imgui_unregister_texture(uint64_t tex_id);
void     ap_imgui_viewport_window(const char *title, uint64_t tex_id,
                                  int img_width, int img_height);

void     ap_imgui_edit_panel(float *exposure_ev,
                             float *tone_contrast,
                             float *tone_pivot);

#ifdef __cplusplus
}
#endif

#endif
