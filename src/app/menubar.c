#define _GNU_SOURCE

#include "menubar.h"

#include "ui/file_dialog.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

static const char *library_display_label(ap_library *lib)
{
    if (!lib) return "(no library)";
    const char *name = ap_library_name(lib);
    if (name && *name) return name;
    return ap_library_root(lib);
}

void refresh_window_title(ap_app *app)
{
    if (!app || !app->gpu) return;
    if (!app->library) {
        ap_gpu_set_window_title(app->gpu, "Aperture");
        return;
    }
    char title[5120];
    snprintf(title, sizeof(title), "Aperture: %s",
             library_display_label(app->library));
    ap_gpu_set_window_title(app->gpu, title);
}

void toggle_and_persist_fullscreen(ap_app *app)
{
    ap_gpu_toggle_fullscreen(app->gpu);
    ap_settings_set("fullscreen", ap_gpu_is_fullscreen(app->gpu) ? "1" : "0");
}

static void trigger_quick_export(ap_app *app)
{
    if (!app->photo) return;
    const char *src = ap_photo_path(app->photo);

    const char *slash = strrchr(src, '/');
    const char *base  = slash ? slash + 1 : src;
    char stem[1024];
    snprintf(stem, sizeof(stem), "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';

    char out[4096];
    if (app->library) {
        char dir[4096];
        int dn = snprintf(dir, sizeof(dir), "%s/export",
                          ap_library_root(app->library));
        if (dn <= 0 || (size_t)dn >= sizeof(dir)) {
            AP_ERROR("export: directory path too long");
            return;
        }
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            AP_ERROR("export: mkdir(%s): %s", dir, strerror(errno));
            return;
        }
        int n = snprintf(out, sizeof(out), "%s/%s.jpg", dir, stem);
        if (n <= 0 || (size_t)n >= sizeof(out)) {
            AP_ERROR("export: path too long for %s", stem);
            return;
        }
    } else {
        int n = snprintf(out, sizeof(out), "%s.jpg", src);
        if (n <= 0 || (size_t)n >= sizeof(out)) {
            AP_ERROR("export: path too long for %s", src);
            return;
        }
    }

    ap_app_request_jpeg_export(app, app->photo, out, 90);
}

