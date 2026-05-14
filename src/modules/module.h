#ifndef APERTURE_MODULE_H
#define APERTURE_MODULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpu/gpu.h"
#include "io/raw.h"

#ifdef __cplusplus
extern "C" {
#endif

// Categories shape both the module's role in the pipeline graph and which
// mode's UI hosts its controls. Adding a category is a single enum entry
// plus, optionally, scheduling rules in the graph.
typedef enum {
    AP_MODULE_INPUT,            // CPU-side adapters: file decoders, raw upload
    AP_MODULE_COLOR,            // demosaic, white balance, color profile
    AP_MODULE_TONE,             // exposure, contrast, sigmoid/filmic
    AP_MODULE_GEOMETRIC,        // crop, rotate, lens correction, perspective
    AP_MODULE_DETAIL,           // sharpening, noise reduction (incl. AI)
    AP_MODULE_OUTPUT_TRANSFER,  // linear → display transfer (sRGB / PQ / HLG / linear)
    AP_MODULE_OUTPUT,           // file writers (JPEG / TIFF / PNG)
    AP_MODULE_METADATA,         // EXIF write, GPX, AI classification
    AP_MODULE_LIBRARY,          // import, group ops
} ap_module_category;

typedef struct ap_module ap_module;

// Fill a per-dispatch push-constant blob (size == module->push_size).
// `params` is the per-instance parameter slot array carried by the
// edit entry that scheduled this module (NULL for transport modules
// like demosaic / encode with no user-visible parameters). `meta`
// carries static per-image data (raw camera matrix, black levels,
// etc.) and may be NULL for modules that don't need it. Return 0 to
// dispatch normally; nonzero to skip this module this frame.
typedef int (*ap_module_pack_push_fn)(const ap_module *self,
                                      const float *params,
                                      const ap_raw_metadata *meta,
                                      void *push_out);

// Render an ImGui config widget for the module's per-instance
// parameter slots. Called from the focused-edit panel when the user
// selects this entry on the stack. NULL for transport modules.
typedef void (*ap_module_render_params_fn)(const ap_module *self,
                                           float *params);

struct ap_module {
    const char *name;          // unique identifier, lowercase, e.g. "exposure"
    const char *display_name;  // human-readable, e.g. "Exposure"
    ap_module_category category;

    // false marks transport modules that always run as part of the
    // graph (demosaic, encode) and aren't shown in the tools palette.
    // true marks user-facing edits.
    bool user_visible;

    // Compute path. NULL spv_data marks a non-GPU module (CPU only); for
    // those the dispatch/push fields are unused. v1 modules are all GPU.
    const uint32_t *spv_data;
    size_t          spv_size;
    size_t          push_size;
    ap_module_pack_push_fn pack_push;

    // Per-instance parameters. params_count <= AP_EDIT_PARAMS_SLOTS.
    // params_default[params_count] seeds new stack entries.
    // params_names[params_count] gives sidecar field names (one per
    // slot); it doubles as the ground truth for what each slot means.
    int                  params_count;
    const float         *params_default;
    const char *const   *params_names;
    ap_module_render_params_fn render_params;
};

// Built-in module registry - NULL-terminated array of pointers. Defined
// in src/modules/registry.c. Adding a new module is "create file +
// add one entry here + add to meson.build".
extern const ap_module *const ap_module_registry[];

// Lookup by name. Returns NULL if no module matches.
const ap_module *ap_module_find(const char *name);

#ifdef __cplusplus
}
#endif

#endif
