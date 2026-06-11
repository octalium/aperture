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
typedef struct ap_edit_stack      ap_edit_stack;

ap_gpu *ap_gpu_create(int width, int height, const char *title);
void    ap_gpu_destroy(ap_gpu *g);

bool ap_gpu_should_run(ap_gpu *g);
int  ap_gpu_render_frame(ap_gpu *g, const ap_edit_stack *stack);

// Block until the device is idle, then destroy everything on the deferred
// retire list (idle satisfies every retire gate). Callers that destroy GPU
// resources inline (library close / cache swap, photo close) rely on this
// drain to make their idle assumption hold for retired resources too.
void ap_gpu_wait_idle(ap_gpu *g);

// Bind `graph` as the photo graph rendered by the pump. The device must be
// idle: the scheduler is reset, discarding any promoted slot. Use
// ap_gpu_swap_graph for a live structural replacement.
void ap_gpu_set_graph(ap_gpu *g, ap_pipeline_graph *graph);

// Structurally replace the bound photo graph without a device stall. The
// compositor keeps sampling the old graph until `graph`'s first render
// completes; the pump then rebinds `canvas` to the new graph and retires
// the old one once no submitted work can reference it. `old` is the graph
// the caller just unlinked (ownership transfers to the gpu); it must be the
// currently bound or previously pending graph.
void ap_gpu_swap_graph(ap_gpu *g, ap_pipeline_graph *graph,
                       ap_canvas *canvas, ap_pipeline_graph *old);
void ap_gpu_set_canvas(ap_gpu *g, ap_canvas *canvas);
void ap_gpu_set_grid(ap_gpu *g, ap_grid *grid);

void ap_gpu_set_window_title(ap_gpu *g, const char *title);

// "Fullscreen" here means decorations off + maximized. The
// compositor places the maximized surface on whichever monitor the
// window already lives on, which is the only portable way to do
// this on Wayland (where clients can't query window position).
void ap_gpu_toggle_fullscreen(ap_gpu *g);
bool ap_gpu_is_fullscreen(const ap_gpu *g);

#ifdef __cplusplus
}
#endif

#endif
