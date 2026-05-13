#ifndef APERTURE_GPU_H
#define APERTURE_GPU_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_gpu             ap_gpu;
typedef struct ap_pipeline_graph  ap_pipeline_graph;
typedef struct ap_canvas          ap_canvas;
typedef struct ap_grid            ap_grid;

typedef struct {
    float exposure_ev;
    float tone_contrast;
    float tone_pivot;
    int   respect_orientation;  // bool - apply EXIF flip in demosaic
} ap_edit_state;

ap_gpu *ap_gpu_create(int width, int height, const char *title);
void    ap_gpu_destroy(ap_gpu *g);

bool ap_gpu_should_run(ap_gpu *g);
int  ap_gpu_render_frame(ap_gpu *g, const ap_edit_state *edit);
void ap_gpu_wait_idle(ap_gpu *g);

void ap_gpu_set_graph(ap_gpu *g, ap_pipeline_graph *graph);
void ap_gpu_set_canvas(ap_gpu *g, ap_canvas *canvas);
void ap_gpu_set_grid(ap_gpu *g, ap_grid *grid);

void ap_gpu_set_window_title(ap_gpu *g, const char *title);

// Toggle fullscreen on the configured target monitor (see
// ap_gpu_set_fullscreen_monitor). Windowed geometry is remembered
// across the toggle.
void ap_gpu_toggle_fullscreen(ap_gpu *g);

// Available monitors (re-queried per call — GLFW handles hotplug).
int          ap_gpu_monitor_count(ap_gpu *g);
const char  *ap_gpu_monitor_name(ap_gpu *g, int idx);

// Target monitor for the next fullscreen entry. Index is into the
// list returned by ap_gpu_monitor_name. Defaults to 0 (primary).
int  ap_gpu_fullscreen_monitor(const ap_gpu *g);
void ap_gpu_set_fullscreen_monitor(ap_gpu *g, int idx);

#ifdef __cplusplus
}
#endif

#endif
