#ifndef APERTURE_UPDATE_UPDATER_H
#define APERTURE_UPDATE_UPDATER_H

#include "update/manifest.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Platform-abstraction interface for the install / restart half of
// the update flow. Each platform's implementation lives in its own
// TU under src/update/ and is selected at build time. The active
// updater is reached through ap_updater_get(); the per-platform
// real implementations land in subsequent PRs (#418 AppImage, #419
// macOS Sparkle, #420 Windows WinSparkle, #421 Flatpak detect).
//
// The default implementation in updater.c is a no-op placeholder
// that records the latest manifest and falls back to opening the
// GitHub Releases page via xdg-open / open / start when the user
// hits "Update now".
typedef struct {
    // Trigger a platform-specific manifest poll. The default
    // implementation submits an async update-check job through the
    // worker pool; platform implementations may layer their own
    // signature / channel logic on top.
    void (*check)(void);

    // Apply the pending update and restart, when the platform
    // supports in-place install. The default implementation opens
    // the project's Releases page in the user's browser so the user
    // can grab the artifact manually.
    void (*apply)(void);

    // True when a newer version is known to be available. The
    // default implementation returns whatever ap_updater_set_pending
    // last recorded.
    bool (*available)(void);
} ap_updater;

// Return the active platform updater. Never NULL.
const ap_updater *ap_updater_get(void);

// Record the manifest the most recent check produced. `newer`
// indicates whether `manifest->latest` is strictly newer than the
// running build. Passing newer=false clears the "available" flag.
// The default updater consults this state from `available`.
void ap_updater_set_pending(const ap_manifest *manifest, bool newer);

// Latest manifest recorded by ap_updater_set_pending, or NULL when
// no check has succeeded yet. The buffer is internal — read it as
// const and copy out anything that must outlive the next call.
const ap_manifest *ap_updater_pending(void);

#ifdef __cplusplus
}
#endif

#endif
