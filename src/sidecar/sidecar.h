#ifndef APERTURE_SIDECAR_H
#define APERTURE_SIDECAR_H

#include "edit/stack.h"
#include "photo/culling.h"
#include "photo/groups.h"
#include "photo/metadata.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Load the photo's persisted state from `<source_path>.aperture`.
// `*stack` is reset and then populated from the file's [[edit]]
// entries; `*respect_orientation` is filled from the top-level
// [aperture] table (defaults to true when missing); the [metadata]
// table populates `*user_meta` / `user_set` (slots not present in
// the table are left empty / false) and `*culling` (rating / flag /
// color, cleared to their defaults when absent); `*groups` is filled
// from the [aperture] table's `groups` array (empty when absent).
//
// Returns 0 on success, nonzero on missing / unparseable file -
// callers leave the out-params at their caller-seeded defaults in
// that case.
int ap_sidecar_load(const char *source_path,
                    ap_edit_stack *stack,
                    bool *respect_orientation,
                    ap_photo_metadata *user_meta,
                    bool user_set[AP_META_FIELD_COUNT],
                    ap_photo_culling *culling,
                    ap_photo_groups *groups);

// Atomically write the edit stack, photo flags, metadata overrides,
// culling state, and group membership to `<source_path>.aperture`.
// Returns 0 on success.
int ap_sidecar_save(const char *source_path,
                    const ap_edit_stack *stack,
                    bool respect_orientation,
                    const ap_photo_metadata *user_meta,
                    const bool user_set[AP_META_FIELD_COUNT],
                    const ap_photo_culling *culling,
                    const ap_photo_groups *groups);

// Read only the culling fields (rating / flag / color) from a sidecar
// — a lightweight parse that skips the edit stack and string
// metadata. Used to build the library db's cached culling columns.
// `*out` is cleared first. Returns 0 on success, nonzero when the
// sidecar is missing or unparseable (`*out` left at its defaults).
int ap_sidecar_load_culling(const char *source_path, ap_photo_culling *out);

// Read only the group membership from a sidecar — a lightweight parse
// that skips the edit stack and metadata. Used to build the library's
// group index. `*out` is cleared first. Returns 0 on success, nonzero
// when the sidecar is missing or unparseable (`*out` left empty).
int ap_sidecar_load_groups(const char *source_path, ap_photo_groups *out);

// Delete the photo's sidecar file (`<source_path>.aperture`) from
// disk. Returns 0 on success or when the sidecar is already absent,
// nonzero on a real removal error.
int ap_sidecar_remove(const char *source_path);

#ifdef __cplusplus
}
#endif

#endif
