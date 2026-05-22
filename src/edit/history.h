#ifndef APERTURE_EDIT_HISTORY_H
#define APERTURE_EDIT_HISTORY_H

#include "edit/stack.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Per-photo undo/redo ring. Snapshots are whole ap_edit_stack copies —
// the struct is fixed-size so a bounded ring of them is cheap (each
// entry is ~75 KB; AP_HISTORY_CAP slots ≈ a few MB per open photo).
//
// Usage pattern:
//   ap_edit_history_snapshot(h, stack)  — call before every mutation
//   ap_edit_history_undo(h, stack)      — Ctrl+Z
//   ap_edit_history_redo(h, stack)      — Ctrl+Shift+Z / Ctrl+Y
//
// The ring stores states to restore TO (captured before each mutation).
// Undo restores the most recent snapshot; redo re-applies after an undo.
// Pushing a new snapshot after an undo discards unreachable redo states
// (standard linear undo semantics).

#define AP_HISTORY_CAP 32

typedef struct {
    ap_edit_stack entries[AP_HISTORY_CAP];
    int           base;    // ring oldest-entry offset
    int           size;    // number of valid entries in [0, AP_HISTORY_CAP]
    int           cursor;  // index into [0, size): most recently restored
                           // entry, or -1 if no entry has been restored
} ap_edit_history;

void ap_edit_history_init(ap_edit_history *h);

// Capture a snapshot of `stack` before mutating it. Any redo states
// beyond the current cursor are discarded.
void ap_edit_history_snapshot(ap_edit_history *h, const ap_edit_stack *stack);

// Restore the previous snapshot into `*stack`. Returns true on
// success; false when there is nothing to undo.
bool ap_edit_history_undo(ap_edit_history *h, ap_edit_stack *stack);

// Re-apply the next snapshot into `*stack`. Returns true on success;
// false when there is nothing to redo.
bool ap_edit_history_redo(ap_edit_history *h, ap_edit_stack *stack);

bool ap_edit_history_can_undo(const ap_edit_history *h);
bool ap_edit_history_can_redo(const ap_edit_history *h);

#ifdef __cplusplus
}
#endif

#endif
