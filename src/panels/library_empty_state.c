#include "panels.h"

#include "app/app.h"
#include "library/library.h"

#include "cimgui.h"

// Renders an "Import photos to get started" call-to-action as a
// centred, always-on-top ImGui window when the current library has
// zero visible photos. Hidden automatically as soon as a photo
// appears (grid_map_count > 0) so it never overlaps the grid.

static void library_empty_state_draw(ap_app *app)
{
    if (!app) return;
    ap_library *lib = ap_app_library(app);
    if (!lib) return;
    if (ap_library_photo_count(lib) > 0) return;

    ImGuiIO *io = igGetIO_Nil();
    ImVec2_c center = { io->DisplaySize.x * 0.5f,
                        io->DisplaySize.y * 0.5f };
    igSetNextWindowPos(center, ImGuiCond_Always,
                       (ImVec2_c){ 0.5f, 0.5f });
    igSetNextWindowSize((ImVec2_c){ 320.0f, 0.0f }, ImGuiCond_Always);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration      |
        ImGuiWindowFlags_NoMove            |
        ImGuiWindowFlags_NoSavedSettings   |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (!igBegin("##empty_library", NULL, flags)) {
        igEnd();
        return;
    }

    igTextWrapped("This library has no photos yet.");
    igSpacing();
    if (igButton("Import photos...",
                 (ImVec2_c){ -1.0f, 0.0f })) {
        ap_app_open_import_modal(app);
    }

    igEnd();
}

const ap_panel panel_library_empty_state = {
    .name       = "library_empty_state",
    .mode       = AP_MODE_LIBRARY,
    .draw       = library_empty_state_draw,
    .visible    = NULL,
    .menu_label = NULL,
};
