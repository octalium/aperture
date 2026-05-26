#include "core/version.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>

// Parse the next dotted component out of *p, advancing past it and
// the trailing separator (a single '.', if present). Stops on any
// non-digit other than '.': "1.2-rc1" yields 1, then 2, then 0.
static int parse_component(const char **p)
{
    int v = 0;
    const char *s = *p;
    while (*s && isdigit((unsigned char)*s)) {
        v = v * 10 + (*s - '0');
        s++;
    }
    if (*s == '.') s++;
    else if (*s) s = "";
    *p = s;
    return v;
}

int ap_version_compare(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    for (;;) {
        bool ae = (*a == '\0');
        bool be = (*b == '\0');
        if (ae && be) return 0;
        int av = ae ? 0 : parse_component(&a);
        int bv = be ? 0 : parse_component(&b);
        if (av != bv) return av < bv ? -1 : 1;
        if (*a == '\0' && *b == '\0') return 0;
    }
}
