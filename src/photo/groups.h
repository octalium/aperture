#ifndef APERTURE_PHOTO_GROUPS_H
#define APERTURE_PHOTO_GROUPS_H

#ifdef __cplusplus
extern "C" {
#endif

// Group membership for a single photo. Groups are plain names; a photo
// may belong to several at once. This is the in-memory form of the
// sidecar's `groups` array — the sidecar is the source of truth, so
// membership travels with the photo.
#define AP_GROUPS_MAX     16
#define AP_GROUP_NAME_LEN 64

typedef struct {
    char names[AP_GROUPS_MAX][AP_GROUP_NAME_LEN];
    int  count;
} ap_photo_groups;

#ifdef __cplusplus
}
#endif

#endif
