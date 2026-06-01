#ifndef APERTURE_APP_MODALS_H
#define APERTURE_APP_MODALS_H

#include "app_priv.h"

void draw_import_modal(ap_app *app);
void draw_export_modal(ap_app *app);
void draw_preferences_modal(ap_app *app);
void draw_rename_library_modal(ap_app *app);
void draw_save_layout_modal(ap_app *app);
void draw_delete_modal(ap_app *app);
void draw_delete_edit_modal(ap_app *app);
void draw_about_modal(ap_app *app);
void draw_update_modal(ap_app *app);

// Non-modal "Background Jobs" window: lists every live ap_job with a
// per-job progress bar and a Cancel button. Reads only ap_job_snapshot
// and cancels by id, so it is use-after-free safe against background
// completion. Auto-closes when no jobs are live.
void draw_jobs_panel(ap_app *app);

#endif /* APERTURE_APP_MODALS_H */
