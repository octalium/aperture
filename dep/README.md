# External dependencies

Vendored dependencies live here as Meson wrap files (`*.wrap`). When a
wrap is fetched, Meson populates the matching subdirectory; those
subdirectories are gitignored.

Configured as the project's `subproject_dir` in the root `meson.build`,
overriding Meson's default `subprojects/` location.

Wraps will be added as dependencies are introduced (GLFW, cimgui, etc.).
