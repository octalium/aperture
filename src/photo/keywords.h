#ifndef APERTURE_PHOTO_KEYWORDS_H
#define APERTURE_PHOTO_KEYWORDS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Per-photo keyword list, persisted in the `.aperture` sidecar's
// [metadata] table as a TOML string array (`keywords`).
//
// Keywords are stored verbatim as authored; hierarchy is expressed
// using the separator character (default `|`, also accepted: `/`).
// For example `lighting|softbox` is a two-level path. The struct
// carries them as flat strings; callers that need the hierarchy
// split on `|` or `/`.
//
// The list is unsorted and duplicate-free (ap_photo_keywords_add
// is a no-op when the keyword already exists, modulo leading /
// trailing whitespace).

#define AP_KEYWORDS_MAX     256
#define AP_KEYWORD_LEN      128

// Canonical separator written to the sidecar.
#define AP_KW_SEPARATOR     '|'

typedef struct {
    char kw[AP_KEYWORDS_MAX][AP_KEYWORD_LEN];
    int  count;
} ap_photo_keywords;

// Reset to empty.
void ap_photo_keywords_clear(ap_photo_keywords *k);

// True when no keywords are present.
bool ap_photo_keywords_is_empty(const ap_photo_keywords *k);

// Add a keyword. Strips leading/trailing whitespace. Both `|` and
// `/` in `kw` are normalised to `AP_KW_SEPARATOR` so authoring
// via either convention is equivalent. A no-op if the keyword
// already exists or if the list is full. Returns true when the
// keyword was actually added.
bool ap_photo_keywords_add(ap_photo_keywords *k, const char *kw);

// Remove a keyword by exact match. Returns true when found and
// removed.
bool ap_photo_keywords_remove(ap_photo_keywords *k, const char *kw);

// True when the exact keyword string `kw` is present.
bool ap_photo_keywords_contains(const ap_photo_keywords *k,
                                const char *kw);

#ifdef __cplusplus
}
#endif

#endif
