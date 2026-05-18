#ifndef APERTURE_PHOTO_METADATA_H
#define APERTURE_PHOTO_METADATA_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Field IDs for the per-photo metadata pane. The order also drives
// the panel's display order. All values are stored as strings;
// numeric fields are formatted to the user-facing convention at
// extraction time ("1/125", "f/2.8", "200mm", etc.) and re-parsed
// only if a downstream consumer needs the numeric form.
typedef enum {
    AP_META_DATETIME = 0,
    AP_META_CAMERA_MAKE,
    AP_META_CAMERA_MODEL,
    AP_META_LENS_MAKE,
    AP_META_LENS_MODEL,
    AP_META_FOCAL_LEN,
    AP_META_APERTURE,
    AP_META_SHUTTER,
    AP_META_ISO,
    AP_META_GPS_LAT,
    AP_META_GPS_LON,
    AP_META_GPS_ALT,
    AP_META_ARTIST,
    AP_META_DESCRIPTION,
    AP_META_FIELD_COUNT
} ap_meta_field;

#define AP_META_VALUE_LEN 256

// All fields side-by-side. A given slot is "unset" when its string
// is empty (the extractor leaves a field blank when the source has
// nothing for it, and the user-overlay struct treats empty as "no
// override" - see is-set predicate in photo.h).
typedef struct {
    char value[AP_META_FIELD_COUNT][AP_META_VALUE_LEN];
} ap_photo_metadata;

void        ap_photo_metadata_clear(ap_photo_metadata *m);
void        ap_photo_metadata_set(ap_photo_metadata *m, ap_meta_field f,
                                  const char *value);
const char *ap_photo_metadata_get(const ap_photo_metadata *m, ap_meta_field f);

// UI label ("Camera Model"); sidecar key ("camera_model").
const char *ap_meta_field_label(ap_meta_field f);
const char *ap_meta_field_key(ap_meta_field f);

// Resolve a sidecar key string back to a field id, or
// AP_META_FIELD_COUNT when the key is unknown.
ap_meta_field ap_meta_field_from_key(const char *key);

#ifdef __cplusplus
}
#endif

#endif
