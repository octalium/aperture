project('cimgui', 'cpp',
  default_options: [
    'cpp_std=c++17',
    'warning_level=0',
  ],
)

vulkan_dep = dependency('vulkan')
glfw_dep   = dependency('glfw3')

cimgui_inc = include_directories(
  'upstream',
  'upstream/imgui',
  'upstream/imgui/backends',
)

cimgui_cpp_args = [
  # IMGUI_IMPL_API as extern "C" makes backend (glfw/vulkan) function
  # symbols C-callable so our C++ bridge can name them without mangling.
  # Obsolete overloads in the backend headers (e.g. ImGui_ImplVulkan_AddTexture
  # with a sampler) collide with extern "C" because C has no overloading.
  '-DIMGUI_IMPL_API=extern "C"',
  '-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS',
]

cimgui_lib = static_library('cimgui',
  [
    'upstream/cimgui.cpp',
    'upstream/imgui/imgui.cpp',
    'upstream/imgui/imgui_demo.cpp',
    'upstream/imgui/imgui_draw.cpp',
    'upstream/imgui/imgui_tables.cpp',
    'upstream/imgui/imgui_widgets.cpp',
    'upstream/imgui/backends/imgui_impl_glfw.cpp',
    'upstream/imgui/backends/imgui_impl_vulkan.cpp',
  ],
  cpp_args: cimgui_cpp_args,
  include_directories: cimgui_inc,
  dependencies: [vulkan_dep, glfw_dep],
)

cimgui_dep = declare_dependency(
  link_with: cimgui_lib,
  include_directories: cimgui_inc,
  compile_args: [
    '-DCIMGUI_DEFINE_ENUMS_AND_STRUCTS',
    '-DIMGUI_IMPL_API=extern "C"',
    '-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS',
  ],
)

meson.override_dependency('cimgui', cimgui_dep)
