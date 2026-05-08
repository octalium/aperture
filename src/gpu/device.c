#include "gpu_internal.h"

#include "core/log.h"

#include <stdlib.h>
#include <string.h>

static const char *DEVICE_EXTENSIONS[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

static bool extensions_supported(VkPhysicalDevice dev)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(dev, NULL, &count, NULL);
    if (count == 0) {
        return false;
    }

    VkExtensionProperties *props = calloc(count, sizeof(*props));
    if (!props) {
        return false;
    }
    vkEnumerateDeviceExtensionProperties(dev, NULL, &count, props);

    bool ok = true;
    for (size_t r = 0; r < sizeof(DEVICE_EXTENSIONS) / sizeof(DEVICE_EXTENSIONS[0]); r++) {
        bool found = false;
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(props[i].extensionName, DEVICE_EXTENSIONS[r]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            ok = false;
            break;
        }
    }

    free(props);
    return ok;
}

static bool find_queue_families(VkPhysicalDevice dev, VkSurfaceKHR surface,
                                uint32_t *graphics_out, uint32_t *present_out)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, NULL);
    if (count == 0) {
        return false;
    }

    VkQueueFamilyProperties *fams = calloc(count, sizeof(*fams));
    if (!fams) {
        return false;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, fams);

    bool got_graphics = false;
    bool got_present = false;
    uint32_t graphics = 0;
    uint32_t present = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (!got_graphics && (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            graphics = i;
            got_graphics = true;
        }
        VkBool32 supports_present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &supports_present);
        if (!got_present && supports_present) {
            present = i;
            got_present = true;
        }
        if (got_graphics && got_present) {
            break;
        }
    }

    free(fams);
    if (!got_graphics || !got_present) {
        return false;
    }
    *graphics_out = graphics;
    *present_out = present;
    return true;
}

static bool surface_has_formats_and_modes(VkPhysicalDevice dev, VkSurfaceKHR surface)
{
    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &fmt_count, NULL);
    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface, &mode_count, NULL);
    return fmt_count > 0 && mode_count > 0;
}

static bool meets_api_version(VkPhysicalDevice dev)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev, &props);
    return props.apiVersion >= VK_API_VERSION_1_3;
}

static bool supports_required_features(VkPhysicalDevice dev)
{
    VkPhysicalDeviceVulkan12Features v12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    };
    VkPhysicalDeviceVulkan13Features v13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &v12,
    };
    VkPhysicalDeviceFeatures2 f = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &v13,
    };
    vkGetPhysicalDeviceFeatures2(dev, &f);
    return v13.synchronization2 == VK_TRUE
        && v13.dynamicRendering == VK_TRUE
        && v12.runtimeDescriptorArray == VK_TRUE
        && v12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE
        && v12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE;
}

static int score_device(VkPhysicalDevice dev)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev, &props);
    return props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000 : 100;
}

static int pick_physical_device(struct ap_gpu *g)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g->instance, &count, NULL);
    if (count == 0) {
        AP_ERROR("no Vulkan physical devices found");
        return -1;
    }

    VkPhysicalDevice *devs = calloc(count, sizeof(*devs));
    if (!devs) {
        AP_ERROR("physical device list: out of memory");
        return -1;
    }
    vkEnumeratePhysicalDevices(g->instance, &count, devs);

    VkPhysicalDevice best = VK_NULL_HANDLE;
    int best_score = -1;
    uint32_t best_graphics = 0;
    uint32_t best_present = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t graphics = 0;
        uint32_t present = 0;
        if (!meets_api_version(devs[i])) continue;
        if (!find_queue_families(devs[i], g->surface, &graphics, &present)) continue;
        if (!extensions_supported(devs[i])) continue;
        if (!surface_has_formats_and_modes(devs[i], g->surface)) continue;
        if (!supports_required_features(devs[i])) continue;

        int s = score_device(devs[i]);
        if (s > best_score) {
            best = devs[i];
            best_score = s;
            best_graphics = graphics;
            best_present = present;
        }
    }
    free(devs);

    if (best == VK_NULL_HANDLE) {
        AP_ERROR("no suitable Vulkan physical device");
        return -1;
    }

    g->physical = best;
    g->graphics_family = best_graphics;
    g->present_family = best_present;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(best, &props);
    AP_INFO("gpu: %s (api %u.%u.%u)",
            props.deviceName,
            VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion),
            VK_API_VERSION_PATCH(props.apiVersion));

    return 0;
}

int gpu_device_create(struct ap_gpu *g)
{
    if (pick_physical_device(g) < 0) {
        return -1;
    }

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queues[2] = {0};
    uint32_t queue_count = 1;

    queues[0] = (VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g->graphics_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };

    if (g->present_family != g->graphics_family) {
        queues[1] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = g->present_family,
            .queueCount = 1,
            .pQueuePriorities = &queue_priority,
        };
        queue_count = 2;
    }

    VkPhysicalDeviceVulkan12Features v12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .runtimeDescriptorArray = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
        .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
    };
    VkPhysicalDeviceVulkan13Features v13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &v12,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };

    VkDeviceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &v13,
        .queueCreateInfoCount = queue_count,
        .pQueueCreateInfos = queues,
        .enabledExtensionCount = sizeof(DEVICE_EXTENSIONS) / sizeof(DEVICE_EXTENSIONS[0]),
        .ppEnabledExtensionNames = DEVICE_EXTENSIONS,
    };

    VK_CHECK(vkCreateDevice(g->physical, &ci, NULL, &g->device));

    vkGetDeviceQueue(g->device, g->graphics_family, 0, &g->graphics_queue);
    vkGetDeviceQueue(g->device, g->present_family, 0, &g->present_queue);

    return 0;
}

void gpu_device_destroy(struct ap_gpu *g)
{
    if (g->device) {
        vkDestroyDevice(g->device, NULL);
        g->device = VK_NULL_HANDLE;
    }
}
