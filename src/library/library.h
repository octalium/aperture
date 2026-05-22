#ifndef APERTURE_LIBRARY_H
#define APERTURE_LIBRARY_H

#include "edit/stack.h"
#include "photo/culling.h"
#include "photo/groups.h"
#include "output/export.h"
#include "photo/metadata.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_library ap_library;

// One row from the app-wide library registry.
typedef struct {
    char    id[37];      // RFC 4122 v4 UUID + NUL
    char    path[4096];  // absolute filesystem path
    char    name[128];   // user-set display name; empty if unset
    int64_t created_at;  // unix seconds
} ap_registry_entry;

// Read up to `max` rows from the registry, ordered by most-recent
// created_at. Returns the number written, or -1 on error. Safe to
// call before any library has been opened (creates the registry db
// + schema if needed).
int ap_registry_list(ap_registry_entry *out, int max);

// ----- pipelines (app-wide, registry db) -----

#define AP_PIPELINE_NAME_LEN 64

// A pipeline is a named, ordered list of edit-stack entries
// (module + params + enabled + display_name). The on-disk shape
// reuses the sidecar's `[[edit]]` TOML serialization — see
// edit/stack_toml.h — so the same stack round-trips between sidecar
// and pipeline row.
typedef struct {
    int64_t       id;
    char          name[AP_PIPELINE_NAME_LEN];
    ap_edit_stack stack;
} ap_pipeline_def;

// Fetch the app-wide default pipeline. The registry is created and
// the default row seeded if either is missing. Returns 0 on success.
int ap_pipeline_get_default(ap_pipeline_def *out);

// Fetch a pipeline by id. Returns 0 on success, -1 if not found.
int ap_pipeline_get(int64_t id, ap_pipeline_def *out);

// Fetch a pipeline by name (exact match). Returns 0 on success,
// -1 if no pipeline with that name exists.
int ap_pipeline_get_by_name(const char *name, ap_pipeline_def *out);

// List up to `max` pipelines, ordered by name. Returns the number
// written, or -1 on error.
int ap_pipeline_list(ap_pipeline_def *out, int max);

// Create a new pipeline with the given name and stack contents.
// On success writes the new row's id to `*out_id` and returns 0.
// Returns -1 on failure (name collision, db error, ...).
int ap_pipeline_create(const char *name, const ap_edit_stack *stack,
                       int64_t *out_id);

// Update an existing pipeline's name and/or stack. Pass NULL for
// either to leave it untouched. Returns 0 on success.
int ap_pipeline_update(int64_t id, const char *name,
                       const ap_edit_stack *stack);

// Delete a pipeline by id. Returns 0 on success, -1 on failure. The
// default pipeline is protected — delete on its id is a no-op.
int ap_pipeline_delete(int64_t id);

// Replace `out` with a fresh stack assembled from the pipeline's
// entries (user-visible modules only; transport modules like
// demosaic are managed by the graph). Preserves the pipeline's
// params, display_name, and enabled flags. Returns 0 on success.
int ap_pipeline_apply_to_stack(int64_t pipeline_id, ap_edit_stack *out);

// Convenience wrapper around ap_pipeline_apply_to_stack for the
// app-wide default. Used by the photo-open + sidecar fallback paths.
int ap_pipeline_apply_default_to_stack(ap_edit_stack *out);

// ----- app-wide key/value settings (registry db) -----

// Look up a setting by key. On success writes a NUL-terminated string
// into `out` and returns 0. Returns -1 when missing or on error.
int ap_settings_get(const char *key, char *out, size_t out_len);

// Upsert a setting. Pass NULL or empty to remove. Returns 0 on success.
int ap_settings_set(const char *key, const char *value);

// Open a directory as a library:
//   - Resolves `path` to an absolute root.
//   - Opens or creates `<root>/library.aperture-db` (SQLite).
//   - Runs schema-create-if-needed for the v1 tables.
//   - Recursively scans the tree for raw files; inserts new ones
//     into the photos table.
//   - Caches the photo list in memory for browsing.
//
// Returns NULL on failure (path not a directory, db open failure,
// out of memory, etc.). On success the caller owns the returned
// pointer until ap_library_close.
ap_library *ap_library_open(const char *path);
void        ap_library_close(ap_library *lib);

const char *ap_library_root(const ap_library *lib);
int         ap_library_photo_count(const ap_library *lib);

// User-set display name. Empty string when unset (callers fall back
// to the basename of the path).
const char *ap_library_name(const ap_library *lib);

// Persist a new display name on the library's registry row. Pass an
// empty string (or NULL) to clear. Returns 0 on success.
int         ap_library_set_name(ap_library *lib, const char *name);

