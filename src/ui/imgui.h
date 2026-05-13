#ifndef APERTURE_UI_IMGUI_H
#define APERTURE_UI_IMGUI_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GLFWwindow GLFWwindow;

// Backend lifecycle. The bridge's job narrows to this - anything that
// touches ImGui's GLFW / Vulkan impl backends, plus texture-handle
// registration. Higher-level UI lives in src/panels/ via cimgui.

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

// End the current ImGui frame without producing draw data. Use when
// the GPU path bails after ap_imgui_new_frame (e.g. swapchain
// recreate); without this, the next NewFrame trips
// ErrorCheckNewFrameSanityChecks and aborts.
void ap_imgui_discard_frame(void);

uint64_t ap_imgui_register_texture(VkSampler sampler, VkImageView view, VkImageLayout layout);
void     ap_imgui_unregister_texture(uint64_t tex_id);

#ifdef __cplusplus
}
#endif

#endif
