project('cimgui', 'cpp',
  default_options: [
    'cpp_std=c++17',
    'warning_level=0',
  ],
)

vulkan_dep = dependency('vulkan')
glfw_dep   = dependency('glfw3')

cimgui_inc = include_directories(
  '.',
  'imgui',
  'imgui/backends',
)

cimgui_cpp_args = [
  # IMGUI_IMPL_API as extern "C" makes backend (glfw/vulkan) function
  # symbols C-callable so our C++ bridge can name them without mangling.
  '-DIMGUI_IMPL_API=extern "C"',
]

cimgui_lib = static_library('cimgui',
  [
    'cimgui.cpp',
    'imgui/imgui.cpp',
    'imgui/imgui_demo.cpp',
    'imgui/imgui_draw.cpp',
    'imgui/imgui_tables.cpp',
    'imgui/imgui_widgets.cpp',
    'imgui/backends/imgui_impl_glfw.cpp',
    'imgui/backends/imgui_impl_vulkan.cpp',
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
  ],
)

meson.override_dependency('cimgui', cimgui_dep)
