#define _GNU_SOURCE

#include "grid_view.h"

#include <string.h>

static void drive_grid_culling_input(ap_app *app, ImGuiIO *io)
{
    if (!app->library || !app->grid) return;
    if (io->WantTextInput || io->KeyCtrl) return;
    if (ap_grid_selection_count(app->grid) <= 0) return;

    for (int d = 0; d <= 5; d++) {
        if (!igIsKeyPressed_Bool((ImGuiKey)(ImGuiKey_0 + d), false)) continue;
        if (io->KeyShift) {
            ap_app_set_selection_color(app, (ap_color_label)d);
        } else {
            ap_app_set_selection_rating(app, d);
        }
        return;
    }

    if (io->KeyShift) return;

    if (igIsKeyPressed_Bool(ImGuiKey_P, false)) {
        ap_app_set_selection_flag(app, AP_FLAG_PICK);
    } else if (igIsKeyPressed_Bool(ImGuiKey_X, false)) {
        ap_app_set_selection_flag(app, AP_FLAG_REJECT);
    } else if (igIsKeyPressed_Bool(ImGuiKey_U, false)) {
        ap_app_set_selection_flag(app, AP_FLAG_NONE);
    }
}

void draw_grid_context_menu(ap_app *app)
{
    if (!igBeginPopup("##grid_ctx", 0)) return;

    if (!app->library || !app->grid || app->grid_map_count <= 0) {
        igEndPopup();
        return;
    }

    int sel_count   = ap_grid_selection_count(app->grid);
    int focus_cell  = ap_grid_selected(app->grid);

    if (igMenuItem_Bool("Open", NULL, false,
                        focus_cell >= 0 && focus_cell < app->grid_map_count)) {
        open_selected_photo(app);
        igCloseCurrentPopup();
    }

    igSeparator();

    if (igBeginMenu("Set Rating", sel_count > 0)) {
        static const char *const rating_labels[] = {
            "0 (None)", "1 Star", "2 Stars", "3 Stars", "4 Stars", "5 Stars"
        };
        for (int r = 0; r <= 5; r++) {
            if (igMenuItem_Bool(rating_labels[r], NULL, false, true)) {
                ap_app_set_selection_rating(app, r);
            }
        }
        igEndMenu();
    }

    if (igMenuItem_Bool("Pick", "P", false, sel_count > 0)) {
        ap_app_set_selection_flag(app, AP_FLAG_PICK);
    }
    if (igMenuItem_Bool("Reject", "X", false, sel_count > 0)) {
        ap_app_set_selection_flag(app, AP_FLAG_REJECT);
    }
    if (igMenuItem_Bool("Clear Flag", "U", false, sel_count > 0)) {
        ap_app_set_selection_flag(app, AP_FLAG_NONE);
    }

    igSeparator();

    if (igBeginMenu("Set Color Label", sel_count > 0)) {
        static const char *const color_labels[] = {
            "None", "Red", "Yellow", "Green", "Blue", "Purple"
        };
        for (int c = 0; c <= 5; c++) {
            if (igMenuItem_Bool(color_labels[c], NULL, false, true)) {
                ap_app_set_selection_color(app, (ap_color_label)c);
            }
        }
        igEndMenu();
    }

    igSeparator();

    {
        char gnames[256][AP_GROUP_NAME_LEN];
        int gn = ap_library_group_list(app->library, gnames, 256);
        bool has_groups = (gn > 0);
        if (igBeginMenu("Add to Group", has_groups && sel_count > 0)) {
            for (int i = 0; i < gn; i++) {
                if (igMenuItem_Bool(gnames[i], NULL, false, true)) {
                    ap_app_assign_selection_to_group(app, gnames[i], true);
                }
            }
            igEndMenu();
        }
        if (igBeginMenu("Remove from Group", has_groups && sel_count > 0)) {
            for (int i = 0; i < gn; i++) {
                if (igMenuItem_Bool(gnames[i], NULL, false, true)) {
                    ap_app_assign_selection_to_group(app, gnames[i], false);
                }
            }
            igEndMenu();
        }
    }

    igSeparator();

    if (igMenuItem_Bool("Export...", NULL, false, sel_count > 0)) {
        ap_app_open_export_modal(app);
        igCloseCurrentPopup();
    }

    if (igMenuItem_Bool("Delete", NULL, false, sel_count > 0)) {
        app->delete_modal = true;
        igCloseCurrentPopup();
    }

    igEndPopup();
}

