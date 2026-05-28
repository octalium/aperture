project('vulkan-headers', 'c',
  default_options: [
    'warning_level=0',
  ],
)

# Vulkan-Headers ships only headers. The loader is system-provided
# everywhere — vulkan-1.dll with GPU drivers on Windows, the universal
# loader on Linux, MoltenVK on macOS. The vendored headers pin the API
# version; the loader is whatever the host has.
cc = meson.get_compiler('c')

vulkan_inc = include_directories('upstream/include')

if host_machine.system() == 'windows'
  vulkan_loader = cc.find_library('vulkan-1')
else
  vulkan_loader = cc.find_library('vulkan')
endif

vulkan_dep = declare_dependency(
  include_directories: vulkan_inc,
  dependencies: vulkan_loader,
)

meson.override_dependency('vulkan', vulkan_dep)
