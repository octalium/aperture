// angle brackets, not quotes: this file lives in src/ui/ next to
// aperture's own ui/imgui.h C wrapper, and MSVC's quote-include rule
// searches the including file's directory first — which would resolve
// "imgui.h" to the wrapper instead of the upstream Dear ImGui header.
// angle brackets force the -I (cimgui) search path on every compiler.
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include <vulkan/vulkan.h>

#include <cstdint>

extern "C" {
#include "app/root.h"
#include "core/log.h"
#include "ui/imgui.h"
}

namespace {
VkDevice         g_device          = VK_NULL_HANDLE;
VkDescriptorPool g_descriptor_pool = VK_NULL_HANDLE;
// Backing store for io.IniFilename — ImGui keeps the pointer, not a copy.
char             g_ini_path[4096]  = {0};
}

extern "C" bool ap_imgui_init(GLFWwindow *window,
                              VkInstance instance,
                              VkPhysicalDevice physical,
                              VkDevice device,
                              uint32_t queue_family,
                              VkQueue queue,
                              uint32_t image_count,
                              VkFormat color_format)
{
    g_device = device;

    // The Vulkan backend builds two separate descriptor-set layouts: one
    // VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE set per registered texture (font
    // atlas + every ImGui_ImplVulkan_AddTexture caller) and a small fixed
    // number of VK_DESCRIPTOR_TYPE_SAMPLER sets for the shared samplers.
    // The pool must declare both types or spec-compliant drivers will
    // return VK_ERROR_OUT_OF_POOL_MEMORY.
    const uint32_t sampled_image_count = 1000;
    const uint32_t sampler_count       = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE;
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampled_image_count },
        { VK_DESCRIPTOR_TYPE_SAMPLER,       sampler_count       },
    };
    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_ci.maxSets       = sampled_image_count + sampler_count;
    pool_ci.poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]);
    pool_ci.pPoolSizes    = pool_sizes;

    if (vkCreateDescriptorPool(device, &pool_ci, nullptr, &g_descriptor_pool) != VK_SUCCESS) {
        AP_ERROR("vkCreateDescriptorPool for imgui failed");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Auto-persist the working layout to <app_config>/imgui.ini — ImGui
    // loads it on the first frame and saves changes back, so panel
    // arrangement is remembered across sessions. Named layout profiles
    // (src/app/layout_profiles.c) are explicit snapshots layered on
    // top of this always-remembered layout.
    if (ap_app_config_ensure() == 0 &&
        ap_app_config_join("imgui.ini", g_ini_path, sizeof(g_ini_path)) == 0) {
        io.IniFilename = g_ini_path;
    } else {
        io.IniFilename = nullptr;   // no config root — run without persistence
    }

    ImGui::StyleColorsDark();

    if (!ImGui_ImplGlfw_InitForVulkan(window, true)) {
        AP_ERROR("ImGui_ImplGlfw_InitForVulkan failed");
        return false;
    }

    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.ApiVersion          = VK_API_VERSION_1_3;
    init_info.Instance            = instance;
    init_info.PhysicalDevice      = physical;
    init_info.Device              = device;
    init_info.QueueFamily         = queue_family;
    init_info.Queue               = queue;
    init_info.DescriptorPool      = g_descriptor_pool;
    init_info.MinImageCount       = image_count;
    init_info.ImageCount          = image_count;
    init_info.UseDynamicRendering = true;

    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;

    if (!ImGui_ImplVulkan_Init(&init_info)) {
        AP_ERROR("ImGui_ImplVulkan_Init failed");
        return false;
    }

    return true;
}

extern "C" void ap_imgui_shutdown(void)
{
    if (g_device == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(g_device);

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    // Capture the final layout — ImGui's periodic auto-save may not
    // have caught a change made just before quitting.
    if (ImGui::GetIO().IniFilename != nullptr) {
        ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
    }
    ImGui::DestroyContext();

    if (g_descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g_device, g_descriptor_pool, nullptr);
        g_descriptor_pool = VK_NULL_HANDLE;
    }
    g_device = VK_NULL_HANDLE;
}

extern "C" void ap_imgui_new_frame(void)
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

extern "C" void ap_imgui_render(VkCommandBuffer cmd)
{
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd, VK_NULL_HANDLE);
}

extern "C" void ap_imgui_discard_frame(void)
{
    ImGui::EndFrame();
}

extern "C" uint64_t ap_imgui_register_texture(VkSampler sampler,
                                              VkImageView view,
                                              VkImageLayout layout)
{
    // The Vulkan backend now sources the sampler from
    // ImGui_ImplVulkan_InitInfo (or a per-texture sampler set elsewhere).
    // Wrapper signature is kept for callers; sampler arg is unused.
    (void)sampler;
    VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture(view, layout);
    return reinterpret_cast<uint64_t>(ds);
}

extern "C" void ap_imgui_unregister_texture(uint64_t tex_id)
{
    if (tex_id == 0) {
        return;
    }
    VkDescriptorSet ds = reinterpret_cast<VkDescriptorSet>(tex_id);
    ImGui_ImplVulkan_RemoveTexture(ds);
}

extern "C" void ap_imgui_load_layout(const char *path)
{
    if (!path) return;
    ImGui::LoadIniSettingsFromDisk(path);
}

extern "C" void ap_imgui_save_layout(const char *path)
{
    if (!path) return;
    ImGui::SaveIniSettingsToDisk(path);
}

// cimgui exposes the runtime-clear helper as the flat `igClearIniSettings`;
// the upstream C++ binding doesn't ship it under the `ImGui::` namespace
// in this header, so reach for the flat symbol directly.
extern "C" void igClearIniSettings(void);

extern "C" void ap_imgui_clear_settings(void)
{
    igClearIniSettings();
}
