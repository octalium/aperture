// Undo/redo ring semantics for ap_edit_history. Exercises the lazy
// live-state capture on first undo (redo must return to the live
// state), snapshot-after-undo truncation, and cap eviction. Stacks are
// distinguished by next_id — no module registry needed.

#include "aptest.h"

#include "edit/history.h"

#include <string.h>

// build a stack whose identity is the given marker value.
static ap_edit_stack mk(uint32_t marker)
{
    ap_edit_stack s;
    memset(&s, 0, sizeof(s));
    s.next_id = marker;
    return s;
}

static void test_empty(void)
{
    ap_edit_history h;
    ap_edit_history_init(&h);
    ap_edit_stack live = mk(1);

    AP_TEST_ASSERT(!ap_edit_history_can_undo(&h), "empty history can_undo");
    AP_TEST_ASSERT(!ap_edit_history_can_redo(&h), "empty history can_redo");
    AP_TEST_ASSERT(!ap_edit_history_undo(&h, &live), "undo on empty history");
    AP_TEST_ASSERT(!ap_edit_history_redo(&h, &live), "redo on empty history");
    AP_TEST_ASSERT(live.next_id == 1, "stack mutated by failed undo/redo");
}

static void test_undo_redo_live_state(void)
{
    ap_edit_history h;
    ap_edit_history_init(&h);

    // snapshot before each mutation: A → B → C → live S.
    ap_edit_stack live = mk(10);
    ap_edit_history_snapshot(&h, &live);  // A = 10
    live = mk(20);
    ap_edit_history_snapshot(&h, &live);  // B = 20
    live = mk(30);
    ap_edit_history_snapshot(&h, &live);  // C = 30
    live = mk(40);                        // S = 40, never snapshotted

    AP_TEST_ASSERT(!ap_edit_history_can_redo(&h), "can_redo before any undo");

    // first undo restores C and must make redo (back to S) available.
    AP_TEST_ASSERT(ap_edit_history_undo(&h, &live), "undo to C");
    AP_TEST_ASSERT(live.next_id == 30, "undo restored %u, want 30", live.next_id);
    AP_TEST_ASSERT(ap_edit_history_can_redo(&h), "can_redo after first undo");

    AP_TEST_ASSERT(ap_edit_history_redo(&h, &live), "redo to live state");
    AP_TEST_ASSERT(live.next_id == 40, "redo restored %u, want 40 (live)", live.next_id);
    AP_TEST_ASSERT(!ap_edit_history_can_redo(&h), "can_redo at live tip");

    // walk all the way down and back up.
    AP_TEST_ASSERT(ap_edit_history_undo(&h, &live) && live.next_id == 30, "undo to C");
    AP_TEST_ASSERT(ap_edit_history_undo(&h, &live) && live.next_id == 20, "undo to B");
    AP_TEST_ASSERT(ap_edit_history_undo(&h, &live) && live.next_id == 10, "undo to A");
    AP_TEST_ASSERT(!ap_edit_history_can_undo(&h), "can_undo past oldest");
    AP_TEST_ASSERT(ap_edit_history_redo(&h, &live) && live.next_id == 20, "redo to B");
    AP_TEST_ASSERT(ap_edit_history_redo(&h, &live) && live.next_id == 30, "redo to C");
    AP_TEST_ASSERT(ap_edit_history_redo(&h, &live) && live.next_id == 40, "redo to S");
    AP_TEST_ASSERT(!ap_edit_history_can_redo(&h), "can_redo at live tip");
}

static void test_snapshot_after_undo_truncates(void)
{
    ap_edit_history h;
    ap_edit_history_init(&h);

    ap_edit_stack live = mk(10);
    ap_edit_history_snapshot(&h, &live);  // A
    live = mk(20);
    ap_edit_history_snapshot(&h, &live);  // B
    live = mk(30);                        // S

    AP_TEST_ASSERT(ap_edit_history_undo(&h, &live) && live.next_id == 20, "undo to B");
    AP_TEST_ASSERT(ap_edit_history_can_redo(&h), "can_redo after undo");

    // new mutation from B: snapshot discards the redo branch (S).
    ap_edit_history_snapshot(&h, &live);
    live = mk(50);
    AP_TEST_ASSERT(!ap_edit_history_can_redo(&h), "redo branch survived snapshot");

    AP_TEST_ASSERT(ap_edit_history_undo(&h, &live) && live.next_id == 20, "undo to B");
    AP_TEST_ASSERT(ap_edit_history_redo(&h, &live) && live.next_id == 50, "redo to new live");
}

static void test_cap_eviction(void)
{
    ap_edit_history h;
    ap_edit_history_init(&h);

    // fill the ring past capacity; the oldest snapshots are evicted.
    ap_edit_stack live;
    for (uint32_t i = 0; i < AP_HISTORY_CAP + 4; i++) {
        live = mk(100 + i);
        ap_edit_history_snapshot(&h, &live);
    }
    live = mk(999);  // live state

    // first undo pushes the live state, evicting one more snapshot;
    // AP_HISTORY_CAP - 1 older states remain reachable below it.
    uint32_t newest = 100 + AP_HISTORY_CAP + 4 - 1;
    for (uint32_t i = 0; i < AP_HISTORY_CAP - 1; i++) {
        AP_TEST_ASSERT(ap_edit_history_undo(&h, &live),
                       "undo %u of %d", i + 1, AP_HISTORY_CAP - 1);
        AP_TEST_ASSERT(live.next_id == newest - i,
                       "undo %u restored %u, want %u",
                       i + 1, live.next_id, newest - i);
    }
    AP_TEST_ASSERT(!ap_edit_history_can_undo(&h), "can_undo past evicted entries");

    // redo walks back up to the captured live state.
    for (uint32_t i = AP_HISTORY_CAP - 2; i > 0; i--) {
        AP_TEST_ASSERT(ap_edit_history_redo(&h, &live), "redo");
        AP_TEST_ASSERT(live.next_id == newest - i + 1,
                       "redo restored %u, want %u", live.next_id, newest - i + 1);
    }
    AP_TEST_ASSERT(ap_edit_history_redo(&h, &live) && live.next_id == 999,
                   "redo to live state after eviction");
    AP_TEST_ASSERT(!ap_edit_history_can_redo(&h), "can_redo at live tip");
}

int main(void)
{
    test_empty();
    test_undo_redo_live_state();
    test_snapshot_after_undo_truncates();
    test_cap_eviction();
    return 0;
}
