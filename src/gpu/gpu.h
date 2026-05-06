#ifndef APERTURE_GPU_H
#define APERTURE_GPU_H

#include <stdbool.h>

typedef struct ap_gpu ap_gpu;

ap_gpu *ap_gpu_create(int width, int height, const char *title);
void ap_gpu_destroy(ap_gpu *g);

bool ap_gpu_should_run(ap_gpu *g);
int ap_gpu_render_frame(ap_gpu *g);
void ap_gpu_wait_idle(ap_gpu *g);

#endif