// Per-library default pipeline. The pointer is stored in the
// per-library db's settings table; the registry default is the
// fallback when the per-library pointer is unset or the referenced
// pipeline no longer exists. Returns the pipeline id (>0) on
// success, or 0 if no pipeline is found (caller falls back).
int64_t     ap_library_default_pipeline_id(const ap_library *lib);

// Persist the per-library default pipeline id. Pass 0 to clear so
// the library reverts to the registry default. Returns 0 on success.
int         ap_library_set_default_pipeline_id(ap_library *lib, int64_t id);

// Generic per-library key/value settings (per-library db). _get writes
// a NUL-terminated string into `out`; returns 0 on success, -1 when the
// key is missing or on error. _set upserts; pass NULL or empty `value`
// to remove the key. Returns 0 on success.
int         ap_library_setting_get(const ap_library *lib, const char *key,
                                   char *out, size_t out_len);
int         ap_library_setting_set(ap_library *lib, const char *key,
                                   const char *value);

// ----- photo groups -----
//
// Group membership lives in each photo's sidecar (the source of
// truth). The library builds an in-memory index from the sidecars
// when it is opened; the calls below read and mutate that index and
// keep the sidecars in sync.

// The n-th photo's group membership, or NULL when the index is out of
// range. Owned by the library; valid until close or the next mutation.
const ap_photo_groups *ap_library_photo_groups(const ap_library *lib,
                                               int index);

// Collect the registered group names into `names`. Returns the count
// written (<= max).
int ap_library_group_list(const ap_library *lib,
                          char names[][AP_GROUP_NAME_LEN], int max);

// Register a new (possibly empty) group. Idempotent — a no-op when the
// group already exists. Returns 0 on success.
int ap_library_group_create(ap_library *lib, const char *name);

// Add (member=true) or remove (member=false) the n-th photo's
// membership in `group`. Updates the index and rewrites the photo's
// sidecar. A no-op (returns 0) when already in the requested state.
int ap_library_set_photo_group(ap_library *lib, int index,
                               const char *group, bool member);

// Rename `old_name` to `new_name` across every photo carrying it.
int ap_library_rename_group(ap_library *lib, const char *old_name,
                            const char *new_name);

// Remove `group` from every photo carrying it.
int ap_library_delete_group(ap_library *lib, const char *group);

// ----- culling state (rating / pick-reject flag / colour label) -----
//
// Each photo's culling state lives in its sidecar (the source of
// truth) and is cached in the photos table for fast filtering. The
// library builds an in-memory cache on open; the calls below read and
// mutate that cache and keep the sidecar + db column in sync.

// The n-th photo's cached culling state. Returns a cleared (untouched)
// struct when the index is out of range.
ap_photo_culling ap_library_photo_culling(const ap_library *lib, int index);

// Overwrite the n-th photo's culling state: updates the in-memory
// cache, the cached db columns, and the photo's `.aperture` sidecar
// (seeding the default edit pipeline when the photo has no sidecar
// yet, so the write doesn't strip its edits). Returns 0 on success.
int ap_library_set_photo_culling(ap_library *lib, int index,
                                 ap_photo_culling culling);

// Relative path of the n-th photo (n in [0, count)). Owned by the
// library; valid until close.
const char *ap_library_photo_relative_path(const ap_library *lib, int index);

// Build the absolute path for the n-th photo into `buf`. Returns 0
// on success, nonzero if the buffer is too small or the index is out
// of range.
int ap_library_photo_absolute_path(const ap_library *lib, int index,
                                   char *buf, size_t buflen);

// Delete the n-th photo: remove its raw file and `.aperture` sidecar
// from disk, delete its photos-table row and cached edit-render
// thumbnail, and drop it from the library's in-memory caches. The
// library holds an imported copy of each photo; the originals live
// outside it. Photos after `index` shift down by one. Returns 0 on
// success, -1 on a bad index.
int ap_library_photo_remove(ap_library *lib, int index);

// Sort keys for the photo list order. The value is the SQLite ORDER BY
// clause term; passing an invalid enum falls back to AP_SORT_PATH.
typedef enum {
    AP_SORT_PATH         = 0,  // relative filename path (default)
    AP_SORT_CAPTURE_TIME = 1,  // EXIF capture_time (oldest first)
    AP_SORT_ADDED_AT     = 2,  // library import time (oldest first)
} ap_library_sort;

// Re-order the in-memory photo list by re-reading the photos table
// with the given sort key.  Drops all cached thumbnails, rebuilds
// the group index, and resets the thumbnail decode cursor.  Callers
// must follow this with ap_app_rebuild_grid_map (or equivalent) so
// the grid reflects the new order.  Returns 0 on success.
int ap_library_reload_sorted(ap_library *lib, ap_library_sort sort);

// ----- thumbnail cache (lifetime-bound to the library) -----

typedef struct ap_thumbnail ap_thumbnail;

