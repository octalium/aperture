#ifndef APERTURE_SIDECAR_H
#define APERTURE_SIDECAR_H

#include "gpu/gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

// Try to load the edit state from `<source_path>.aperture`. Fills
// `*edit` from the sidecar's [edit] table. Returns 0 on success.
// Returns nonzero on missing file, parse error, or unsupported
// schema version - caller should leave `*edit` at its defaults.
int ap_sidecar_load_edit(const char *source_path, ap_edit_state *edit);

// Atomically write the edit state to `<source_path>.aperture`.
// Returns 0 on success.
int ap_sidecar_save_edit(const char *source_path, const ap_edit_state *edit);

#ifdef __cplusplus
}
#endif

#endif
