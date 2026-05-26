// macOS ap_updater backend backed by Sparkle.
//
// Sparkle owns the entire update UX once we hand off: appcast fetch,
// EdDSA signature verification, download progress UI, atomic swap, and
// the restart prompt. This TU is a narrow Objective-C bridge that
// exposes ap_updater_macos_get() to the routing chain in updater.c,
// inheriting check + available from ap_updater_default() and overriding
// only apply(). check inherits the default no-op because Sparkle polls
// independently per SUEnableAutomaticChecks; available reflects the
// shared ap_updater_set_pending state populated by the manifest-fetch
// worker that runs on every platform.
//
// Built only when ap_dist == 'macos' (see meson.options). Routed in
// via ap_updater_get() in updater.c.

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

#include "update/updater.h"

#include <stdbool.h>

// Lazily-constructed SPUStandardUpdaterController. Sparkle's standard
// controller wires up the default user driver (system dialogs) and
// schedules background checks per SUEnableAutomaticChecks. One instance
// per process — we never tear it down because Sparkle keeps internal
// state (last-check time, deferred-version skips) tied to its lifetime.
static SPUStandardUpdaterController *g_updater_controller = nil;

static SPUStandardUpdaterController *macos_controller(void)
{
    if (g_updater_controller) return g_updater_controller;
    @autoreleasepool {
        g_updater_controller =
            [[SPUStandardUpdaterController alloc]
                initWithStartingUpdater:YES
                        updaterDelegate:nil
                     userDriverDelegate:nil];
    }
    return g_updater_controller;
}

static int macos_apply(const char **err_msg)
{
    @autoreleasepool {
        SPUStandardUpdaterController *ctl = macos_controller();
        if (!ctl) {
            if (err_msg) *err_msg = "Sparkle controller failed to initialise.";
            return -1;
        }
        SPUUpdater *updater = ctl.updater;
        if (!updater) {
            if (err_msg) *err_msg = "Sparkle updater unavailable.";
            return -1;
        }
        [ctl checkForUpdates:nil];
    }
    return 0;
}

const ap_updater *ap_updater_macos_get(void)
{
    static ap_updater self;
    static bool       init = false;
    if (!init) {
        self       = *ap_updater_default();
        self.apply = macos_apply;
        init       = true;
    }
    return &self;
}
