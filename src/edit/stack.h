#ifndef APERTURE_EDIT_STACK_H
#define APERTURE_EDIT_STACK_H

#include <stdbool.h>

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
#define AP_EDIT_PARAMS_SLOTS 8
#define AP_EDIT_STACK_MAX   32

typedef struct {
    char  module_name[AP_EDIT_NAME_LEN];
    float params[AP_EDIT_PARAMS_SLOTS];
    bool  enabled;
    bool  show_config;   // UI-only, not persisted: is the config
                         // window visible for this entry?
} ap_edit_entry;

typedef struct ap_edit_stack {
    ap_edit_entry entries[AP_EDIT_STACK_MAX];
    int           count;
    int           focus;     // -1 when nothing focused
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

// Swap entry at idx with its neighbor in direction dir (+1 = down,
// -1 = up). Out-of-bounds moves are no-ops.
void           ap_edit_stack_move(ap_edit_stack *s, int idx, int dir);

// Toggle the enabled flag of entry at idx.
void           ap_edit_stack_set_enabled(ap_edit_stack *s, int idx, bool enabled);

int            ap_edit_stack_count(const ap_edit_stack *s);
ap_edit_entry *ap_edit_stack_at(ap_edit_stack *s, int idx);
const ap_edit_entry *ap_edit_stack_at_const(const ap_edit_stack *s, int idx);

void           ap_edit_stack_set_focus(ap_edit_stack *s, int idx);
int            ap_edit_stack_focus(const ap_edit_stack *s);

#ifdef __cplusplus
}
#endif

#endif