void drive_grid_input(ap_app *app)
{
    if (!app->library || !app->grid) return;
    int n = app->grid_map_count;
    if (n <= 0) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    if (igIsPopupOpen_Str(NULL, ImGuiPopupFlags_AnyPopup)) {
        ap_grid_set_hover(app->grid, -1);
        return;
    }

    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;

    {
        int hover = io->WantCaptureMouse ? -1
                  : ap_grid_hit_test(app->grid,
                                     io->MousePos.x, io->MousePos.y,
                                     win_w, win_h);
        ap_grid_set_hover(app->grid, hover);
        if (hover >= 0 && hover < app->grid_map_count) {
            int i = app->grid_map[hover];
            const char *rel = ap_library_photo_relative_path(app->library, i);
            if (rel) igSetTooltip("%s", rel);
            igSetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }

    if (!io->WantCaptureMouse) {
        if (igIsMouseClicked_Bool(ImGuiMouseButton_Right, false)) {
            int hit = ap_grid_hit_test(app->grid,
                                       io->MousePos.x, io->MousePos.y,
                                       win_w, win_h);
            if (hit >= 0) {
                if (!ap_grid_is_selected(app->grid, hit)) {
                    ap_grid_select_only(app->grid, hit);
                }
                igOpenPopup_Str("##grid_ctx", 0);
            }
        }
        if (io->MouseWheel != 0.0f) {
            if (io->KeyCtrl) {
                int cur = ap_grid_cell_size(app->grid);
                float factor = io->MouseWheel > 0.0f
                    ? 1.0f + ZOOM_FACTOR * io->MouseWheel
                    : 1.0f / (1.0f - ZOOM_FACTOR * io->MouseWheel);
                int next = (int)((float)cur * factor + 0.5f);
                ap_grid_zoom_at(app->grid, next,
                                io->MousePos.x, io->MousePos.y,
                                win_w, win_h);
            } else {
                const float wheel_step_px = 60.0f;
                ap_grid_scroll(app->grid, -io->MouseWheel * wheel_step_px,
                               win_w, win_h);
            }
        }
        if (igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
            app->marquee_active   = false;
            app->thumb_drag_active = false;
            int hit = ap_grid_hit_test(app->grid,
                                       io->MousePos.x, io->MousePos.y,
                                       win_w, win_h);
            if (hit >= 0) {
                ap_grid_select_only(app->grid, hit);
                open_selected_photo(app);
                return;
            }
        } else if (igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
            app->marquee_x0 = io->MousePos.x;
            app->marquee_y0 = io->MousePos.y;
            app->deferred_select_cell = -1;
            int hit = ap_grid_hit_test(app->grid,
                                       io->MousePos.x, io->MousePos.y,
                                       win_w, win_h);
            if (hit >= 0) {
                int anchor = ap_grid_selected(app->grid);
                if (io->KeyShift) {
                    ap_grid_select_range(app->grid, anchor, hit);
                } else if (io->KeyCtrl) {
                    ap_grid_select_toggle(app->grid, hit);
                } else if (ap_grid_is_selected(app->grid, hit) &&
                           ap_grid_selection_count(app->grid) > 1) {
                    app->deferred_select_cell = hit;
                } else {
                    ap_grid_select_only(app->grid, hit);
                }
            } else {
                app->marquee_active = true;
            }
        }

        if (app->deferred_select_cell >= 0 &&
            !igIsMouseDown_Nil(ImGuiMouseButton_Left) &&
            !igIsMouseDragging(ImGuiMouseButton_Left, -1.0f)) {
            ap_grid_select_only(app->grid, app->deferred_select_cell);
            app->deferred_select_cell = -1;
        }

        if (app->marquee_active) {
            if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
                app->marquee_active = false;
            } else if (igIsMouseDragging(ImGuiMouseButton_Left, -1.0f)) {
                ap_grid_select_rect(app->grid,
                                    app->marquee_x0, app->marquee_y0,
                                    io->MousePos.x,  io->MousePos.y,
                                    win_w, win_h);
            }
        }

        if (!app->marquee_active && !app->thumb_drag_active) {
            if (igIsMouseDragging(ImGuiMouseButton_Left, -1.0f)) {
                if (app->deferred_select_cell >= 0) {
                    app->deferred_select_cell = -1;
                    app->thumb_drag_active = true;
                } else {
                    int hit = ap_grid_hit_test(app->grid,
                                               app->marquee_x0, app->marquee_y0,
                                               win_w, win_h);
                    if (hit >= 0 && ap_grid_is_selected(app->grid, hit) &&
                        ap_grid_selection_count(app->grid) > 0) {
                        app->thumb_drag_active = true;
                    }
                }
            }
        }
    } else {
        app->marquee_active = false;
        app->deferred_select_cell = -1;
    }

    if (app->thumb_drag_active) {
        if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
            app->thumb_drag_active = false;
        } else if (igBeginDragDropSource(ImGuiDragDropFlags_SourceExtern)) {
            int sel = ap_grid_selection_count(app->grid);
            igSetDragDropPayload("AP_THUMB_DRAG", &sel, sizeof(int),
                                 ImGuiCond_Once);
            igText("%d photo(s)", sel);
            igEndDragDropSource();
        }
    }

    int sel = ap_grid_selected(app->grid);
    int new_sel = sel;
    int cpr = ap_grid_cells_per_row(app->grid, win_w, win_h);

    int rows_per_page = ap_grid_rows_per_page(app->grid, win_w, win_h);

    if (!io->WantTextInput) {
        if      (igIsKeyPressed_Bool(ImGuiKey_RightArrow, true)) new_sel = sel + 1;
        else if (igIsKeyPressed_Bool(ImGuiKey_LeftArrow,  true)) new_sel = sel - 1;
        else if (igIsKeyPressed_Bool(ImGuiKey_DownArrow,  true)) new_sel = sel + cpr;
        else if (igIsKeyPressed_Bool(ImGuiKey_UpArrow,    true)) new_sel = sel - cpr;
        else if (igIsKeyPressed_Bool(ImGuiKey_Home,  false))     new_sel = 0;
        else if (igIsKeyPressed_Bool(ImGuiKey_End,   false))     new_sel = n - 1;
        else if (igIsKeyPressed_Bool(ImGuiKey_PageDown, true))   new_sel = sel + rows_per_page * cpr;
        else if (igIsKeyPressed_Bool(ImGuiKey_PageUp,   true))   new_sel = sel - rows_per_page * cpr;
    }
    if (new_sel != sel) {
        if (io->KeyShift) {
            ap_grid_select_range(app->grid, sel, new_sel);
        } else {
            ap_grid_select_only(app->grid, new_sel);
        }
        ap_grid_ensure_visible(app->grid, ap_grid_selected(app->grid),
                               win_w, win_h);
    }

    if (!io->KeyCtrl && !io->WantTextInput &&
        (igIsKeyPressed_Bool(ImGuiKey_Enter, false) ||
         igIsKeyPressed_Bool(ImGuiKey_Space, false))) {
        open_selected_photo(app);
    }

    // Ctrl+A in library mode: select every visible cell.
    if (io->KeyCtrl && !io->WantTextInput &&
        igIsKeyPressed_Bool(ImGuiKey_A, false)) {
        ap_grid_select_range(app->grid, 0, n - 1);
    }

    if (!io->WantTextInput &&
        igIsKeyPressed_Bool(ImGuiKey_Delete, false) &&
        ap_grid_selection_count(app->grid) > 0) {
        if (io->KeyShift) {
            delete_grid_selection(app);
        } else {
            app->delete_modal = true;
        }
    }

    drive_grid_culling_input(app, io);
}