// Returns the cached thumbnail for the n-th photo, or NULL if not
// yet decoded.
ap_thumbnail *ap_library_thumbnail(const ap_library *lib, int index);

// Hand a freshly-decoded thumbnail to the library. The library takes
// ownership and will destroy it on close. No-op if `index` is out
// of range.
void ap_library_set_thumbnail(ap_library *lib, int index, ap_thumbnail *t);

// Index of the next photo whose thumbnail hasn't been decoded yet,
// or -1 if every slot is filled. The library does not decide when
// or how decoding happens - callers drive a per-frame pump.
int  ap_library_pending_thumbnail_idx(const ap_library *lib);

// Drop the cached thumbnail for the n-th photo and rewind the
// decode cursor so the pump re-decodes it. Used after a photo is
// edited + closed so the grid picks up its freshly-rendered
// edit-cache thumbnail.
void ap_library_invalidate_thumbnail(ap_library *lib, int index);

// Record that decoding the n-th photo's thumbnail failed. The slot
// stays NULL but is excluded from future pending_thumbnail_idx results
// so the pump does not spin trying to re-submit it. A subsequent
// ap_library_invalidate_thumbnail call clears the flag and allows a
// fresh attempt.
void ap_library_mark_thumbnail_failed(ap_library *lib, int index);

// ----- edit-render thumbnail blobs (persisted in the library db) -----
//
// The `thumbnails` table stores a small JPEG of each photo rendered
// through its edit stack. These survive across sessions and are the
// preferred source for grid thumbnails; the camera's embedded
// preview is the fallback when no fresh blob exists.

// Fetch the n-th photo's edit-render JPEG if it's fresh — i.e. the
// stored render is at least as new as the photo's `.aperture`
// sidecar. On success allocates `*out_jpeg` (caller frees) and
// returns 0. Returns -1 when there's no row, the render is stale,
// or the sidecar is gone.
int  ap_library_thumbnail_blob(const ap_library *lib, int index,
                               unsigned char **out_jpeg, size_t *out_size);

// Upsert the n-th photo's edit-render JPEG, stamping updated_at to
// now. Call this *after* the photo's sidecar has been written so
// the freshness comparison holds. Returns 0 on success.
int  ap_library_store_thumbnail(ap_library *lib, int index,
                                const unsigned char *jpeg, size_t size);

// Replace the n-th photo's edit stack with the contents of the
// pipeline. Loads the existing sidecar so the photo's orientation
// toggle + per-field metadata overrides are preserved; writes the
// sidecar atomically. Returns 0 on success.
int  ap_library_apply_pipeline_to_photo(ap_library *lib, int index,
                                        int64_t pipeline_id);

// Replace the n-th photo's edit stack with a caller-supplied stack.
// Loads the existing sidecar to preserve orientation + metadata;
// writes the sidecar atomically. Returns 0 on success.
int  ap_library_apply_stack_to_photo(ap_library *lib, int index,
                                     const ap_edit_stack *stack);

// Apply a metadata-override patch to the n-th photo's sidecar.
// Loads the existing sidecar (seeding the default edit pipeline if
// none, so this write doesn't strip the photo's edits the next time
// it's opened), merges every field where `patch_set[f]` is true
// (replacing the previous override at that field), and saves the
// sidecar atomically. The photo's pixels and library db blob are
// untouched - metadata is per-photo string state independent of the
// rendered output. Returns 0 on success.
int  ap_library_apply_metadata_patch(ap_library *lib, int index,
                                     const ap_photo_metadata *patch,
                                     const bool patch_set[AP_META_FIELD_COUNT]);

// ----- export presets (per-library db) -----
//
// A preset is a named bundle of ap_export_settings. The library db
// stores them in the `export_presets` table so they survive across
// sessions. The settings are serialised as a flat key=value blob
// (not exposed outside library.c).

#define AP_EXPORT_PRESET_NAME_LEN 128
#define AP_EXPORT_PRESETS_MAX     64

typedef struct {
    int64_t            id;
    char               name[AP_EXPORT_PRESET_NAME_LEN];
    ap_export_settings settings;
} ap_export_preset;

// Save the current settings as a named preset. On a name collision the
// existing row is replaced. Returns 0 on success, -1 on error.
int ap_export_preset_save(ap_library *lib, const char *name,
                          const ap_export_settings *s);

// List up to `max` presets, ordered by name. Returns the count written,
// or -1 on error.
int ap_export_preset_list(const ap_library *lib,
                          ap_export_preset *out, int max);

// Load a preset by id into `out`. Returns 0 on success, -1 when not found.
int ap_export_preset_load(const ap_library *lib, int64_t id,
                          ap_export_preset *out);

// Delete a preset by id. Returns 0 on success, -1 on error.
int ap_export_preset_delete(ap_library *lib, int64_t id);

#ifdef __cplusplus
}
#endif

#endif
