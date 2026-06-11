#include "history.h"

#include <string.h>

// Ring layout:
//   entries[(base + i) % AP_HISTORY_CAP] for i in [0, size) are the
//   valid snapshots, oldest first (i=0) and newest last (i=size-1).
//   cursor is in [-1, size-1]:
//     -1  → no undo applied; live state is beyond the tip of the ring.
//     k≥0 → the snapshot at ring position k has been restored.
//
// Concrete example with AP_HISTORY_CAP=4 (S = live state at undo time):
//   snapshot(A) → size=1, cursor=-1, ring=[A]
//   snapshot(B) → size=2, cursor=-1, ring=[A,B]
//   snapshot(C) → size=3, cursor=-1, ring=[A,B,C]
//   undo        → push live S → ring=[A,B,C,S]; restore ring[2]=C, cursor=2
//   undo        → restore ring[1]=B, cursor=1
//   undo        → restore ring[0]=A, cursor=0 (exhausted)
//   redo        → cursor=1, restore ring[1]=B
//   redo        → cursor=2, restore ring[2]=C
//   redo        → cursor=3, restore ring[3]=S (back to the live state)
//   redo        → cursor==size-1: nothing to redo.
//
// The first undo from the live end (cursor == -1) lazily pushes the
// live state onto the ring before stepping back, so redo can return to
// it. When the ring is full this evicts the oldest snapshot, same as
// any other push.

// append `stack` at the tip of the ring, evicting the oldest entry
// when the ring is full.
static void ring_push(ap_edit_history *h, const ap_edit_stack *stack)
{
    if (h->size < AP_HISTORY_CAP) {
        int slot = (h->base + h->size) % AP_HISTORY_CAP;
        h->entries[slot] = *stack;
        h->size++;
    } else {
        h->entries[h->base] = *stack;
        h->base = (h->base + 1) % AP_HISTORY_CAP;
    }
}

void ap_edit_history_init(ap_edit_history *h)
{
    if (!h) return;
    memset(h, 0, sizeof(*h));
    h->cursor = -1;
}

void ap_edit_history_snapshot(ap_edit_history *h, const ap_edit_stack *stack)
{
    if (!h || !stack) return;

    // Discard redo states beyond the current cursor.
    if (h->cursor >= 0) {
        // Entries 0..cursor-1 are prior states; entry cursor is the live
        // state we were undo'd to. Overwrite at position cursor so the
        // new snapshot replaces the current "live" tip — avoids a
        // redundant duplicate entry when snapshotting after an undo.
        h->size = h->cursor;
    }
    // cursor resets to -1: the live state is now beyond the new tip.
    h->cursor = -1;

    ring_push(h, stack);
}

bool ap_edit_history_can_undo(const ap_edit_history *h)
{
    if (!h || h->size == 0) return false;
    // cursor == -1: live state is beyond the tip; oldest undo is ring[size-1].
    // cursor >= 0:  we are at ring[cursor]; can undo further when cursor > 0.
    return (h->cursor == -1) || (h->cursor > 0);
}

bool ap_edit_history_can_redo(const ap_edit_history *h)
{
    if (!h) return false;
    // Redo is available when we are not already at the tip (cursor < size-1),
    // and we have at least applied one undo (cursor has been set to ≥ 0).
    return (h->cursor >= 0) && (h->cursor < h->size - 1);
}

bool ap_edit_history_undo(ap_edit_history *h, ap_edit_stack *stack)
{
    if (!ap_edit_history_can_undo(h) || !stack) return false;

    if (h->cursor == -1) {
        // First undo from the live end: capture the live state as the
        // new tip so redo can return to it, then step back to the
        // newest snapshot.
        ring_push(h, stack);
        h->cursor = h->size - 2;
    } else {
        h->cursor--;
    }

    int slot = (h->base + h->cursor) % AP_HISTORY_CAP;
    *stack = h->entries[slot];
    return true;
}

bool ap_edit_history_redo(ap_edit_history *h, ap_edit_stack *stack)
{
    if (!ap_edit_history_can_redo(h) || !stack) return false;

    h->cursor++;
    int slot = (h->base + h->cursor) % AP_HISTORY_CAP;
    *stack = h->entries[slot];
    return true;
}
