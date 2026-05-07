#ifndef APERTURE_GPU_H
#define APERTURE_GPU_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_gpu     ap_gpu;
typedef struct ap_compute ap_compute;

typedef struct {
    float exposure_ev;
    float tone_contrast;
    float tone_pivot;
} ap_edit_state;

ap_gpu *ap_gpu_create(int width, int height, const char *title);
void    ap_gpu_destroy(ap_gpu *g);

bool ap_gpu_should_run(ap_gpu *g);
int  ap_gpu_render_frame(ap_gpu *g, const ap_edit_state *edit);
void ap_gpu_wait_idle(ap_gpu *g);

void ap_gpu_set_compute(ap_gpu *g, ap_compute *c);

#ifdef __cplusplus
}
#endif

#endif
