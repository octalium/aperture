#ifndef APERTURE_TEST_APTEST_MODULES_H
#define APERTURE_TEST_APTEST_MODULES_H

// Fixture module registry for tests that exercise edit-stack / sidecar
// pack-unpack without pulling in the real GPU / cimgui / lensfun-bearing
// modules. The TUs that compose this header also become the test's
// definitions of ap_module_registry / ap_module_find, replacing the
// production registry.c which is too heavy to link from tests.
//
// Modules here mirror the *shape* of their production counterparts —
// same name, same params_count, same params_names — so the stack TOML
// reader/writer behaves identically. Shader bytecode and render_params
// are intentionally absent: tests never dispatch.

#include "modules/module.h"

#include <stddef.h>

// Default pipeline modules (referenced by ap_pipeline_apply_default_to_stack
// in library.c) — each has zero numeric params, mirroring the
// "transport-ish" production modules whose state lives entirely in
// shaders.

extern const ap_module aptest_module_demosaic;
extern const ap_module aptest_module_wb;
extern const ap_module aptest_module_profile;

// Three param-heavy modules mirroring the production shapes for
// color_grade, lens_correction, chromatic_aberration. These are what
// the module pack/unpack round-trip test exercises.

extern const ap_module aptest_module_color_grade;
extern const ap_module aptest_module_lens_correction;
extern const ap_module aptest_module_chromatic_aberration;

#endif
