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

// Fill a per-dispatch push-constant blob (size == module->push_size,
// or the variant's push_size when the module declares variants).
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

// Panel-side context handed to render_params: data the panel knows
// but the module doesn't carry. Currently the open photo's pixel
// dimensions — Crop uses them to present its rect in pixels. Grows
// as more modules want panel context.
typedef struct {
    int image_width;
    int image_height;
} ap_module_render_ctx;

// Render an ImGui config widget for the module's per-instance
// parameter slots. Called from the focused-edit panel when the user
// selects this entry on the stack. NULL for transport modules. `ctx`
// is never NULL — see ap_module_render_ctx.
typedef void (*ap_module_render_params_fn)(const ap_module *self,
                                           float *params,
                                           const ap_module_render_ctx *ctx);

// One algorithm variant. A module either declares `variants[]` here
// (modern) or fills the legacy single-shader fields below (transport
// modules + simple single-algorithm modules). When variants exist, the
// pipeline graph picks the active variant via
// `int(params[variant_param_slot])`, clamped to [0, variant_count).
typedef struct {
    const char *display_name;        // shown in the variant combo
    const uint32_t *spv_data;
    size_t          spv_size;
    size_t          push_size;        // may differ between variants
    ap_module_pack_push_fn pack_push; // variant-specific packing
} ap_module_variant;

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
    // When `variant_count > 0` these fields are ignored and the
    // pipeline graph uses the per-variant copies instead.
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

    // Algorithm variants. variant_count > 0 routes the pipeline graph
    // through `variants[clamped_idx]` for shader bytecode + push size
    // + pack_push. The active variant index is read from
    // `params[variant_param_slot]` (cast to int); changing the slot
    // triggers a graph rebuild — the panel layer detects the change.
    int                      variant_count;
    const ap_module_variant *variants;
    int                      variant_param_slot;
};

// Convenience widget for module render_params: a Combo binding to
// `params[module->variant_param_slot]`. Returns true when the user
// changed the variant; render_params bodies typically forward this
// signal up via the panel's existing rebuild plumbing (the panel
// snapshots the slot around the render_params call so a manual
// return is optional but tidy).
bool ap_module_render_variant_combo(const ap_module *self, float *params);

// Resolved per-stage view of which shader + push layout + pack_push to
// use. For modules with no variants, mirrors the legacy single-shader
// fields. For variant-bearing modules, returns the picked variant.
// Pipeline graph code uses this so it doesn't have to know about
// variants beyond the call site.
typedef struct {
    const uint32_t        *spv_data;
    size_t                 spv_size;
    size_t                 push_size;
    ap_module_pack_push_fn pack_push;
    const char            *display_name;   // variant name; "" when none
    int                    variant_idx;    // resolved index; 0 when none
} ap_module_active;

// Resolve which shader + pack_push to use given the current runtime
// params. Reads params[variant_param_slot] (cast to int) for
// variant-bearing modules; falls back to the legacy fields otherwise.
// Safe to call with params == NULL (transport modules); returns
// variant 0 (or legacy fields) in that case.
void ap_module_resolve(const ap_module *self, const float *params,
                       ap_module_active *out);

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