void draw_selection_overlay(ap_app *app)
{
    if (!app->library || !app->grid) return;
    int n = app->grid_map_count;
    if (n <= 0) return;
    int sel_count = ap_grid_selection_count(app->grid);
    if (sel_count <= 1) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;
    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;
    ImDrawList *dl = igGetForegroundDrawList_ViewportPtr(NULL);
    if (!dl) return;

    int focus = ap_grid_selected(app->grid);
    for (int i = 0; i < n; i++) {
        if (i == focus) continue;
        if (!ap_grid_is_selected(app->grid, i)) continue;
        float cx, cy, cw, ch;
        if (ap_grid_cell_rect(app->grid, i, win_w, win_h,
                              &cx, &cy, &cw, &ch) != 0) continue;
        ImVec2_c tl = { cx,      cy      };
        ImVec2_c br = { cx + cw, cy + ch };
        ImDrawList_AddRect(dl, tl, br, 0xFFB8C4D9, 0.0f, 2.0f, 0);
    }
}

void draw_marquee_overlay(ap_app *app)
{
    if (!app->marquee_active) return;
    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;
    if (!igIsMouseDragging(ImGuiMouseButton_Left, -1.0f)) return;

    ImDrawList *dl = igGetForegroundDrawList_ViewportPtr(NULL);
    if (!dl) return;

    ImVec2_c tl = { app->marquee_x0 < io->MousePos.x
                        ? app->marquee_x0 : io->MousePos.x,
                    app->marquee_y0 < io->MousePos.y
                        ? app->marquee_y0 : io->MousePos.y };
    ImVec2_c br = { app->marquee_x0 < io->MousePos.x
                        ? io->MousePos.x : app->marquee_x0,
                    app->marquee_y0 < io->MousePos.y
                        ? io->MousePos.y : app->marquee_y0 };

    ImDrawList_AddRectFilled(dl, tl, br, 0x26B8C4D9, 0.0f, 0);
    ImDrawList_AddRect(dl, tl, br, 0xCCB8C4D9, 0.0f, 1.0f, 0);
}

