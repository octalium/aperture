---
name: add-preferences-section
description: Add a new configurable settings section to the Preferences modal. Use this when implementing a feature that has user-tunable behavior (defaults, paths, formats, etc.) that should persist across runs. Captures the load-once-at-startup pattern (avoid per-frame DB reads), the ap_settings_* persistence layer, the modal UI conventions, and the cache-update flow on save.
---

Pattern reference: `Quick Export` (added in PR #412 for issue #394). When in doubt, mirror that section.

## When to use

- A feature has a knob that doesn't change per-photo / per-session and should persist across app restarts.
- The user should be able to discover and change the knob from Preferences.
- The setting is global, not per-library (per-library settings have their own pattern via `ap_library_settings_*`; this skill is for app-wide prefs).

## Architecture

The pattern has three layers:

1. **Settings struct + persistence** in a feature module (e.g. `src/output/export.h` defines `ap_quick_export_settings`).
2. **Cached accessor on `ap_app`** (e.g. `ap_app_quick_export_settings(app)`) — loaded once at app init, mutated by the modal, written back via `ap_quick_export_save`.
3. **Modal UI** in `src/app/preferences_modal.c` — a new `igSeparatorText` section + form widgets bound to the cached struct.

### Why a cache, not direct sqlite reads

The cardinal mistake (caught in PR #412 review): if your tooltip / hot-path reads call `ap_settings_get` directly, every call does sqlite open/prepare/finalize/close. On a tooltip that polls every frame, that's hundreds of sqlite operations per second. Always read from the cached struct on `ap_app`; the modal writes through the cache + persists once on Save.

## Steps

### 1. Define the settings struct + persistence API

Pick a home in the feature's module. Conventions:

```c
// src/output/<feature>.h or wherever your feature lives
typedef struct {
    /* fields */
} ap_<feature>_settings;

void ap_<feature>_settings_defaults(ap_<feature>_settings *out);
void ap_<feature>_load(ap_<feature>_settings *out);
void ap_<feature>_save(const ap_<feature>_settings *in);
```

Implement load/save against `ap_settings_get` / `ap_settings_set` (the app-wide settings table, see `src/library/library.c` for the implementation). Use a stable, namespaced key prefix (e.g. `"quick_export.format"`, `"quick_export.quality"`).

Defaults: fill any missing field with a sensible default rather than failing on absence. The first-run user has no settings rows; the app should not crash.

### 2. Cache on `ap_app`

In `src/app/app.h`:

```c
ap_<feature>_settings *ap_app_<feature>_settings(ap_app *app);
```

In `src/app/app.c`, add the field to `struct ap_app` (already declared in the .c file) and call `ap_<feature>_load(&app->cached)` once during `ap_app_init` (or whichever init function exists; grep for the existing `ap_app_quick_export_load` call site as a reference).

The accessor returns a pointer to the cached struct. The modal mutates it directly; readers (tooltips, action handlers) read from it directly.

### 3. Wire the modal section

Edit `src/app/preferences_modal.c`. Add a section using the existing conventions:

```c
ap_<feature>_settings *s = ap_app_<feature>_settings(app);

igSeparatorText("<Section title>");

igText("Field label:");
igSameLine(0.0f, -1.0f);
igSetNextItemWidth(<sensible width>);
/* widget bound to &s-><field>, with stable ##id */
```

Save / Cancel buttons are shared at the bottom of the modal — Save calls `ap_<feature>_save(s)` for each section, Cancel calls `ap_<feature>_load(s)` to discard unsaved edits. Add your `_save` and `_load` calls to those existing button bodies; don't grow new buttons.

The modal uses the standard keyboard nav helpers (`ap_modal_enter_pressed`, `ap_modal_esc_pressed`) — your section gets them for free via the shared buttons.

### 4. Use the cached settings in your feature's action paths

Anywhere your feature reads a setting at runtime:

```c
const ap_<feature>_settings *s = ap_app_<feature>_settings(app);
/* read s-><field> */
```

Do NOT call `ap_<feature>_load` from a hot path — that re-hits sqlite. The cache is the source of truth at runtime; sqlite is only touched at startup and on Save.

If your feature has a tooltip showing current values (common pattern — see Quick Export's menu tooltip), it reads from the cache too.

### 5. Tooltip and discoverability

Where the feature is invoked (menu item, button, hotkey), surface the current configured value in a tooltip so the user knows what's about to happen without opening Preferences. Quick Export's `quick_export_tooltip` is the reference implementation.

## Defaults strategy

Defaults serve two purposes: first-run UX, and a "Use default" shortcut in the modal. For path-like settings (e.g. an export destination), the convention is:

- Stored value is empty (`""`) when the user hasn't explicitly set one.
- The UI shows a placeholder (via `igInputTextWithHint`) describing what empty resolves to.
- The action path resolves empty -> default at use time, not at save time, so the default tracks state changes (e.g. switching libraries changes the resolved default).

See `quick_export_tooltip` + `ap_quick_export_resolve_destination` for the pattern.

## What NOT to do

- **No per-frame sqlite reads.** Cache once, mutate cache, write on Save.
- **No new top-level modal.** New settings go in the existing Preferences modal as a new `igSeparatorText` section. The user shouldn't need to learn N different config surfaces.
- **No "advanced" tab.** Discoverability beats clutter; if a setting matters enough to expose, it goes in the main flow. If it doesn't, don't expose it.
- **No naming-template-like complexity** for path/format fields unless the feature actually warrants it. Quick Export deliberately punts naming templates to the full Export modal.
- **No global cache that becomes inconsistent with the modal state.** The Save / Cancel buttons mutate the same cached struct the rest of the app reads from. No parallel "pending" copy.

## Testing checklist before opening the PR

- First-run (delete `~/.local/share/aperture/library.db` or whatever the per-library settings table is in your test library) — defaults appear correctly in the modal and the action path uses the defaults.
- Change a value in the modal, click Save, restart the app — value persists.
- Change a value, click Cancel — change is discarded.
- Action path that reads the setting reflects the saved value immediately (no restart needed).
- Tooltip surfaces the current value correctly.
