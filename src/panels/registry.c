#include "panels.h"

#include <stddef.h>

extern const ap_panel panel_library;
extern const ap_panel panel_photo_info;
extern const ap_panel panel_photo_edit;
extern const ap_panel panel_photo_viewport;

const ap_panel *const ap_panel_registry[] = {
    &panel_photo_info,
    &panel_library,
    &panel_photo_edit,
    &panel_photo_viewport,
    NULL,
};
