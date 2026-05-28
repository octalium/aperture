#define _GNU_SOURCE

#include "modals.h"

#include "core/version.h"
#include "ui/modal_kbd.h"

#include <stdio.h>

#define AP_PROJECT_URL  "https://github.com/octalium/aperture"
#define AP_LICENSE_URL  "https://github.com/octalium/aperture/blob/main/LICENSE"

void draw_about_modal(ap_app *app)
{
    if (app->about_modal) {
        igOpenPopup_Str("About aperture", 0);
        app->about_modal = false;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize
                           | ImGuiWindowFlags_NoCollapse;
    if (!igBeginPopupModal("About aperture", NULL, flags)) return;

    igText("aperture %s", AP_VERSION_STRING);
    igTextDisabled("An opinionated raw photo processor.");
    igTextDisabled("C + Vulkan + Dear ImGui.");

    igSeparator();

    igText("License:");
    igSameLine(0.0f, -1.0f);
    igTextDisabled("MIT  (%s)", AP_LICENSE_URL);

    igText("Project:");
    igSameLine(0.0f, -1.0f);
    igTextDisabled("%s", AP_PROJECT_URL);

    igSeparator();

    igSeparatorText("Updates");

    if (ap_app_update_check_inflight(app)) {
        igTextDisabled("Checking for updates...");
    } else {
        if (igButton("Check for updates", (ImVec2_c){ 160.0f, 0.0f })) {
            ap_app_check_for_updates(app);
        }
    }

    igSeparatorText("Vendored dependencies");
    // manually kept in sync with dep/*.wrap (vendored) and the
    // dependency() calls in meson.build (system). update both lists
    // together whenever a dep is added, removed, or moved.
    igTextDisabled("cimgui, lcms2, libpng, libtiff, tomlc99, blake3, cJSON,");
    igTextDisabled("nativefiledialog-extended, mbedtls (linux only).");
    igTextDisabled("System: vulkan, glfw3, libraw, lensfun, sqlite3,");
    igTextDisabled("libjpeg.");

    igSeparator();

    if (igButton("Close", (ImVec2_c){ 120.0f, 0.0f })
        || ap_modal_enter_pressed()
        || ap_modal_esc_pressed()) {
        igCloseCurrentPopup();
    }

    igEndPopup();
}

void draw_update_modal(ap_app *app)
{
    if (app->update.modal) {
        igOpenPopup_Str("Update available", 0);
        app->update.modal = false;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize
                           | ImGuiWindowFlags_NoCollapse;
    if (!igBeginPopupModal("Update available", NULL, flags)) return;

    const ap_manifest *m = &app->update.manifest;
    igText("aperture %s is available.", m->latest);
    igTextDisabled("You're on %s.", AP_VERSION_STRING);

    if (m->notes[0]) {
        igSeparator();
        igTextWrapped("%s", m->notes);
    }

    igSeparator();

    if (igButton("Update now", (ImVec2_c){ 120.0f, 0.0f })
        || ap_modal_enter_pressed()) {
        ap_app_apply_update(app);
        app->update.modal_dismissed = true;
        igCloseCurrentPopup();
    }
    igSameLine(0.0f, -1.0f);
    if (igButton("Remind later", (ImVec2_c){ 120.0f, 0.0f })) {
        // Per-session suppression: don't auto-reopen this session.
        app->update.modal_dismissed = true;
        igCloseCurrentPopup();
    }
    igSameLine(0.0f, -1.0f);
    if (igButton("Skip", (ImVec2_c){ 80.0f, 0.0f })
        || ap_modal_esc_pressed()) {
        app->update.modal_dismissed = true;
        igCloseCurrentPopup();
    }

    igEndPopup();
}
