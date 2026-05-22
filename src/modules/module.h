#ifndef APERTURE_MODULE_H
#define APERTURE_MODULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/canvas_tool.h"
#include "edit/stack.h"
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
// like demosaic / encode with no user-visible parameters).
// `str_params` is the focused edit entry's string params (paths etc.);
// NULL for transport modules with no entry — parallel to `params`.
// `meta` carries static per-image data (raw camera matrix, black levels,
// etc.) and may be NULL for modules that don't need it. Return 0 to
// dispatch normally; nonzero to skip this module this frame.
typedef int (*ap_module_pack_push_fn)(const ap_module *self,
                                      const float *params,
                                      const char (*str_params)[AP_EDIT_STR_LEN],
                                      const ap_raw_metadata *meta,
                                      void *push_out);

// Panel-side context handed to render_params: data the panel knows
// but the module doesn't carry. Grows as more modules want panel
// context.
typedef struct {
    int image_width;        // open photo's pixel dimensions — Crop
    int image_height;       // presents its rect in pixels

    // The focused entry's string params (paths etc.). Modules with
    // str_params_count > 0 read and write these in place; the buffers
    // belong to the edit entry, so edits persist with the stack.
    char (*str_params)[AP_EDIT_STR_LEN];

    // render_params sets *request_rebuild to ask the panel to rebuild
    // the pipeline graph — needed when an edit changes graph structure
    // rather than just a push constant (e.g. committing a profile path
    // so a LUT-bearing variant re-bakes). Never NULL.
    bool *request_rebuild;

    // render_params sets *snapshot_requested when a slider drag begins
    // (igIsItemActivated for any slider). The config window takes an
    // undo snapshot once per drag-start so continuous slider motion is a
    // single undo step. Never NULL.
    bool *snapshot_requested;

    // render_params writes an ap_canvas_tool here to ask the app to
    // arm an interactive canvas tool (white-balance eyedropper,
    // interactive crop). The config window forwards the request to the
    // app, which owns the tool state and drives the canvas overlay.
    // Pre-set to the currently-armed tool so a module can both read
    // the live state (to draw an active/inactive toggle) and change
    // it. Never NULL.
    ap_canvas_tool *request_canvas_tool;
} ap_module_render_ctx;

// Render an ImGui config widget for the module's per-instance
// parameter slots. Called from the focused-edit panel when the user
// selects this entry on the stack. NULL for transport modules. `ctx`
// is never NULL — see ap_module_render_ctx.
typedef void (*ap_module_render_params_fn)(const ap_module *self,
                                           float *params,
                                           const ap_module_render_ctx *ctx);

// Bake a variant's colour LUT.
//
// Called once, when the pipeline graph is built, for a variant that
// declares `bake_lut`. The graph supplies `out_lut`, a buffer of
// AP_ICC_LUT_DIM^3 * 4 floats (see color/icc.h); the module fills it
// with an RGBA 3D LUT the variant's shader samples at binding 2.
// `params` / `str_params` / `meta` describe the scheduling edit entry,
// exactly as for pack_push. Return 0 when the buffer was filled;
// non-zero leaves it to the graph, which substitutes an identity LUT
// so the stage becomes a pass-through.
typedef int (*ap_module_bake_lut_fn)(const float *params,
                                     const char (*str_params)[AP_EDIT_STR_LEN],
                                     const ap_raw_metadata *meta,
                                     float *out_lut);

// Buffers a multi-pass variant's passes can read from / write to.
// IN is the buffer the module received; OUT is the buffer the
// module must leave its result in; SCRATCH0..2 are module-private
// intermediates the pipeline graph allocates. A pass never writes
// IN — the module's input stays intact for any later pass that
// needs it (e.g. an unsharp-mask combine).
typedef enum {
    AP_PASS_BUF_IN       = 0,
    AP_PASS_BUF_OUT      = 1,
    AP_PASS_BUF_SCRATCH0 = 2,
    AP_PASS_BUF_SCRATCH1 = 3,
    AP_PASS_BUF_SCRATCH2 = 4,
} ap_pass_buf;

#define AP_MODULE_MAX_SCRATCH 3

// One compute pass of a multi-pass variant. `read0` feeds binding 0,
// `read1` binding 2 (the aux input — set it equal to read0 when the
// pass needs only one input), `write` binding 1.
typedef struct {
    const uint32_t        *spv_data;
    size_t                 spv_size;
    size_t                 push_size;
    ap_module_pack_push_fn pack_push;
    ap_pass_buf            read0;
    ap_pass_buf            read1;
    ap_pass_buf            write;
} ap_module_pass;

