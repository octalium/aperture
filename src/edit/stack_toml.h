#ifndef APERTURE_EDIT_STACK_TOML_H
#define APERTURE_EDIT_STACK_TOML_H

#include "edit/stack.h"

#include <stdio.h>

#include <toml.h>

#ifdef __cplusplus
extern "C" {
#endif

// Populate `out` from a TOML `[[edit]]` array. Caller owns the parse
// and the document — typically:
//
//   toml_table_t *root = toml_parse_file(...) | toml_parse(...);
//   toml_array_t *arr  = toml_table_in(root, "edit");
//   ap_edit_stack_read_toml_array(arr, &stack);
//
// The stack is reset before loading. Unknown modules + out-of-range
// rows are skipped (not errors); a per-row failure doesn't abort the
// rest of the load. Returns 0 on success.
int ap_edit_stack_read_toml_array(toml_array_t *arr, ap_edit_stack *out);

// Write the stack as a sequence of `[[edit]]` TOML rows. Callers
// control the surrounding document — the sidecar wraps with
// [aperture] and [metadata] tables; pipeline storage just stores the
// edit-array blob. Returns 0 on success.
int ap_edit_stack_write_toml(const ap_edit_stack *stack, FILE *f);

#ifdef __cplusplus
}
#endif

#endif
