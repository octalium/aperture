#ifndef APERTURE_PHOTO_CULLING_H
#define APERTURE_PHOTO_CULLING_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Per-photo culling state — the editorial layer the photographer sets
// while triaging a shoot. Unlike ap_photo_metadata (EXIF-style facts
// stored as strings), these are typed values: a star rating, a
// pick/reject flag, and a colour label. They serialize to the
// `.aperture` sidecar's [metadata] table (rating / flag / color) and
// are cached in the library db's photos table so grid filtering stays
// a single query.

// Star rating, 0 (unrated) through 5.
#define AP_RATING_MIN 0
#define AP_RATING_MAX 5

// Pick / reject flag.
typedef enum {
    AP_FLAG_NONE   = 0,
    AP_FLAG_PICK   = 1,
    AP_FLAG_REJECT = 2,
} ap_flag;

// Colour label. The order matches the conventional 1..5 palette so a
// digit shortcut maps straight onto the enum value.
typedef enum {
    AP_COLOR_NONE   = 0,
    AP_COLOR_RED    = 1,
    AP_COLOR_YELLOW = 2,
    AP_COLOR_GREEN  = 3,
    AP_COLOR_BLUE   = 4,
    AP_COLOR_PURPLE = 5,
} ap_color_label;

#define AP_COLOR_LABEL_COUNT 6

// The three culling fields side-by-side. A freshly cleared struct is
// "untouched": rating 0, flag none, colour none.
typedef struct {
    int            rating;  // clamped to [AP_RATING_MIN, AP_RATING_MAX]
    ap_flag        flag;
    ap_color_label color;
} ap_photo_culling;

// Reset to the untouched state (rating 0, no flag, no colour).
void ap_photo_culling_clear(ap_photo_culling *c);

// True when every field is at its untouched default — used to decide
// whether the sidecar needs a [metadata] culling write at all.
bool ap_photo_culling_is_empty(const ap_photo_culling *c);

// Clamp an arbitrary integer into the valid rating range.
int  ap_rating_clamp(int rating);

// Sidecar key <-> enum conversion. The key strings are the stable
// snake_case tokens the [metadata] table serializes ("pick", "red",
// ...). _from_key returns the default (AP_FLAG_NONE / AP_COLOR_NONE)
// for NULL or unknown input.
const char    *ap_flag_key(ap_flag f);
ap_flag        ap_flag_from_key(const char *key);
const char    *ap_color_label_key(ap_color_label c);
ap_color_label ap_color_label_from_key(const char *key);

// Packed 0xAARRGGBB swatch for a colour label, for grid / panel
// rendering. AP_COLOR_NONE returns a fully transparent value.
unsigned ap_color_label_rgba(ap_color_label c);

#ifdef __cplusplus
}
#endif

#endif
