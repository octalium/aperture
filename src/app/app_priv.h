#ifndef APERTURE_APP_PRIV_H
#define APERTURE_APP_PRIV_H

/*
 * Private implementation header for src/app/ translation units.
 * Not installed; never included from outside this directory.
 *
 * Exposes the full ap_app struct definition and the forward declarations
 * of internal helpers so the sub-modules (jobs, menubar, modals, grid_view)
 * can reference both without duplicating the struct.
 */

#include "app.h"
#include "app/canvas_tool.h"
#include "app/layout_profiles.h"
#include "core/log.h"
#include "core/worker.h"
#include "edit/stack.h"
#include "edit/viewport.h"
#include "gpu/canvas.h"

// Defined in src/app/jobs.h; only its address is held by ap_app.
struct import_job;
#include "gpu/gpu.h"
#include "gpu/grid.h"
#include "gpu/pipeline_graph.h"
#include "library/import.h"
#include "library/library.h"
#include "output/export.h"
#include "panels/panels.h"
#include "photo/photo.h"
#include "photo/thumbnail.h"
#include "ui/file_dialog.h"
#include "ui/imgui.h"
#include "ui/status.h"
#include "ui/toast.h"

#include "cimgui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ap_app {
    ap_gpu          *gpu;
    ap_canvas       *canvas;
    ap_grid         *grid;
    ap_mode          mode;
    ap_photo        *photo;
    int              photo_library_idx;
    ap_library      *library;
    ap_worker_pool  *workers;
    int              thumb_inflight;
    bool             photo_loading;
    char             loading_path[4096];
    uint64_t         photo_load_gen;
    uint64_t         thumb_load_gen;
    int              export_inflight;

    bool             show_panels;
    bool             show_rendered_thumbnails;

    int              group_filter_kind;
    char             group_filter_name[AP_GROUP_NAME_LEN];
    int             *grid_map;
    int              grid_map_count;
    int              grid_map_cap;

    ap_library_sort  sort;
    char             search_buf[256];
    ap_culling_filter culling_filter;

    bool             marquee_active;
    float            marquee_x0, marquee_y0;

    bool             thumb_drag_active;

    int              deferred_select_cell;

    ap_edit_stack    edit_clipboard;
    bool             edit_clipboard_valid;

    bool             compare_original;

    bool                import_modal;
    bool                import_inflight;
    char                import_source[4096];
    ap_import_settings  import_settings;
    char                import_status[160];
    ap_import_report    import_report;
    // Borrowed pointer to the in-flight import job, valid only
    // while import_inflight is true. The job's own cancel flag is
    // an atomic so the main thread can flip it without locking.
    // Cleared in handle_import_complete / discard_completed_item.
    struct import_job  *inflight_import_job;

    ap_export_settings       export_settings;
    bool                     export_modal;
    ap_quick_export_settings quick_export_settings;
    bool                     preferences_modal;
    bool             rename_library_modal;
    char             rename_library_input[128];
    bool             save_layout_modal;
    char             save_layout_input[AP_LAYOUT_NAME_LEN];
    bool             delete_modal;
    bool             delete_edit_modal;
    bool             quit_requested;

    ap_canvas_tool   canvas_tool;
    int              canvas_tool_entry;
    int              crop_drag_handle;
    bool             crop_aspect_locked;
    float            crop_aspect_ratio;
    float            crop_drag_u0, crop_drag_v0;
};

/* crop-handle enum used by both app.c and grid_view.c via canvas_tool_entry */
enum {
    CROP_HANDLE_NONE       = -1,
    CROP_HANDLE_TL         = 0,
    CROP_HANDLE_TR         = 1,
    CROP_HANDLE_BL         = 2,
    CROP_HANDLE_BR         = 3,
    CROP_HANDLE_L          = 4,
    CROP_HANDLE_R          = 5,
    CROP_HANDLE_T          = 6,
    CROP_HANDLE_B          = 7,
    CROP_HANDLE_MOVE       = 8,
    CROP_HANDLE_STRAIGHTEN = 9,
};

#define THUMB_MAX_INFLIGHT 8
#define ZOOM_FACTOR 0.10f

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* app.c internals called from sub-modules */
void bind_mode_view(ap_app *app);
void rebuild_grid_map(ap_app *app);
int  cell_for_photo(const ap_app *app, int photo_idx);
void release_photo(ap_app *app);
void submit_thumb_refresh(ap_app *app, int idx);
void toggle_and_persist_fullscreen(ap_app *app);
void toggle_rendered_thumbnails(ap_app *app);
void refresh_window_title(ap_app *app);
void delete_grid_selection(ap_app *app);
void delete_edit_photo(ap_app *app);
void open_selected_photo(ap_app *app);
void navigate_library_relative(ap_app *app, int dir);
void drain_all_workers(ap_app *app);
ap_edit_entry *canvas_tool_entry(ap_app *app, const char *module);
void crop_handle_uv(const ap_viewport *vp, int handle, float *u, float *v);
void crop_rect_sanitize(ap_viewport *vp);

/* jobs.c run-fn pointers — needed by discard_completed_item */
void thumb_job_run(ap_work_item *self);
void thumb_encode_job_run(ap_work_item *self);
void photo_open_job_run(ap_work_item *self);
void export_job_run(ap_work_item *self);
void import_job_run(ap_work_item *self);

/* jobs.c public entry points */
void submit_import_job(ap_app *app, const char *lib_root, const char *src_dir,
                       const ap_import_settings *settings);

/* jobs.c helper — install a successfully decoded raw as the active photo */
struct photo_open_job;
void install_loaded_photo(ap_app *app, struct photo_open_job *j);

#endif /* APERTURE_APP_PRIV_H */
