#ifndef APERTURE_EDIT_STACK_H
#define APERTURE_EDIT_STACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Per-photo ordered list of edit entries. Each entry references a
// module by name and carries an instance of that module's parameters
// (a flat float array — modules document the slot layout via
// ap_module::params_names).
//
// The pipeline graph is rebuilt whenever the stack changes (add /
// remove / move / enable). Disabled entries are skipped at build time
// so they neither cost a dispatch nor allocate working buffers.

#define AP_EDIT_NAME_LEN    32
// Per-entry param capacity. Modules with more knobs (HSL, lens
// correction, color grading wheels) need elbow room beyond the
// initial 8. Bumping the cap keeps every entry the same shape — a
// few hundred extra bytes per entry, no variable-size blob plumbing
// to maintain. Cross-reference #108.
#define AP_EDIT_PARAMS_SLOTS 32
#define AP_EDIT_STACK_MAX   32

#define AP_EDIT_DISPLAY_LEN 64

// Per-entry string parameters — text a module needs that can't live
// in the float params blob (e.g. Color Profile's .icc / .dcp path).
// Fixed-size for the same reason the float params are: every entry
// stays the same shape, no variable-size blob plumbing. A module
// declares how many slots it uses via ap_module::str_params_count.
#define AP_EDIT_STR_SLOTS 2
#define AP_EDIT_STR_LEN   512

typedef struct {
    char     module_name[AP_EDIT_NAME_LEN];
    char     display_name[AP_EDIT_DISPLAY_LEN]; // user-set; empty falls
                                                // back to module's name
                                                // plus an auto-suffix
                                                // to disambiguate dupes
    float    params[AP_EDIT_PARAMS_SLOTS];
    char     str_params[AP_EDIT_STR_SLOTS][AP_EDIT_STR_LEN];
    bool     enabled;
    bool     show_config;   // UI-only, not persisted: is the config
                            // window visible for this entry?
    uint32_t id;            // stable per-session identity; assigned on
                            // ap_edit_stack_add, survives reorder. Used
                            // to key per-entry ImGui window IDs so window
                            // state follows the entry on drag-reorder.
} ap_edit_entry;

typedef struct ap_edit_stack {
    ap_edit_entry entries[AP_EDIT_STACK_MAX];
    int           count;
    int           focus;     // -1 when nothing focused
    uint32_t      next_id;   // monotonic counter for entry IDs
} ap_edit_stack;

// Reset to empty, focus = -1.
void           ap_edit_stack_init(ap_edit_stack *s);

// Append an entry for the named module, default-initialized via the
// module's params_default. Returns the new entry's index, or -1 on
// failure (unknown module, stack full).
int            ap_edit_stack_add(ap_edit_stack *s, const char *module_name);

// Remove the entry at idx. Focus follows reasonable semantics: if the
// focused entry is removed, focus shifts to the previous entry (or -1
// when empty).
void           ap_edit_stack_remove(ap_edit_stack *s, int idx);

// Move the entry currently at `src` to position `dst`, shifting the
// entries in between. Out-of-bounds or no-op moves are silent.
void           ap_edit_stack_reorder(ap_edit_stack *s, int src, int dst);

// Toggle the enabled flag of entry at idx.
void           ap_edit_stack_set_enabled(ap_edit_stack *s, int idx, bool enabled);

int            ap_edit_stack_count(const ap_edit_stack *s);
ap_edit_entry *ap_edit_stack_at(ap_edit_stack *s, int idx);
const ap_edit_entry *ap_edit_stack_at_const(const ap_edit_stack *s, int idx);

void           ap_edit_stack_set_focus(ap_edit_stack *s, int idx);
int            ap_edit_stack_focus(const ap_edit_stack *s);

// Reset the entry at idx to its module's default params. No-op for
// modules with no params or unknown module. Returns 0 on success.
int            ap_edit_stack_reset(ap_edit_stack *s, int idx);

// The label shown in the Edits row / config window title. Returns
// `entry->display_name` when non-empty, otherwise the module's
// display_name with an auto-suffix when the same module appears
// multiple times on the stack ("Exposure", "Exposure 2", ...).
// Caller-supplied buffer; safe truncation.
const char    *ap_edit_stack_label_at(const ap_edit_stack *s, int idx,
                                      char *buf, size_t buflen);

// True when the supplied name is unique in the stack (or not used
// anywhere except by `ignore_idx`, which is treated as the caller's
// own entry for rename validation). Compares against both
// display_name and the auto-suffix labels.
bool           ap_edit_stack_name_unique(const ap_edit_stack *s,
                                         const char *candidate,
                                         int ignore_idx);

#ifdef __cplusplus
}
#endif

#endif
