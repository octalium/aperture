#include "module.h"

#include <string.h>

extern const ap_module module_demosaic;
extern const ap_module module_exposure;
extern const ap_module module_tone;
extern const ap_module module_encode;

const ap_module *const ap_module_registry[] = {
    &module_demosaic,
    &module_exposure,
    &module_tone,
    &module_encode,
    NULL,
};

const ap_module *ap_module_find(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (const ap_module *const *p = ap_module_registry; *p; p++) {
        if (strcmp((*p)->name, name) == 0) {
            return *p;
        }
    }
    return NULL;
}
