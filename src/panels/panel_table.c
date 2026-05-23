#include "panels.h"

#include <stddef.h>

// Centralised visibility state for all optional panels (those that
// participate in the Edit menu show/hide pattern from panels.h).
// Each bool is named after its panel so grep finds it from either
// direction; the panel's .visible pointer is initialised to the
// corresponding address below.
bool ap_panel_visible_library_info      = false;
bool ap_panel_visible_library_metadata  = false;
bool ap_panel_visible_library_pipelines = false;
bool ap_panel_visible_library_groups    = false;

extern const ap_panel panel_photo_edit;
extern const ap_panel panel_photo_keywords;
extern const ap_panel panel_photo_metadata;
extern const ap_panel panel_library_empty_state;
extern const ap_panel panel_library_filter;
extern const ap_panel panel_library_info;
extern const ap_panel panel_library_metadata;
extern const ap_panel panel_library_pipelines;
extern const ap_panel panel_library_groups;
extern const ap_panel panel_export_format;
extern const ap_panel panel_export_quality;
extern const ap_panel panel_export_naming;
extern const ap_panel panel_export_destination;
extern const ap_panel panel_export_presets;
extern const ap_panel panel_export_batch;

const ap_panel *const ap_panel_registry[] = {
    &panel_photo_edit,
    &panel_photo_keywords,
    &panel_photo_metadata,
    &panel_library_empty_state,
    &panel_library_filter,
    &panel_library_info,
    &panel_library_metadata,
    &panel_library_pipelines,
    &panel_library_groups,
    &panel_export_format,
    &panel_export_quality,
    &panel_export_naming,
    &panel_export_destination,
    &panel_export_presets,
    &panel_export_batch,
    NULL,
};