// One algorithm variant. A module either declares `variants[]` here
// (modern) or fills the legacy single-shader fields below (transport
// modules + simple single-algorithm modules). When variants exist, the
// pipeline graph picks the active variant via
// `int(params[variant_param_slot])`, clamped to [0, variant_count).
//
// A variant is single-pass (spv_data set, pass_count == 0) or
// multi-pass (passes[] set, pass_count > 0). A multi-pass variant
// expands into `pass_count` pipeline-graph stages; `scratch_count`
// (<= AP_MODULE_MAX_SCRATCH) full-resolution intermediates are
// allocated for it.
typedef struct {
    const char *display_name;        // shown in the variant combo
    const uint32_t *spv_data;
    size_t          spv_size;
    size_t          push_size;        // may differ between variants
    ap_module_pack_push_fn pack_push; // variant-specific packing

    int                   pass_count;
    const ap_module_pass *passes;
    int                   scratch_count;

    // When set, this variant samples a colour LUT at binding 2. The
    // graph calls bake_lut at build time, uploads the result into a
    // small image, and routes it to the stage's aux binding.
    ap_module_bake_lut_fn bake_lut;
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

    // Per-instance string parameters — parallel to the float params
    // above but carried in ap_edit_entry::str_params (paths, etc.).
    // str_params_count <= AP_EDIT_STR_SLOTS; str_params_names gives
    // the sidecar field name for each slot.
    int                  str_params_count;
    const char *const   *str_params_names;

    // Algorithm variants. variant_count > 0 routes the pipeline graph
    // through `variants[clamped_idx]` for shader bytecode + push size
    // + pack_push. The active variant index is read from
    // `params[variant_param_slot]` (cast to int); changing the slot
    // triggers a graph rebuild — the panel layer detects the change.
    int                      variant_count;
    const ap_module_variant *variants;
    int                      variant_param_slot;

    // Optional post-init hook called by ap_edit_stack_add after copying
    // params_default into the new entry. Modules that need per-instance
    // randomisation (e.g. Grain seed) set slots here. NULL when unused.
    void (*init_instance)(float *params);
};

// Convenience widget for module render_params: a Combo binding to
// `params[module->variant_param_slot]`. Returns true when the user
// changed the variant; render_params bodies typically forward this
// signal up via the panel's existing rebuild plumbing (the panel
// snapshots the slot around the render_params call so a manual
// return is optional but tidy).
bool ap_module_render_variant_combo(const ap_module *self, float *params);

// Shared slider widget used by all module render_params implementations.
// Draws igSliderFloat with AlwaysClamp (so out-of-range edits are
// clamped on input) and with ImGui's default speed-tweak support active
// (Ctrl-drag = fine control, Shift-drag = coarse). Double-click resets
// the slot to its default value.
//
// When a render context is active (ap_module_render_ctx_push was
// called) and a drag begins (igIsItemActivated), the slider sets
// ctx->snapshot_requested so the config window can snapshot the
// pre-drag stack state for undo coalescing.
void ap_module_slider_reset(const ap_module *self, float *params,
                            const char *label, int slot,
                            float lo, float hi, const char *fmt);

// Set / clear the render context that ap_module_slider_reset reads to
// signal drag activation. Called by the config window around
// render_params so the snapshot path has access without changing the
// slider helper's call signature.
void ap_module_render_ctx_push(const ap_module_render_ctx *ctx);
void ap_module_render_ctx_pop(void);

// Resolved per-stage view of which shader + push layout + pack_push to
// use. For modules with no variants, mirrors the legacy single-shader
// fields. For variant-bearing modules, returns the picked variant.
// Pipeline graph code uses this so it doesn't have to know about
// variants beyond the call site.
//
// When `pass_count > 0` the active variant is multi-pass: the
// `spv_data`/`push_size`/`pack_push` triple is unused and the graph
// walks `passes[]` instead.
typedef struct {
    const uint32_t        *spv_data;
    size_t                 spv_size;
    size_t                 push_size;
    ap_module_pack_push_fn pack_push;
    const char            *display_name;   // variant name; "" when none
    int                    variant_idx;    // resolved index; 0 when none

    int                    pass_count;     // 0 = single-pass
    const ap_module_pass  *passes;
    int                    scratch_count;
    ap_module_bake_lut_fn  bake_lut;        // non-NULL: stage wants a LUT
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
