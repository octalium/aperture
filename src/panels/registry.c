#include "panels.h"

#include <stddef.h>

extern const ap_panel panel_libraries;
extern const ap_panel panel_photo_info;
extern const ap_panel panel_photo_edit;

const ap_panel *const ap_panel_registry[] = {
    &panel_libraries,
    &panel_photo_info,
    &panel_photo_edit,
    NULL,
};
