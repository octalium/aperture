// Minimal vulkan.h shim for tests that link source files which include
// <vulkan/vulkan.h> for an opaque-pointer typedef but never call the
// API. Provides only the typedefs the production headers (e.g.
// photo/thumbnail.h) use in declarations.

#ifndef APERTURE_TEST_VULKAN_STUB_H
#define APERTURE_TEST_VULKAN_STUB_H

#include <stdint.h>

typedef void *VkImageView;
typedef void *VkSampler;
typedef void *VkImage;
typedef void *VkDevice;
typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef void *VkQueue;
typedef void *VkCommandBuffer;
typedef uint64_t VkDeviceSize;
typedef uint32_t VkFlags;

#define VK_NULL_HANDLE 0

#endif
