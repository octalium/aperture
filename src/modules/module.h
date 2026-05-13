#ifndef APERTURE_MODULE_H
#define APERTURE_MODULE_H

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
// Modules read `edit` for user-controlled state and `meta` for static
// per-image data (raw camera matrix, black levels, etc.). `meta` may be
// NULL for modules that don't need it. Return 0 to dispatch normally;
// nonzero to skip this module this frame.
typedef int (*ap_module_pack_push_fn)(const ap_module *self,
                                      const ap_edit_state *edit,
                                      const ap_raw_metadata *meta,
                                      void *push_out);

struct ap_module {
    const char *name;          // unique identifier, lowercase, e.g. "exposure"
    const char *display_name;  // human-readable, e.g. "Exposure"
    ap_module_category category;

    // Compute path. NULL spv_data marks a non-GPU module (CPU only); for
    // those the dispatch/push fields are unused. v1 modules are all GPU.
    const uint32_t *spv_data;
    size_t          spv_size;
    size_t          push_size;
    ap_module_pack_push_fn pack_push;
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
