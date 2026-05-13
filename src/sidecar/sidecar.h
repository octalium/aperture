#ifndef APERTURE_SIDECAR_H
#define APERTURE_SIDECAR_H

#include "edit/stack.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Load the photo's persisted state from `<source_path>.aperture`.
// `*stack` is reset and then populated from the file's [[edit]]
// entries; `*respect_orientation` is filled from the top-level
// [aperture] table (defaults to true when missing).
//
// On schema v1 sidecars (the legacy flat exposure/contrast/pivot
// shape) the loader seeds the stack with equivalent default entries
// so old photos open with the same edits they had before.
//
// Returns 0 on success, nonzero on missing / unparseable file -
// callers leave the stack at its caller-seeded defaults in that
// case.
int ap_sidecar_load(const char *source_path,
                    ap_edit_stack *stack,
                    bool *respect_orientation);

// Atomically write the edit stack + photo flags to
// `<source_path>.aperture` in the v2 schema. Returns 0 on success.
int ap_sidecar_save(const char *source_path,
                    const ap_edit_stack *stack,
                    bool respect_orientation);

#ifdef __cplusplus
}
#endif

#endif