void draw_menubar(ap_app *app)
{
    if (!igBeginMainMenuBar()) return;

    if (igBeginMenu("File", true)) {
        if (igMenuItem_Bool("Open Library", NULL, false, true)) {
            const char *root = app->library ? ap_library_root(app->library)
                                            : NULL;
            char picked[4096];
            if (ap_file_dialog_pick_folder(picked, sizeof(picked), root)) {
                ap_app_open_library(app, picked);
            }
        }

        if (igMenuItem_Bool("Import...", "Ctrl+I", false,
                            app->library != NULL)) {
            ap_app_open_import_modal(app);
        }

        {
            bool can_export = app->photo != NULL ||
                (app->library && app->grid &&
                 ap_app_grid_selection_count(app) > 0);
            if (igMenuItem_Bool("Export...", "Ctrl+E", false, can_export)) {
                ap_app_open_export_modal(app);
            }
        }

        igSeparator();

        if (igMenuItem_Bool("Close Library", NULL,
                            false, app->library != NULL)) {
            ap_app_close_library(app);
        }

        igSeparator();

        if (igMenuItem_Bool("Quit", "Ctrl+Q", false, true)) {
            app->quit_requested = true;
        }
        igEndMenu();
    }

    if (igBeginMenu("Photo", app->photo != NULL)) {
        if (igMenuItem_Bool("Close", "Esc", false, app->photo != NULL)) {
            ap_app_close_photo(app);
        }
        if (igMenuItem_Bool("Delete", "Del", false,
                            app->photo != NULL &&
                            app->photo_library_idx >= 0)) {
            app->delete_edit_modal = true;
        }

        igSeparator();

        if (igMenuItem_Bool("Export...", "Ctrl+E",
                            false, app->photo != NULL)) {
            ap_app_open_export_modal(app);
        }
        if (igMenuItem_Bool("Quick Export", "Ctrl+Shift+E",
                            false, app->photo != NULL)) {
            trigger_quick_export(app);
        }
        igEndMenu();
    }

    if (igBeginMenu("View", true)) {
        bool show = app->show_panels;
        const char *panels_sc = (app->mode == AP_MODE_PHOTO) ? "Space" : NULL;
        if (igMenuItem_BoolPtr("Show Panels", panels_sc, &show, true)) {
            app->show_panels = show;
        }
        if (igMenuItem_Bool("Fullscreen", "F11",
                            ap_gpu_is_fullscreen(app->gpu), true)) {
            toggle_and_persist_fullscreen(app);
        }
        igSeparator();
        if (app->mode == AP_MODE_PHOTO) {
            if (igMenuItem_Bool("Reset View", "Ctrl+0", false, true)) {
                ap_canvas_reset_view(app->canvas);
            }
        } else {
            if (igMenuItem_Bool("Reset Cell Zoom", "Ctrl+0", false, true)) {
                ap_grid_reset_cell_size(app->grid);
            }
        }
        if (app->photo) {
            bool view_raw = ap_photo_view_raw(app->photo);
            if (igMenuItem_Bool("View Raw", NULL, view_raw, true)) {
                ap_photo_set_view_raw(app->photo, !view_raw);
                ap_app_rebuild_photo_graph(app);
            }
        }
        if (app->mode == AP_MODE_LIBRARY && app->library) {
            bool rendered = app->show_rendered_thumbnails;
            if (igMenuItem_BoolPtr("Show Rendered Thumbnails", "`",
                                   &rendered, true)) {
                toggle_rendered_thumbnails(app);
            }
        }
        igSeparator();

        if (igBeginMenu("Layout", true)) {
            const char *active = ap_layout_active_name();
            char names[AP_LAYOUT_MAX_ENUM][AP_LAYOUT_NAME_LEN];
            int n = ap_layout_list(names, AP_LAYOUT_MAX_ENUM);
            if (n <= 0) {
                igMenuItem_Bool("(no saved layouts)", NULL, false, false);
            } else {
                for (int i = 0; i < n; i++) {
                    bool checked = (strcmp(active, names[i]) == 0);
                    if (igMenuItem_Bool(names[i], NULL, checked, true)
                        && !checked) {
                        ap_layout_set_active(names[i]);
                    }
                }
            }
            igSeparator();
            if (igMenuItem_Bool("Save Current As...", NULL, false, true)) {
                app->save_layout_modal = true;
                app->save_layout_input[0] = '\0';
            }
            if (igMenuItem_Bool("Reset to Active Profile", NULL,
                                false, active && active[0])) {
                ap_layout_reload_active();
            }
            if (igMenuItem_Bool("Reset to Defaults", NULL, false, true)) {
                ap_layout_reset_to_default();
            }
            igEndMenu();
        }

        igEndMenu();
    }

    bool any_optional = false;
    for (const ap_panel *const *p = ap_panel_registry; *p; p++) {
        const ap_panel *panel = *p;
        if (!panel->menu_label || !panel->visible) continue;
        if (panel->mode != AP_MODE_ANY && panel->mode != app->mode) continue;
        any_optional = true;
        break;
    }
    if (any_optional && igBeginMenu("Panels", true)) {
        for (const ap_panel *const *p = ap_panel_registry; *p; p++) {
            const ap_panel *panel = *p;
            if (!panel->menu_label || !panel->visible) continue;
            if (panel->mode != AP_MODE_ANY && panel->mode != app->mode) continue;
            igMenuItem_BoolPtr(panel->menu_label, NULL, panel->visible, true);
        }
        igEndMenu();
    }

    const char *lib_label = library_display_label(app->library);
    {
        ImVec2_c label_size = igCalcTextSize(lib_label, NULL, false, -1.0f);
        ImGuiStyle *style = igGetStyle();
        float item_w   = label_size.x + style->FramePadding.x * 2.0f;
        float center_x = (igGetWindowWidth() - item_w) * 0.5f;
        igSetCursorPosX(center_x);
    }
    if (igBeginMenu(lib_label, true)) {
        if (app->library) {
            igText("%s", ap_library_root(app->library));
            igText("%d photos", ap_library_photo_count(app->library));
            if (igMenuItem_Bool("Rename", NULL, false, true)) {
                snprintf(app->rename_library_input,
                         sizeof(app->rename_library_input), "%s",
                         ap_library_name(app->library));
                app->rename_library_modal = true;
            }
            igSeparator();
        }
        ap_registry_entry rows[16];
        int n = ap_registry_list(rows, 16);
        if (n <= 0) {
            igMenuItem_Bool("(no recent libraries)", NULL, false, false);
        } else {
            for (int i = 0; i < n; i++) {
                bool current = app->library
                    && strcmp(rows[i].path, ap_library_root(app->library)) == 0;
                const char *label_name = rows[i].name[0] ? rows[i].name
                                                         : rows[i].path;
                if (igMenuItem_Bool(label_name, NULL, current, true) && !current) {
                    ap_app_open_library(app, rows[i].path);
                }
                if (igIsItemHovered(0)) {
                    igSetTooltip("%s", rows[i].path);
                }
            }
        }
        igEndMenu();
    }

    igEndMainMenuBar();
}

