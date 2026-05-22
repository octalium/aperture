#include "keywords.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// Strip leading and trailing ASCII whitespace from `src`, write the
// result into `out` (up to `out_len - 1` bytes), and NUL-terminate.
static void trim(const char *src, char *out, size_t out_len)
{
    if (!src || out_len == 0) {
        if (out && out_len > 0) out[0] = '\0';
        return;
    }
    while (*src && isspace((unsigned char)*src)) src++;
    size_t n = strlen(src);
    while (n > 0 && isspace((unsigned char)src[n - 1])) n--;
    if (n >= out_len) n = out_len - 1;
    memcpy(out, src, n);
    out[n] = '\0';
}

// Normalise hierarchy separator: replace every `/` with
// AP_KW_SEPARATOR so both authoring conventions are equivalent.
static void normalise_sep(char *kw)
{
    for (char *p = kw; *p; p++) {
        if (*p == '/') *p = AP_KW_SEPARATOR;
    }
}

void ap_photo_keywords_clear(ap_photo_keywords *k)
{
    if (!k) return;
    k->count = 0;
}

bool ap_photo_keywords_is_empty(const ap_photo_keywords *k)
{
    return !k || k->count == 0;
}

bool ap_photo_keywords_add(ap_photo_keywords *k, const char *kw)
{
    if (!k || !kw) return false;
    if (k->count >= AP_KEYWORDS_MAX) return false;

    char buf[AP_KEYWORD_LEN];
    trim(kw, buf, sizeof(buf));
    normalise_sep(buf);

    if (buf[0] == '\0') return false;
    for (int i = 0; i < k->count; i++) {
        if (strcmp(k->kw[i], buf) == 0) return false;
    }
    snprintf(k->kw[k->count], AP_KEYWORD_LEN, "%s", buf);
    k->count++;
    return true;
}

bool ap_photo_keywords_remove(ap_photo_keywords *k, const char *kw)
{
    if (!k || !kw) return false;
    for (int i = 0; i < k->count; i++) {
        if (strcmp(k->kw[i], kw) == 0) {
            // Shift remaining entries left.
            for (int j = i + 1; j < k->count; j++) {
                memcpy(k->kw[j - 1], k->kw[j], AP_KEYWORD_LEN);
            }
            k->count--;
            return true;
        }
    }
    return false;
}

bool ap_photo_keywords_contains(const ap_photo_keywords *k, const char *kw)
{
    if (!k || !kw) return false;
    for (int i = 0; i < k->count; i++) {
        if (strcmp(k->kw[i], kw) == 0) return true;
    }
    return false;
}
