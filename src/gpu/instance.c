#include "gpu_internal.h"

#include "core/log.h"

#include <stdlib.h>
#include <string.h>

static const char *VALIDATION_LAYERS[] = {
    "VK_LAYER_KHRONOS_validation",
};

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void *user)
{
    (void)type;
    (void)user;

    ap_log_level lvl = AP_LOG_INFO;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        lvl = AP_LOG_ERROR;
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        lvl = AP_LOG_WARN;
    }

    ap_log(lvl, "vk: %s", data->pMessage);
    return VK_FALSE;
}

static bool validation_layers_supported(void)
{
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, NULL);

    VkLayerProperties *layers = calloc(count, sizeof(*layers));
    if (!layers) {
        AP_ERROR("layer enumeration: out of memory");
        return false;
    }
    vkEnumerateInstanceLayerProperties(&count, layers);

    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(layers[i].layerName, VALIDATION_LAYERS[0]) == 0) {
            found = true;
            break;
        }
    }

    free(layers);
    return found;
}

static const VkDebugUtilsMessengerCreateInfoEXT DEBUG_MESSENGER_CI = {
    .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
    .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
    .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                 | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                 | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
    .pfnUserCallback = debug_callback,
};

int gpu_instance_create(struct ap_gpu *g, const char *app_name)
{
    bool want_validation = APERTURE_VALIDATION;
    if (want_validation && !validation_layers_supported()) {
        AP_ERROR("validation layers required by debug build but %s is not available; "
                 "install vulkan-validationlayers (Arch: pacman -S vulkan-validation-layers) "
                 "or build with -Dbuildtype=release",
                 VALIDATION_LAYERS[0]);
        return -1;
    }

    uint32_t glfw_ext_count = 0;
    const char **glfw_exts = glfwGetRequiredInstanceExtensions(&glfw_ext_count);
    if (!glfw_exts) {
        AP_ERROR("glfwGetRequiredInstanceExtensions returned NULL");
        return -1;
    }

    uint32_t ext_count = glfw_ext_count + (want_validation ? 1 : 0);
    const char **exts = calloc(ext_count, sizeof(*exts));
    if (!exts) {
        AP_ERROR("instance extension list: out of memory");
        return -1;
    }
    for (uint32_t i = 0; i < glfw_ext_count; i++) {
        exts[i] = glfw_exts[i];
    }
    if (want_validation) {
        exts[glfw_ext_count] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = app_name,
        .applicationVersion = VK_MAKE_VERSION(0, 0, 1),
        .pEngineName = "aperture",
        .engineVersion = VK_MAKE_VERSION(0, 0, 1),
        .apiVersion = VK_API_VERSION_1_3,
    };

    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = ext_count,
        .ppEnabledExtensionNames = exts,
    };

    if (want_validation) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = VALIDATION_LAYERS;
        ci.pNext = &DEBUG_MESSENGER_CI;
    }

    VkResult res = vkCreateInstance(&ci, NULL, &g->instance);
    free(exts);
    if (res != VK_SUCCESS) {
        AP_ERROR("vkCreateInstance -> %s", gpu_vk_result_str(res));
        return -1;
    }

    if (want_validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                g->instance, "vkCreateDebugUtilsMessengerEXT");
        if (!create_messenger) {
            AP_ERROR("vkGetInstanceProcAddr: vkCreateDebugUtilsMessengerEXT not found");
            return -1;
        }
        VK_CHECK(create_messenger(g->instance, &DEBUG_MESSENGER_CI, NULL,
                                  &g->debug_messenger));
    }

    return 0;
}

void gpu_instance_destroy(struct ap_gpu *g)
{
    if (g->debug_messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                g->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy_messenger) {
            destroy_messenger(g->instance, g->debug_messenger, NULL);
        }
        g->debug_messenger = VK_NULL_HANDLE;
    }
    if (g->instance) {
        vkDestroyInstance(g->instance, NULL);
        g->instance = VK_NULL_HANDLE;
    }
}