void drive_global_hotkeys(ap_app *app)
{
    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    if (app->mode == AP_MODE_PHOTO && !io->WantTextInput
        && igIsKeyPressed_Bool(ImGuiKey_Space, false)) {
        app->show_panels = !app->show_panels;
    }
    if (igIsKeyPressed_Bool(ImGuiKey_F11, false)) {
        toggle_and_persist_fullscreen(app);
    }
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_Q, false)) {
        app->quit_requested = true;
    }
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_E, false)
        && !io->KeyShift) {
        ap_app_open_export_modal(app);
    }
    if (io->KeyCtrl && !io->WantTextInput
        && igIsKeyPressed_Bool(ImGuiKey_I, false) && app->library) {
        ap_app_open_import_modal(app);
    }
    if (io->KeyCtrl && io->KeyShift
        && igIsKeyPressed_Bool(ImGuiKey_E, false) && app->photo) {
        trigger_quick_export(app);
    }
    if (io->KeyCtrl && !io->WantTextInput
        && igIsKeyPressed_Bool(ImGuiKey_C, false) && app->photo) {
        ap_app_copy_edits(app);
    }
    if (io->KeyCtrl && !io->WantTextInput
        && igIsKeyPressed_Bool(ImGuiKey_V, false) && app->photo
        && app->edit_clipboard_valid) {
        ap_app_paste_edits(app);
    }
    if (io->KeyCtrl && !io->WantTextInput
        && igIsKeyPressed_Bool(ImGuiKey_Z, false) && app->photo) {
        if (io->KeyShift) {
            ap_app_redo(app);
        } else {
            ap_app_undo(app);
        }
    }
    if (io->KeyCtrl && !io->WantTextInput
        && igIsKeyPressed_Bool(ImGuiKey_Y, false) && app->photo) {
        ap_app_redo(app);
    }
    if (!io->WantTextInput) {
        if (app->photo) {
            bool held = igIsKeyDown_Nil(ImGuiKey_GraveAccent);
            if (held != app->compare_original) {
                ap_app_set_compare_original(app, held);
            }
        } else if (app->mode == AP_MODE_LIBRARY && app->library) {
            if (igIsKeyPressed_Bool(ImGuiKey_GraveAccent, false)) {
                toggle_rendered_thumbnails(app);
            }
        }
    }
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_0, false)) {
        if (app->mode == AP_MODE_PHOTO) {
            ap_canvas_reset_view(app->canvas);
        } else {
            ap_grid_reset_cell_size(app->grid);
        }
    }

    if (io->KeyCtrl && (igIsKeyPressed_Bool(ImGuiKey_Equal, true) ||
                        igIsKeyPressed_Bool(ImGuiKey_KeypadAdd, true))) {
        if (app->mode == AP_MODE_PHOTO) {
            int win_w = (int)io->DisplaySize.x;
            int win_h = (int)io->DisplaySize.y;
            ap_canvas_zoom_at(app->canvas, 1.15f,
                              win_w * 0.5f, win_h * 0.5f, win_w, win_h);
        } else {
            int win_w = (int)io->DisplaySize.x;
            int win_h = (int)io->DisplaySize.y;
            ap_grid_zoom_at(app->grid,
                            ap_grid_cell_size(app->grid) + 16,
                            win_w * 0.5f, win_h * 0.5f, win_w, win_h);
        }
    }
    if (io->KeyCtrl && (igIsKeyPressed_Bool(ImGuiKey_Minus, true) ||
                        igIsKeyPressed_Bool(ImGuiKey_KeypadSubtract, true))) {
        if (app->mode == AP_MODE_PHOTO) {
            int win_w = (int)io->DisplaySize.x;
            int win_h = (int)io->DisplaySize.y;
            ap_canvas_zoom_at(app->canvas, 1.0f / 1.15f,
                              win_w * 0.5f, win_h * 0.5f, win_w, win_h);
        } else {
            int win_w = (int)io->DisplaySize.x;
            int win_h = (int)io->DisplaySize.y;
            ap_grid_zoom_at(app->grid,
                            ap_grid_cell_size(app->grid) - 16,
                            win_w * 0.5f, win_h * 0.5f, win_w, win_h);
        }
    }
}
