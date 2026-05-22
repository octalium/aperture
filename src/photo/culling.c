#include "culling.h"

#include <string.h>

void ap_photo_culling_clear(ap_photo_culling *c)
{
    if (!c) return;
    c->rating = 0;
    c->flag   = AP_FLAG_NONE;
    c->color  = AP_COLOR_NONE;
}

bool ap_photo_culling_is_empty(const ap_photo_culling *c)
{
    if (!c) return true;
    return c->rating == 0 &&
           c->flag   == AP_FLAG_NONE &&
           c->color  == AP_COLOR_NONE;
}

int ap_rating_clamp(int rating)
{
    if (rating < AP_RATING_MIN) return AP_RATING_MIN;
    if (rating > AP_RATING_MAX) return AP_RATING_MAX;
    return rating;
}

// Flag <-> key. AP_FLAG_NONE has no key — the sidecar omits the row
// entirely rather than writing a sentinel string.
const char *ap_flag_key(ap_flag f)
{
    switch (f) {
    case AP_FLAG_PICK:   return "pick";
    case AP_FLAG_REJECT: return "reject";
    case AP_FLAG_NONE:   break;
    }
    return "";
}

ap_flag ap_flag_from_key(const char *key)
{
    if (!key) return AP_FLAG_NONE;
    if (strcmp(key, "pick")   == 0) return AP_FLAG_PICK;
    if (strcmp(key, "reject") == 0) return AP_FLAG_REJECT;
    return AP_FLAG_NONE;
}

// Colour label <-> key, indexed by the ap_color_label enum value.
static const char *COLOR_KEYS[AP_COLOR_LABEL_COUNT] = {
    "",        // AP_COLOR_NONE
    "red",     // AP_COLOR_RED
    "yellow",  // AP_COLOR_YELLOW
    "green",   // AP_COLOR_GREEN
    "blue",    // AP_COLOR_BLUE
    "purple",  // AP_COLOR_PURPLE
};

const char *ap_color_label_key(ap_color_label c)
{
    if (c < AP_COLOR_NONE || c >= AP_COLOR_LABEL_COUNT) return "";
    return COLOR_KEYS[c];
}

ap_color_label ap_color_label_from_key(const char *key)
{
    if (!key) return AP_COLOR_NONE;
    for (int i = AP_COLOR_RED; i < AP_COLOR_LABEL_COUNT; i++) {
        if (strcmp(COLOR_KEYS[i], key) == 0) return (ap_color_label)i;
    }
    return AP_COLOR_NONE;
}

// Display swatches, indexed by ap_color_label. The hues track the
// conventional cull-palette colours; alpha is full for every real
// label, zero for AP_COLOR_NONE so a caller can blend it transparently.
static const unsigned COLOR_RGBA[AP_COLOR_LABEL_COUNT] = {
    0x00000000u,  // AP_COLOR_NONE  (transparent)
    0xFF4242E5u,  // AP_COLOR_RED
    0xFF42C8E5u,  // AP_COLOR_YELLOW
    0xFF4CB752u,  // AP_COLOR_GREEN
    0xFFE5994Cu,  // AP_COLOR_BLUE
    0xFFC85CB0u,  // AP_COLOR_PURPLE
};

unsigned ap_color_label_rgba(ap_color_label c)
{
    if (c < AP_COLOR_NONE || c >= AP_COLOR_LABEL_COUNT) return 0x00000000u;
    return COLOR_RGBA[c];
}
