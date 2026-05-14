#include "module.h"

#include <string.h>

extern const ap_module module_raw_passthrough;
extern const ap_module module_demosaic;
extern const ap_module module_wb;
extern const ap_module module_profile;
extern const ap_module module_exposure;
extern const ap_module module_tone;
extern const ap_module module_output_transfer;

const ap_module *const ap_module_registry[] = {
    &module_raw_passthrough,
    &module_demosaic,
    &module_wb,
    &module_profile,
    &module_exposure,
    &module_tone,
    &module_output_transfer,
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
