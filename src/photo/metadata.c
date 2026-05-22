#include "metadata.h"

#include "core/log.h"

#include <stdio.h>
#include <string.h>

// Two parallel tables keyed by the ap_meta_field enum value. Keep the
// order in sync with ap_meta_field. The label is what the panel
// shows; the key is what the sidecar serializes ("snake_case", stable
// across UI rewordings).
typedef struct {
    ap_meta_field f;
    const char   *label;
    const char   *key;
} ap_meta_descriptor;

static const ap_meta_descriptor FIELDS[] = {
    { AP_META_DATETIME,     "Date / Time",   "datetime"     },
    { AP_META_CAMERA_MAKE,  "Camera Make",   "camera_make"  },
    { AP_META_CAMERA_MODEL, "Camera Model",  "camera_model" },
    { AP_META_LENS_MAKE,    "Lens Make",     "lens_make"    },
    { AP_META_LENS_MODEL,   "Lens Model",    "lens_model"   },
    { AP_META_FOCAL_LEN,    "Focal Length",  "focal_length" },
    { AP_META_APERTURE,     "Aperture",      "aperture"     },
    { AP_META_SHUTTER,      "Shutter",       "shutter"      },
    { AP_META_ISO,          "ISO",           "iso"          },
    { AP_META_GPS_LAT,      "GPS Latitude",  "gps_latitude" },
    { AP_META_GPS_LON,      "GPS Longitude", "gps_longitude"},
    { AP_META_GPS_ALT,      "GPS Altitude",  "gps_altitude" },
    { AP_META_ARTIST,       "Artist",        "artist"       },
    { AP_META_DESCRIPTION,  "Description",   "description"  },
};

_Static_assert(sizeof(FIELDS) / sizeof(FIELDS[0]) == AP_META_FIELD_COUNT,
               "FIELDS must enumerate every ap_meta_field");

void ap_photo_metadata_clear(ap_photo_metadata *m)
{
    if (!m) return;
    for (int i = 0; i < AP_META_FIELD_COUNT; i++) m->value[i][0] = '\0';
}

void ap_photo_metadata_set(ap_photo_metadata *m, ap_meta_field f,
                           const char *value)
{
    if (!m) { AP_FATAL("ap_photo_metadata_set: NULL metadata struct"); return; }
    if (f < 0 || f >= AP_META_FIELD_COUNT) {
        AP_FATAL("ap_photo_metadata_set: field %d out of range", (int)f);
        return;
    }
    snprintf(m->value[f], AP_META_VALUE_LEN, "%s", value ? value : "");
}

const char *ap_photo_metadata_get(const ap_photo_metadata *m, ap_meta_field f)
{
    if (!m) { AP_FATAL("ap_photo_metadata_get: NULL metadata struct"); return ""; }
    if (f < 0 || f >= AP_META_FIELD_COUNT) {
        AP_FATAL("ap_photo_metadata_get: field %d out of range", (int)f);
        return "";
    }
    return m->value[f];
}

const char *ap_meta_field_label(ap_meta_field f)
{
    if (f < 0 || f >= AP_META_FIELD_COUNT) {
        AP_FATAL("ap_meta_field_label: field %d out of range", (int)f);
        return "";
    }
    return FIELDS[f].label;
}

const char *ap_meta_field_key(ap_meta_field f)
{
    if (f < 0 || f >= AP_META_FIELD_COUNT) {
        AP_FATAL("ap_meta_field_key: field %d out of range", (int)f);
        return "";
    }
    return FIELDS[f].key;
}

ap_meta_field ap_meta_field_from_key(const char *key)
{
    if (!key) return AP_META_FIELD_COUNT;
    for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
        if (strcmp(FIELDS[i].key, key) == 0) return FIELDS[i].f;
    }
    return AP_META_FIELD_COUNT;
}
