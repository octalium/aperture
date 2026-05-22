#ifndef APERTURE_APP_CANVAS_TOOL_H
#define APERTURE_APP_CANVAS_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

// Interactive canvas tools — modes the photo canvas enters so a click
// or drag on the image drives an edit module instead of panning the
// view. The app owns the active tool; a module's config window asks
// for one via ap_module_render_ctx::request_canvas_tool, and the app's
// canvas-input handler implements it.
//
// AP_CANVAS_TOOL_NONE is the default pan / zoom behaviour.
typedef enum {
    AP_CANVAS_TOOL_NONE          = 0,
    AP_CANVAS_TOOL_WB_EYEDROPPER = 1,  // click a pixel -> solve White Balance
    AP_CANVAS_TOOL_CROP          = 2,  // drag handles -> drive Transform crop
} ap_canvas_tool;

#ifdef __cplusplus
}
#endif

#endif
