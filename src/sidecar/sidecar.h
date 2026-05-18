#ifndef APERTURE_SIDECAR_H
#define APERTURE_SIDECAR_H

#include "edit/stack.h"
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
// the table are left empty / false).
//
// Returns 0 on success, nonzero on missing / unparseable file -
// callers leave the stack at its caller-seeded defaults in that
// case.
int ap_sidecar_load(const char *source_path,
                    ap_edit_stack *stack,
                    bool *respect_orientation,
                    ap_photo_metadata *user_meta,
                    bool user_set[AP_META_FIELD_COUNT]);

// Atomically write the edit stack, photo flags, and metadata
// overrides to `<source_path>.aperture`. Returns 0 on success.
int ap_sidecar_save(const char *source_path,
                    const ap_edit_stack *stack,
                    bool respect_orientation,
                    const ap_photo_metadata *user_meta,
                    const bool user_set[AP_META_FIELD_COUNT]);

#ifdef __cplusplus
}
#endif

#endif