static void draw_cell_culling(ImDrawList *dl, const ap_photo_culling *cull,
                              float fit_x, float fit_y, float fit_w,
                              float band_top, float band_h)
{
    if (ap_photo_culling_is_empty(cull)) return;

    if (cull->color != AP_COLOR_NONE) {
        const float strip_h = 4.0f;
        ImVec2_c s_tl = { fit_x,          fit_y           };
        ImVec2_c s_br = { fit_x + fit_w,  fit_y + strip_h };
        ImDrawList_AddRectFilled(dl, s_tl, s_br,
                                 ap_color_label_rgba(cull->color), 0.0f, 0);
    }

    if (cull->flag != AP_FLAG_NONE) {
        const float r = 5.0f;
        ImVec2_c centre = { fit_x + r + 3.0f, fit_y + r + 3.0f };
        unsigned fill = (cull->flag == AP_FLAG_PICK)
                            ? 0xFF4CB752u : 0xFF4242E5u;
        ImDrawList_AddCircleFilled(dl, centre, r, fill, 0);
        ImDrawList_AddCircle(dl, centre, r, 0xFF101010u, 0, 1.5f);
    }

    if (cull->rating > 0) {
        const float dot_r  = 2.5f;
        const float dot_gap = 3.0f;
        const float pitch  = dot_r * 2.0f + dot_gap;
        float total = pitch * (float)cull->rating - dot_gap;
        float cy_dot = band_top + band_h * 0.5f;
        float x = fit_x + fit_w - 4.0f - total + dot_r;
        for (int s = 0; s < cull->rating; s++) {
            ImVec2_c centre = { x, cy_dot };
            ImDrawList_AddCircleFilled(dl, centre, dot_r, 0xFF55D6F2u, 0);
            x += pitch;
        }
    }
}

void draw_grid_labels(ap_app *app)
{
    if (!app->library || !app->grid) return;
    int n = app->grid_map_count;
    if (n <= 0) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;
    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;

    ImDrawList *dl = igGetBackgroundDrawList(NULL);
    if (!dl) return;

    const float band_h = 18.0f;
    for (int c = 0; c < n; c++) {
        int i = app->grid_map[c];
        const char *rel = ap_library_photo_relative_path(app->library, i);
        if (!rel) continue;
        const char *slash = strrchr(rel, '/');
        const char *label = slash ? slash + 1 : rel;
        float cx, cy, cw, ch;
        if (ap_grid_cell_rect(app->grid, c, win_w, win_h, &cx, &cy, &cw, &ch) != 0) {
            continue;
        }

        float fit_x = cx, fit_y = cy, fit_w = cw, fit_h = ch;
        ap_thumbnail *t = ap_library_thumbnail(app->library, i);
        if (t) {
            int tw = ap_thumbnail_width(t);
            int th = ap_thumbnail_height(t);
            if (tw > 0 && th > 0) {
                float s = cw / (float)tw;
                float sy = ch / (float)th;
                if (sy < s) s = sy;
                fit_w = (float)tw * s;
                fit_h = (float)th * s;
                fit_x = cx + (cw - fit_w) * 0.5f;
                fit_y = cy + (ch - fit_h) * 0.5f;
            }
        }

        if (ch < band_h) continue;

        float band_top = fit_y + fit_h - band_h;
        if (band_top < cy)                  band_top = cy;
        if (band_top + band_h > cy + ch)    band_top = cy + ch - band_h;

        ImVec2_c band_tl = { fit_x,         band_top          };
        ImVec2_c band_br = { fit_x + fit_w, band_top + band_h };
        ImDrawList_AddRectFilled(dl, band_tl, band_br, 0xB8000000, 0.0f, 0);

        ap_photo_culling cull = ap_library_photo_culling(app->library, i);

        float text_right = fit_x + fit_w - 4.0f;
        if (cull.rating > 0) {
            const float pitch = 2.5f * 2.0f + 3.0f;
            text_right -= pitch * (float)cull.rating - 3.0f + 4.0f;
        }
        ImVec2_c text_pos = { fit_x + 4.0f, band_top + 2.0f };
        ImVec2_c clip_tl  = { fit_x,      band_top          };
        ImVec2_c clip_br  = { text_right, band_top + band_h };
        ImDrawList_PushClipRect(dl, clip_tl, clip_br, true);
        ImDrawList_AddText_Vec2(dl, text_pos, 0xFFEEEEEE, label, NULL);
        ImDrawList_PopClipRect(dl);

        draw_cell_culling(dl, &cull, fit_x, fit_y, fit_w,
                          band_top, band_h);
    }
}
