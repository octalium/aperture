#include "panels.h"

#include <stddef.h>

extern const ap_panel panel_photo_edit;
extern const ap_panel panel_photo_metadata;
extern const ap_panel panel_library_metadata;

const ap_panel *const ap_panel_registry[] = {
    &panel_photo_edit,
    &panel_photo_metadata,
    &panel_library_metadata,
    NULL,
};
