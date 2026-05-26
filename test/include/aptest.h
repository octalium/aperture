#ifndef APERTURE_TEST_APTEST_H
#define APERTURE_TEST_APTEST_H

// Single-macro "framework" for aperture tests. A test is a plain
// `int main` that returns 0 on success. AP_TEST_ASSERT prints a
// formatted failure (file:line + caller-supplied message) to stderr
// and abort()s — meson's test() rule records the non-zero exit.

#include <stdio.h>
#include <stdlib.h>

#define AP_TEST_ASSERT(cond, ...)                                         \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: assertion failed: %s\n  ",            \
                    __FILE__, __LINE__, #cond);                           \
            fprintf(stderr, __VA_ARGS__);                                 \
            fputc('\n', stderr);                                          \
            abort();                                                      \
        }                                                                 \
    } while (0)

#endif
