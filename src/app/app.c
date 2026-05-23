#define _GNU_SOURCE

#include "app.h"
#include "app_priv.h"
#include "grid_view.h"
#include "jobs.h"
#include "menubar.h"
#include "modals.h"

#include "modules/wb.h"
#include "output/export.h"
#include "output/jpeg.h"
#include "output/png.h"
#include "output/tiff.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// Set by SIGTERM / SIGINT; polled by ap_app_should_run so the main
// loop exits cleanly (running per-photo save-on-close, library
// teardown, etc.) instead of dying mid-frame.
static volatile sig_atomic_t g_quit_requested = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_quit_requested = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

// Optional panels remember their open / closed state across sessions
// in the app-wide settings, keyed by panel name. Window positions are
// handled separately by ImGui's own ini.
static void load_panel_visibility(void)
{
    for (const ap_panel *const *p = ap_panel_registry; *p; p++) {
        const ap_panel *panel = *p;
        if (!panel->visible || !panel->name) continue;
        char key[128];
        char val[8];
        snprintf(key, sizeof(key), "panel.%s.visible", panel->name);
        if (ap_settings_get(key, val, sizeof(val)) == 0) {
            *panel->visible = (atoi(val) != 0);
        }
    }
}

static void save_panel_visibility(void)
{
    for (const ap_panel *const *p = ap_panel_registry; *p; p++) {
        const ap_panel *panel = *p;
        if (!panel->visible || !panel->name) continue;
        char key[128];
        snprintf(key, sizeof(key), "panel.%s.visible", panel->name);
        ap_settings_set(key, *panel->visible ? "1" : "0");
    }
}

ap_app *ap_app_create(int width, int height, const char *title)
{
    ap_app *app = calloc(1, sizeof(*app));
    if (!app) {
        AP_ERROR("ap_app_create: out of memory");
        return NULL;
    }
    app->mode = AP_MODE_LIBRARY;
    app->show_panels = true;
    app->show_rendered_thumbnails = true;
    app->photo_library_idx = -1;
    app->canvas_tool = AP_CANVAS_TOOL_NONE;
    app->canvas_tool_entry = -1;
    app->crop_drag_handle = CROP_HANDLE_NONE;
    app->deferred_select_cell = -1;
    app->crop_aspect_ratio = 1.0f;

    app->gpu = ap_gpu_create(width, height, title);
    if (!app->gpu) {
        free(app);
        return NULL;
    }

    app->canvas = ap_canvas_create(app->gpu);
    if (!app->canvas) {
        ap_gpu_destroy(app->gpu);
        free(app);
        return NULL;
    }

    app->grid = ap_grid_create(app->gpu);
    if (!app->grid) {
        ap_canvas_destroy(app->canvas);
        ap_gpu_destroy(app->gpu);
        free(app);
        return NULL;
    }

    app->workers = ap_worker_pool_create(0);
    if (!app->workers) {
        ap_grid_destroy(app->grid);
        ap_canvas_destroy(app->canvas);
        ap_gpu_destroy(app->gpu);
        free(app);
        return NULL;
    }

    // Restore persisted preferences.
    {
        char buf[32];
        if (ap_settings_get("fullscreen", buf, sizeof(buf)) == 0
            && atoi(buf) != 0) {
            ap_gpu_toggle_fullscreen(app->gpu);
        }
    }

    // Restore the last-active layout profile and which optional panels
    // were open last session. ImGui auto-persists window positions
    // itself via its ini (see imgui_bridge.cpp).
    ap_layout_init();
    load_panel_visibility();

    install_signal_handlers();
    return app;
}

void ap_app_destroy(ap_app *app)
{
    if (!app) return;

    save_panel_visibility();
    drain_all_workers(app);
    ap_app_wait_idle(app);
    ap_app_close_photo(app);
    ap_app_close_library(app);
    ap_gpu_set_canvas(app->gpu, NULL);
    ap_gpu_set_grid(app->gpu, NULL);
    if (app->workers) {
        ap_worker_pool_destroy(app->workers);
        app->workers = NULL;
    }
    if (app->grid) {
        ap_grid_destroy(app->grid);
        app->grid = NULL;
    }
    free(app->grid_map);
    app->grid_map = NULL;
    if (app->canvas) {
        ap_canvas_destroy(app->canvas);
        app->canvas = NULL;
    }
    if (app->gpu) {
        ap_gpu_destroy(app->gpu);
        app->gpu = NULL;
    }
    free(app);
}

bool ap_app_should_run(ap_app *app)
{
    if (!app) return false;
    if (g_quit_requested) return false;
    if (app->quit_requested) return false;
    return ap_gpu_should_run(app->gpu);
}

void ap_app_wait_idle(ap_app *app)
{
    if (app && app->gpu) {
        ap_gpu_wait_idle(app->gpu);
    }
}

ap_mode ap_app_mode(const ap_app *app)
{
    return app ? app->mode : AP_MODE_LIBRARY;
}

void bind_mode_view(ap_app *app)
{
    if (!app || !app->gpu) return;
    if (app->mode == AP_MODE_PHOTO || app->mode == AP_MODE_EXPORT) {
        ap_gpu_set_grid(app->gpu, NULL);
        ap_gpu_set_canvas(app->gpu, app->canvas);
    } else {
        ap_gpu_set_canvas(app->gpu, NULL);
        ap_gpu_set_grid(app->gpu, app->library ? app->grid : NULL);
    }
}

void ap_app_set_mode(ap_app *app, ap_mode mode)
{
    if (!app) return;
    app->mode = mode;
    bind_mode_view(app);
}

void release_photo(ap_app *app)
{
    if (!app->photo) return;
    app->compare_original = false;
    app->canvas_tool       = AP_CANVAS_TOOL_NONE;
    app->canvas_tool_entry = -1;
    app->crop_drag_handle  = CROP_HANDLE_NONE;
    ap_app_wait_idle(app);
    ap_canvas_set_input(app->canvas, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 0);
    ap_gpu_set_graph(app->gpu, NULL);
    ap_photo_close(app->photo);
    app->photo = NULL;
}

int ap_app_open_photo(ap_app *app, const char *path)
{
    if (!app || !path) {
        return -1;
    }

    photo_open_job *j = calloc(1, sizeof(*j));
    if (!j) {
        AP_ERROR("ap_app_open_photo: oom");
        return -1;
    }
    j->base.run = photo_open_job_run;
    snprintf(j->path, sizeof(j->path), "%s", path);

    app->photo_load_gen++;
    j->gen = app->photo_load_gen;
    snprintf(app->loading_path, sizeof(app->loading_path), "%s", path);
    app->photo_loading = true;

    // Flip into photo mode synchronously so the workspace appears
    // instantly. The canvas binds with no input until the worker lands
    // and install_loaded_photo rebinds it to the freshly built pipeline.
    // draw_loading_overlay covers the gap. If we are already in photo
    // mode (prev/next nav), the previous photo stays visible until the
    // new one is installed.
    if (app->mode != AP_MODE_PHOTO) {
        app->mode = AP_MODE_PHOTO;
        bind_mode_view(app);
    }

    ap_worker_pool_submit(app->workers, &j->base);
    return 0;
}

void ap_app_close_photo(ap_app *app)
{
    if (!app) return;

    // Invalidate any in-flight async open so its result is discarded
    // when it lands.
    if (app->photo_loading) {
        app->photo_load_gen++;
        app->photo_loading = false;
        app->loading_path[0] = '\0';
    }

    int closed_idx = app->photo_library_idx;

    // Sync GPU readback of the rendered output while the graph is
    // still alive. The downsample + libjpeg encode + db store happen
    // on a worker so the return to library mode is immediate; the
    // affected grid cell refreshes when the worker completes.
    submit_thumb_refresh(app, closed_idx);

    release_photo(app);
    app->photo_library_idx = -1;
    app->mode  = AP_MODE_LIBRARY;
    bind_mode_view(app);
}

ap_photo *ap_app_photo(ap_app *app)
{
    return app ? app->photo : NULL;
}

ap_canvas *ap_app_canvas(ap_app *app)
{
    return app ? app->canvas : NULL;
}

void ap_app_rebuild_photo_graph(ap_app *app)
{
    if (!app || !app->photo) return;

    // Idle the device first - the old graph's images may still be in
    // flight. ap_photo_rebuild_graph then destroys the old graph and
    // builds a new one from the current edit stack.
    ap_app_wait_idle(app);
    if (ap_photo_rebuild_graph(app->photo) != 0) {
        AP_ERROR("app: photo graph rebuild failed");
        return;
    }

    // Both the GPU's current-graph pointer and the canvas binding
    // referenced the *old* graph that rebuild just freed. Re-point
    // both at the new one - missing either is a use-after-free the
    // next time a frame is recorded.
    ap_pipeline_graph *graph = ap_photo_graph(app->photo);
    ap_gpu_set_graph(app->gpu, graph);
    ap_canvas_set_input(app->canvas,
                        ap_pipeline_graph_output_view(graph),
                        ap_pipeline_graph_output_sampler(graph),
                        ap_pipeline_graph_output_width(graph),
                        ap_pipeline_graph_output_height(graph));

    // If the before/after compare was active when the graph was rebuilt,
    // re-apply the bypass to the new graph. The rebuild produced fresh
    // stages with skip flags matching the entries' enabled state; we
    // need to override them again.
    if (app->compare_original) {
        app->compare_original = false;   // clear so set_ sees a change
        ap_app_set_compare_original(app, true);
    }
}

bool ap_app_photo_loading(const ap_app *app)
{
    return app ? app->photo_loading : false;
}

bool ap_app_compare_original(const ap_app *app)
{
    return app ? app->compare_original : false;
}

void ap_app_set_compare_original(ap_app *app, bool on)
{
    if (!app || !app->photo) return;
    if (app->compare_original == on) return;
    app->compare_original = on;

    ap_pipeline_graph *graph = ap_photo_graph(app->photo);
    if (!graph) return;

    // Skip (or restore) every stage that belongs to a user-edit stack
    // entry. Transport stages (demosaic, raw_passthrough, output_transfer)
    // have entry_idx == -1 and are left untouched so the display pipeline
    // stays coherent.
    //
    // When restoring (on = false) we reinstate the entry's own enabled
    // flag rather than force-enabling everything: a disabled entry must
    // stay skipped even after the compare ends.
    const ap_edit_stack *stack = ap_photo_stack(app->photo);
    int n = stack ? ap_edit_stack_count(stack) : 0;
    for (int i = 0; i < n; i++) {
        bool want_skip;
        if (on) {
            want_skip = true;
        } else {
            const ap_edit_entry *e = ap_edit_stack_at_const(stack, i);
            want_skip = e ? !e->enabled : false;
        }
        ap_pipeline_graph_set_stage_skip(graph, i, want_skip);
    }
}

int ap_app_request_jpeg_export(ap_app *app, ap_photo *photo,
                               const char *out_path, int quality)
{
    if (!app || !photo || !out_path) return -1;
    int w = ap_photo_width(photo);
    int h = ap_photo_height(photo);
    if (w <= 0 || h <= 0) return -1;

    size_t bytes = (size_t)w * (size_t)h * 4u;
    uint8_t *rgba = malloc(bytes);
    if (!rgba) {
        AP_ERROR("export: out of memory (%zu bytes)", bytes);
        return -1;
    }
    if (ap_pipeline_graph_readback(ap_photo_graph(photo), rgba, bytes) != 0) {
        free(rgba);
        return -1;
    }

    // Apply the viewport (crop / rotation / flip / scale) — the
    // pipeline rendered the full frame; the export rasterizes the
    // framed result. ap_viewport_resample_rgba8 mirrors the canvas
    // shader so the file matches what's on screen.
    {
        ap_viewport vp = ap_photo_viewport(photo);
        int framed_w = 0, framed_h = 0;
        uint8_t *framed = ap_viewport_resample_rgba8(&vp, rgba, w, h,
                                                     &framed_w, &framed_h);
        if (!framed) {
            AP_ERROR("export: out of memory framing the export");
            free(rgba);
            return -1;
        }
        free(rgba);
        rgba = framed;
        w = framed_w;
        h = framed_h;
    }

    export_job *j = calloc(1, sizeof(*j));
    if (!j) {
        AP_ERROR("export: job alloc failed");
        free(rgba);
        return -1;
    }
    j->base.run     = export_job_run;
    j->rgba         = rgba;
    j->width        = w;
    j->height       = h;
    j->format       = AP_EXPORT_FORMAT_JPEG;
    j->jpeg_quality = quality;
    snprintf(j->out_path, sizeof(j->out_path), "%s", out_path);

    app->export_inflight++;
    AP_INFO("export: queued %s (%dx%d, q=%d)", j->out_path, w, h, quality);
    ap_worker_pool_submit(app->workers, &j->base);
    return 0;
}

// Read back the open photo's rendered output, frame it through its
// viewport, and queue an encode job in the configured format. Shared
// by ap_app_run_export; the caller owns the collision-policy and
// directory-creation decisions and passes a final `out_path`.
static int queue_export_job(ap_app *app, ap_photo *photo,
                            const ap_export_settings *s,
                            const char *out_path)
{
    int w = ap_photo_width(photo);
    int h = ap_photo_height(photo);
    if (w <= 0 || h <= 0) return -1;

    size_t bytes = (size_t)w * (size_t)h * 4u;
    uint8_t *rgba = malloc(bytes);
    if (!rgba) {
        AP_ERROR("export: out of memory (%zu bytes)", bytes);
        return -1;
    }
    if (ap_pipeline_graph_readback(ap_photo_graph(photo), rgba, bytes) != 0) {
        free(rgba);
        return -1;
    }

    {
        ap_viewport vp = ap_photo_viewport(photo);
        int framed_w = 0, framed_h = 0;
        uint8_t *framed = ap_viewport_resample_rgba8(&vp, rgba, w, h,
                                                     &framed_w, &framed_h);
        if (!framed) {
            AP_ERROR("export: out of memory framing the export");
            free(rgba);
            return -1;
        }
        free(rgba);
        rgba = framed;
        w = framed_w;
        h = framed_h;
    }

    export_job *j = calloc(1, sizeof(*j));
    if (!j) {
        AP_ERROR("export: job alloc failed");
        free(rgba);
        return -1;
    }
    j->base.run       = export_job_run;
    j->rgba           = rgba;
    j->width          = w;
    j->height         = h;
    j->format         = s->format;
    j->jpeg_quality   = s->jpeg_quality;
    j->png_depth      = s->png_depth;
    j->tiff_depth     = s->tiff_depth;
    j->tiff_compress  = s->tiff_compress;
    snprintf(j->out_path, sizeof(j->out_path), "%s", out_path);

    app->export_inflight++;
    AP_INFO("export: queued %s (%dx%d, format=%d)",
            j->out_path, w, h, s->format);
    ap_worker_pool_submit(app->workers, &j->base);
    return 0;
}

ap_export_settings *ap_app_export_settings(ap_app *app)
{
    return app ? &app->export_settings : NULL;
}

void ap_app_enter_export(ap_app *app)
{
    if (!app || !app->photo) return;
    ap_export_settings_load(app->library, &app->export_settings);
    app->mode = AP_MODE_EXPORT;
    bind_mode_view(app);
}

// Build the output path for `src` under `s` using `seq` as the {SEQ}
// token, honouring the collision policy. Returns 0 on success and
// writes the path into `out` / `out_len`; returns 1 when the file
// was skipped by the SKIP policy; returns -1 on a hard error.
static int resolve_output_path(const ap_export_settings *s,
                               const char *src,
                               const char *library_root,
                               int seq,
                               char *out, size_t out_len)
{
    char dir[4096];
    if (ap_export_resolve_dir(s, src, library_root, dir, sizeof(dir)) != 0)
        return -1;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        AP_ERROR("export: mkdir(%s): %s", dir, strerror(errno));
        return -1;
    }

    const char *slash = strrchr(src, '/');
    const char *base  = slash ? slash + 1 : src;
    char src_stem[1024];
    snprintf(src_stem, sizeof(src_stem), "%s", base);
    char *dot = strrchr(src_stem, '.');
    if (dot) *dot = '\0';

    time_t when = time(NULL);
    {
        struct stat st;
        if (stat(src, &st) == 0) when = st.st_mtime;
    }

    char stem[1024];
    ap_export_format_stem(s, src_stem, when, seq, stem, sizeof(stem));
    const char *ext = ap_export_format_extension(s->format);

    int n = snprintf(out, out_len, "%s/%s.%s", dir, stem, ext);
    if (n <= 0 || (size_t)n >= out_len) {
        AP_ERROR("export: output path too long");
        return -1;
    }

    struct stat st;
    if (stat(out, &st) == 0) {
        if (s->collision == AP_EXPORT_COLLIDE_SKIP)
            return 1;
        if (s->collision == AP_EXPORT_COLLIDE_SUFFIX) {
            int suffix = 1;
            do {
                n = snprintf(out, out_len, "%s/%s_%d.%s",
                             dir, stem, suffix, ext);
                if (n <= 0 || (size_t)n >= out_len) {
                    AP_ERROR("export: suffixed path too long");
                    return -1;
                }
                suffix++;
            } while (stat(out, &st) == 0 && suffix < 10000);
        }
    }
    return 0;
}

int ap_app_run_export(ap_app *app)
{
    if (!app || !app->photo) return -1;
    const ap_export_settings *s = &app->export_settings;
    const char *src = ap_photo_path(app->photo);

    char out[4096];
    int pr = resolve_output_path(s, src,
                                 app->library ? ap_library_root(app->library)
                                              : NULL,
                                 1, out, sizeof(out));
    if (pr < 0) {
        ap_toast_push(AP_TOAST_ERROR, "Export: bad destination.");
        return -1;
    }
    if (pr == 1) {
        ap_toast_push(AP_TOAST_INFO, "Export skipped — file exists.");
        return 1;
    }

    if (queue_export_job(app, app->photo, s, out) != 0) {
        ap_toast_push(AP_TOAST_ERROR, "Export failed — see the log.");
        return -1;
    }

    // Persist the settings now that the user has committed to them.
    ap_export_settings_save(app->library, &app->export_settings);
    return 0;
}

int ap_app_batch_export_selection(ap_app *app, const ap_export_settings *s,
                                  int *out_queued, int *out_skipped)
{
    if (!app || !app->library || !app->grid || !s) return -1;

    if (out_queued)  *out_queued  = 0;
    if (out_skipped) *out_skipped = 0;

    const char *lib_root = ap_library_root(app->library);

    int seq     = 1;
    int queued  = 0;
    int skipped = 0;

    for (int c = 0; c < app->grid_map_count; c++) {
        if (!ap_grid_is_selected(app->grid, c)) continue;
        int i = app->grid_map[c];

        // Skip the currently-open photo: it may have unsaved in-memory
        // edits that differ from the sidecar; the user should close it
        // first if they want to include it.
        if (app->photo && i == app->photo_library_idx) continue;

        char abs[4096];
        if (ap_library_photo_absolute_path(app->library, i,
                                           abs, sizeof(abs)) != 0) {
            AP_WARN("batch export: cannot build path for index %d", i);
            continue;
        }

        char out_path[4096];
        int pr = resolve_output_path(s, abs, lib_root, seq,
                                     out_path, sizeof(out_path));
        if (pr < 0) {
            AP_ERROR("batch export: cannot resolve output path for %s", abs);
            continue;
        }
        if (pr == 1) {
            skipped++;
            seq++;
            continue;
        }

        // Open the photo synchronously on the main thread: decode raw +
        // build GPU pipeline. We operate an entirely separate ap_photo
        // so the canvas / open-photo state is untouched. The readback
        // inside queue_export_job calls vkDeviceWaitIdle on its own
        // graph — no need to touch gpu->current_graph here.
        ap_photo *tmp = ap_photo_open(app->gpu, abs);
        if (!tmp) {
            AP_WARN("batch export: cannot open %s — skipping", abs);
            continue;
        }

        if (queue_export_job(app, tmp, s, out_path) == 0) {
            queued++;
        } else {
            AP_WARN("batch export: encode queue failed for %s", abs);
        }

        // Close without persisting: we did not touch the sidecar and
        // the thumbnail is unchanged. The pipeline readback already
        // called vkDeviceWaitIdle, so the graph's images are idle.
        ap_photo_close(tmp);

        seq++;
    }

    if (out_queued)  *out_queued  = queued;
    if (out_skipped) *out_skipped = skipped;
    return 0;
}

// Rebuild the cell -> library-photo map from the active group filter,
// resize the grid to the visible count, and re-bind each visible
// cell's thumbnail from the library cache (the cells have just been
// reassigned, so their bound textures are stale). The selection and
// focus are preserved: photos that remain visible keep their selected
// state; photos filtered out simply drop from the selection.
void rebuild_grid_map(ap_app *app)
{
    if (!app->library) {
        app->grid_map_count = 0;
        if (app->grid) ap_grid_set_photo_count(app->grid, 0);
        return;
    }

    int n = ap_library_photo_count(app->library);

    // Snapshot which library indices are selected and which is focused
    // before touching anything, so we can restore them afterward.
    int   old_focus_lib = -1;
    bool *lib_was_sel   = NULL;
    if (app->grid && app->grid_map_count > 0 && n > 0) {
        lib_was_sel = calloc((size_t)n, sizeof(bool));
        if (lib_was_sel) {
            int fc = ap_grid_selected(app->grid);
            if (fc >= 0 && fc < app->grid_map_count)
                old_focus_lib = app->grid_map[fc];
            for (int c = 0; c < app->grid_map_count; c++) {
                if (ap_grid_is_selected(app->grid, c))
                    lib_was_sel[app->grid_map[c]] = true;
            }
        }
    }

    app->grid_map_count = 0;
    if (n > app->grid_map_cap) {
        int *m = realloc(app->grid_map, (size_t)n * sizeof(int));
        if (!m) {
            AP_ERROR("app: grid map allocation failed");
            free(lib_was_sel);
            return;
        }
        app->grid_map     = m;
        app->grid_map_cap = n;
    }

    for (int i = 0; i < n; i++) {
        bool show = true;
        if (app->group_filter_kind != AP_GROUP_FILTER_ALL) {
            const ap_photo_groups *g =
                ap_library_photo_groups(app->library, i);
            int gc = g ? g->count : 0;
            if (app->group_filter_kind == AP_GROUP_FILTER_UNGROUPED) {
                show = (gc == 0);
            } else {
                show = false;
                for (int k = 0; k < gc; k++) {
                    if (strcmp(g->names[k], app->group_filter_name) == 0) {
                        show = true;
                        break;
                    }
                }
            }
        }
        if (show && app->search_buf[0]) {
            const char *rel = ap_library_photo_relative_path(app->library, i);
            if (!rel || !strcasestr(rel, app->search_buf)) {
                show = false;
            }
        }
        if (show) {
            const ap_culling_filter *cf = &app->culling_filter;
            if (cf->rating_min > 0 || cf->flag != AP_FLAG_NONE ||
                    cf->color != AP_COLOR_NONE) {
                ap_photo_culling c = ap_library_photo_culling(app->library, i);
                if (cf->rating_min > 0 && c.rating < cf->rating_min)
                    show = false;
                if (show && cf->flag != AP_FLAG_NONE && c.flag != cf->flag)
                    show = false;
                if (show && cf->color != AP_COLOR_NONE && c.color != cf->color)
                    show = false;
            }
        }
        if (show) {
            app->grid_map[app->grid_map_count++] = i;
        }
    }

    if (!app->grid) {
        free(lib_was_sel);
        return;
    }
    ap_grid_set_photo_count(app->grid, app->grid_map_count);

    if (lib_was_sel && app->grid_map_count > 0) {
        // Restore focus to the cell showing the previously-focused photo,
        // falling back to cell 0 if that photo is no longer visible.
        int new_focus = 0;
        if (old_focus_lib >= 0 && old_focus_lib < n) {
            for (int c = 0; c < app->grid_map_count; c++) {
                if (app->grid_map[c] == old_focus_lib) {
                    new_focus = c;
                    break;
                }
            }
        }
        ap_grid_select_only(app->grid, new_focus);
        for (int c = 0; c < app->grid_map_count; c++) {
            if (c == new_focus) continue;
            if (lib_was_sel[app->grid_map[c]])
                ap_grid_select_toggle(app->grid, c);
        }
    } else {
        ap_grid_set_selected(app->grid, 0);
    }

    free(lib_was_sel);

    for (int c = 0; c < app->grid_map_count; c++) {
        ap_thumbnail *t = ap_library_thumbnail(app->library,
                                               app->grid_map[c]);
        if (t) {
            ap_grid_set_thumbnail(app->grid, c, ap_thumbnail_view(t),
                                  ap_thumbnail_sampler(t),
                                  ap_thumbnail_width(t), ap_thumbnail_height(t));
        } else {
            ap_grid_set_thumbnail(app->grid, c,
                                  VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 0);
        }
    }
}

// Grid cell currently showing library photo `photo_idx`, or -1 when
// that photo is filtered out of the visible grid.
int cell_for_photo(const ap_app *app, int photo_idx)
{
    for (int c = 0; c < app->grid_map_count; c++) {
        if (app->grid_map[c] == photo_idx) return c;
    }
    return -1;
}

int ap_app_open_library(ap_app *app, const char *path)
{
    if (!app || !path) return -1;

    ap_app_close_photo(app);
    ap_app_close_library(app);

    app->library = ap_library_open(path);
    if (!app->library) {
        return -1;
    }
    app->group_filter_kind    = AP_GROUP_FILTER_ALL;
    app->group_filter_name[0] = '\0';
    app->sort                 = AP_SORT_PATH;
    app->search_buf[0]        = '\0';
    app->culling_filter       = (ap_culling_filter){ 0, AP_FLAG_NONE, AP_COLOR_NONE };
    rebuild_grid_map(app);
    app->mode = AP_MODE_LIBRARY;
    bind_mode_view(app);
    refresh_window_title(app);
    return 0;
}

void ap_app_close_library(ap_app *app)
{
    if (!app || !app->library) return;

    // Wait for outstanding decode work to land in the completed queue
    // and toss the buffers - they belong to the library that's going
    // away. Then drop GPU references before the textures vanish.
    ap_worker_pool_wait_idle(app->workers);
    drain_all_workers(app);
    ap_app_wait_idle(app);
    ap_grid_set_photo_count(app->grid, 0);
    app->grid_map_count = 0;
    ap_library_close(app->library);
    app->library = NULL;
    bind_mode_view(app);
    refresh_window_title(app);
}

ap_library *ap_app_library(ap_app *app)
{
    return app ? app->library : NULL;
}

void ap_app_request_import(ap_app *app)
{
    if (app && app->library) app->import_modal = true;
}

int ap_app_apply_pipeline_to_selection(ap_app *app, int64_t pipeline_id)
{
    if (!app || !app->library || !app->grid) return -1;

    int wrote = 0;
    for (int c = 0; c < app->grid_map_count; c++) {
        if (!ap_grid_is_selected(app->grid, c)) continue;
        int i = app->grid_map[c];
        // Skip the open photo: rewriting its sidecar would leave the
        // in-memory stack stale until the user closes + reopens.
        if (app->photo && i == app->photo_library_idx) continue;
        if (ap_library_apply_pipeline_to_photo(app->library, i,
                                               pipeline_id) == 0) {
            wrote++;
            // Drop the cached thumbnail so the grid re-decodes against
            // the new stack on the next pump cycle. The stored
            // edit-render blob's freshness check already handles the
            // sidecar-mtime side.
            ap_library_invalidate_thumbnail(app->library, i);
        }
    }
    return wrote;
}

int ap_app_copy_edits(ap_app *app)
{
    if (!app || !app->photo) return -1;
    const ap_edit_stack *stack = ap_photo_stack(app->photo);
    if (!stack) return -1;
    app->edit_clipboard       = *stack;
    app->edit_clipboard_valid = true;
    return 0;
}

int ap_app_paste_edits(ap_app *app)
{
    if (!app || !app->photo || !app->edit_clipboard_valid) return -1;
    ap_edit_stack *stack = ap_photo_stack(app->photo);
    if (!stack) return -1;
    ap_photo_edit_snapshot(app->photo);
    *stack = app->edit_clipboard;
    ap_app_rebuild_photo_graph(app);
    return 0;
}

bool ap_app_has_edit_clipboard(const ap_app *app)
{
    return app && app->edit_clipboard_valid;
}

void ap_app_edit_snapshot(ap_app *app)
{
    if (!app || !app->photo) return;
    ap_photo_edit_snapshot(app->photo);
}

bool ap_app_undo(ap_app *app)
{
    if (!app || !app->photo) return false;
    if (!ap_photo_undo(app->photo)) return false;
    ap_app_rebuild_photo_graph(app);
    return true;
}

bool ap_app_redo(ap_app *app)
{
    if (!app || !app->photo) return false;
    if (!ap_photo_redo(app->photo)) return false;
    ap_app_rebuild_photo_graph(app);
    return true;
}

int ap_app_sync_edits_to_selection(ap_app *app)
{
    if (!app || !app->library || !app->grid) return -1;
    if (!app->edit_clipboard_valid) return -1;

    int wrote = 0;
    for (int c = 0; c < app->grid_map_count; c++) {
        if (!ap_grid_is_selected(app->grid, c)) continue;
        int i = app->grid_map[c];
        if (app->photo && i == app->photo_library_idx) continue;
        if (ap_library_apply_stack_to_photo(app->library, i,
                                            &app->edit_clipboard) == 0) {
            wrote++;
            ap_library_invalidate_thumbnail(app->library, i);
        }
    }
    return wrote;
}

int ap_app_apply_metadata_to_selection(ap_app *app,
                                       const ap_photo_metadata *patch,
                                       const bool patch_set[AP_META_FIELD_COUNT])
{
    if (!app || !patch || !patch_set) return -1;
    if (!app->library || !app->grid)  return -1;

    int wrote = 0;
    for (int c = 0; c < app->grid_map_count; c++) {
        if (!ap_grid_is_selected(app->grid, c)) continue;
        int i = app->grid_map[c];
        if (ap_library_apply_metadata_patch(app->library, i,
                                            patch, patch_set) == 0) {
            wrote++;
        }
    }
    return wrote;
}

// Culling field to mutate across the selection. Each enumerator
// targets exactly one ap_photo_culling member.
typedef enum {
    AP_CULL_RATING = 0,
    AP_CULL_FLAG   = 1,
    AP_CULL_COLOR  = 2,
} ap_cull_field;

// Apply a single culling-field change to every selected grid photo.
// `value` is interpreted per `field`: a rating int, an ap_flag, or an
// ap_color_label. Returns the number of photos written, or -1 on a
// missing library / grid.
static int apply_culling_to_selection(ap_app *app, ap_cull_field field,
                                      int value)
{
    if (!app || !app->library || !app->grid) return -1;

    int wrote = 0;
    for (int c = 0; c < app->grid_map_count; c++) {
        if (!ap_grid_is_selected(app->grid, c)) continue;
        int i = app->grid_map[c];
        ap_photo_culling cull = ap_library_photo_culling(app->library, i);
        switch (field) {
        case AP_CULL_RATING: cull.rating = value;                  break;
        case AP_CULL_FLAG:   cull.flag   = (ap_flag)value;         break;
        case AP_CULL_COLOR:  cull.color  = (ap_color_label)value;  break;
        }
        if (ap_library_set_photo_culling(app->library, i, cull) == 0) {
            wrote++;
        }
    }
    return wrote;
}

int ap_app_set_selection_rating(ap_app *app, int rating)
{
    return apply_culling_to_selection(app, AP_CULL_RATING,
                                      ap_rating_clamp(rating));
}

int ap_app_set_selection_flag(ap_app *app, ap_flag flag)
{
    return apply_culling_to_selection(app, AP_CULL_FLAG, (int)flag);
}

int ap_app_set_selection_color(ap_app *app, ap_color_label color)
{
    return apply_culling_to_selection(app, AP_CULL_COLOR, (int)color);
}

void ap_app_set_group_filter(ap_app *app, int kind, const char *name)
{
    if (!app) return;
    app->group_filter_kind = kind;
    if (kind == AP_GROUP_FILTER_GROUP && name) {
        snprintf(app->group_filter_name, sizeof(app->group_filter_name),
                 "%s", name);
    } else {
        app->group_filter_name[0] = '\0';
    }
    rebuild_grid_map(app);
}

int ap_app_group_filter_kind(const ap_app *app)
{
    return app ? app->group_filter_kind : AP_GROUP_FILTER_ALL;
}

const char *ap_app_group_filter_name(const ap_app *app)
{
    return app ? app->group_filter_name : "";
}

void ap_app_set_sort(ap_app *app, ap_library_sort sort)
{
    if (!app || !app->library) return;
    if (app->sort == sort) return;
    app->sort = sort;
    // Drain in-flight thumbnail jobs before reordering: jobs carry a
    // library index and the reload shifts every index.
    drain_all_workers(app);
    ap_app_wait_idle(app);
    ap_library_reload_sorted(app->library, sort);
    // Clear grid thumbnails: the thumbnail cache (thumbs[]) was reset
    // by the reload, and grid cells still hold stale VkImageViews.
    if (app->grid) {
        int c;
        for (c = 0; c < app->grid_map_count; c++) {
            ap_grid_set_thumbnail(app->grid, c, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 0);
        }
    }
    rebuild_grid_map(app);
}

ap_library_sort ap_app_sort(const ap_app *app)
{
    return app ? app->sort : AP_SORT_PATH;
}

void ap_app_set_search(ap_app *app, const char *query)
{
    if (!app) return;
    snprintf(app->search_buf, sizeof(app->search_buf), "%s", query ? query : "");
    rebuild_grid_map(app);
}

const char *ap_app_search(const ap_app *app)
{
    return app ? app->search_buf : "";
}

ap_culling_filter ap_app_culling_filter(const ap_app *app)
{
    if (!app) return (ap_culling_filter){ 0, AP_FLAG_NONE, AP_COLOR_NONE };
    return app->culling_filter;
}

void ap_app_set_culling_filter(ap_app *app, ap_culling_filter filter)
{
    if (!app) return;
    app->culling_filter = filter;
    rebuild_grid_map(app);
}

void ap_app_set_canvas_tool(ap_app *app, ap_canvas_tool tool, int entry_idx)
{
    if (!app) return;
    // Re-arming the same tool on the same entry toggles it off, so the
    // config-window button reads as a press-on / press-off control.
    if (tool != AP_CANVAS_TOOL_NONE &&
        app->canvas_tool == tool && app->canvas_tool_entry == entry_idx) {
        app->canvas_tool       = AP_CANVAS_TOOL_NONE;
        app->canvas_tool_entry = -1;
    } else {
        app->canvas_tool       = tool;
        app->canvas_tool_entry = (tool == AP_CANVAS_TOOL_NONE) ? -1
                                                               : entry_idx;
    }
    app->crop_drag_handle = CROP_HANDLE_NONE;
}

ap_canvas_tool ap_app_canvas_tool(const ap_app *app)
{
    return app ? app->canvas_tool : AP_CANVAS_TOOL_NONE;
}

int ap_app_canvas_tool_entry(const ap_app *app)
{
    return app ? app->canvas_tool_entry : -1;
}

int ap_app_grid_selection_count(const ap_app *app)
{
    if (!app || !app->grid) return 0;
    return ap_grid_selection_count(app->grid);
}

int ap_app_assign_selection_to_group(ap_app *app, const char *group, bool add)
{
    if (!app || !app->library || !app->grid || !group || !*group) {
        return -1;
    }
    int wrote = 0;
    for (int c = 0; c < app->grid_map_count; c++) {
        if (!ap_grid_is_selected(app->grid, c)) continue;
        if (ap_library_set_photo_group(app->library, app->grid_map[c],
                                       group, add) == 0) {
            wrote++;
        }
    }
    // Membership changed — a group filter's visible set may have moved.
    rebuild_grid_map(app);
    return wrote;
}

void navigate_library_relative(ap_app *app, int dir)
{
    // Walks the library list while staying in photo mode. The
    // library's grid selection is intentionally untouched - it
    // represents the user's earlier intent in library mode and
    // should survive this navigation so backing out (Esc) puts
    // them where they were.
    if (!app->library) return;
    int n = ap_library_photo_count(app->library);
    if (n <= 0 || app->photo_library_idx < 0) return;
    int new_idx = app->photo_library_idx + dir;
    if (new_idx < 0 || new_idx >= n) return;

    char abs[4096];
    if (ap_library_photo_absolute_path(app->library, new_idx,
                                       abs, sizeof(abs)) != 0) return;

    // Refresh the outgoing photo's thumbnail before the async open
    // replaces app->photo — the readback must happen while the graph
    // is still live.
    submit_thumb_refresh(app, app->photo_library_idx);

    app->photo_library_idx = new_idx;
    ap_app_open_photo(app, abs);
}

// Resolve the edit-stack entry an armed canvas tool drives, verifying
// it is still the module the tool expects. A stack reorder / removal
// can shift indices out from under the stored binding; when the entry
// no longer matches, the tool is disarmed and NULL returned rather
// than writing into the wrong module's params.
ap_edit_entry *canvas_tool_entry(ap_app *app, const char *module)
{
    if (!app->photo || app->canvas_tool_entry < 0) return NULL;
    ap_edit_stack *stack = ap_photo_stack(app->photo);
    if (!stack) return NULL;
    ap_edit_entry *e = ap_edit_stack_at(stack, app->canvas_tool_entry);
    if (!e || strcmp(e->module_name, module) != 0) {
        app->canvas_tool       = AP_CANVAS_TOOL_NONE;
        app->canvas_tool_entry = -1;
        return NULL;
    }
    return e;
}

// White-balance eyedropper: on a left-click inside the image, sample
// the clicked pixel from the rendered output and solve the bound White
// Balance entry's multipliers to neutralise it. The rendered image is
// read back in full (synchronous, one-shot on click) and the source
// pixel under the cursor is sampled. Returns true when the click was
// consumed so the caller skips view panning this frame.
static bool drive_wb_eyedropper(ap_app *app, ImGuiIO *io)
{
    ap_edit_entry *e = canvas_tool_entry(app, "wb");
    if (!e) return false;

    igSetMouseCursor(ImGuiMouseCursor_Hand);

    if (io->WantCaptureMouse) return false;
    if (!igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) return false;

    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;

    float u = 0.0f, v = 0.0f;
    if (!ap_canvas_screen_to_source_uv(app->canvas, io->MousePos.x,
                                       io->MousePos.y, win_w, win_h,
                                       &u, &v)) {
        return false;
    }

    ap_pipeline_graph *graph = ap_photo_graph(app->photo);
    if (!graph) return false;
    int gw = ap_pipeline_graph_output_width(graph);
    int gh = ap_pipeline_graph_output_height(graph);
    if (gw <= 0 || gh <= 0) return false;

    size_t bytes = (size_t)gw * (size_t)gh * 4u;
    uint8_t *rgba = malloc(bytes);
    if (!rgba) {
        AP_ERROR("eyedropper: out of memory (%zu bytes)", bytes);
        return true;
    }
    if (ap_pipeline_graph_readback(graph, rgba, bytes) != 0) {
        free(rgba);
        ap_toast_push(AP_TOAST_ERROR, "Eyedropper: pixel readback failed.");
        return true;
    }

    int px = (int)(u * (float)gw);
    int py = (int)(v * (float)gh);
    if (px < 0) px = 0; else if (px >= gw) px = gw - 1;
    if (py < 0) py = 0; else if (py >= gh) py = gh - 1;
    const uint8_t *p = &rgba[((size_t)py * (size_t)gw + (size_t)px) * 4u];
    float sr = (float)p[0], sg = (float)p[1], sb = (float)p[2];
    free(rgba);

    ap_app_edit_snapshot(app);
    if (ap_wb_apply_neutral_pick(e->params, sr, sg, sb)) {
        ap_app_rebuild_photo_graph(app);
        ap_toast_push(AP_TOAST_INFO, "White balance set from picked pixel.");
    } else {
        ap_toast_push(AP_TOAST_INFO,
                      "Eyedropper: pixel too dark — pick a brighter area.");
    }
    return true;
}

// Crop-overlay geometry: the eight handle positions of the crop rect
// in framed-output [0,1] space. The crop rect is itself a [0,1] sub-
// rect of the framed output. Order matches the CROP_HANDLE_* enum for
// the corner / edge handles (0..7).
void crop_handle_uv(const ap_viewport *vp, int handle,
                    float *u, float *v)
{
    float x0 = vp->crop_x0, y0 = vp->crop_y0;
    float x1 = vp->crop_x1, y1 = vp->crop_y1;
    float mx = 0.5f * (x0 + x1);
    float my = 0.5f * (y0 + y1);
    switch (handle) {
    case CROP_HANDLE_TL: *u = x0; *v = y0; break;
    case CROP_HANDLE_TR: *u = x1; *v = y0; break;
    case CROP_HANDLE_BL: *u = x0; *v = y1; break;
    case CROP_HANDLE_BR: *u = x1; *v = y1; break;
    case CROP_HANDLE_L:  *u = x0; *v = my; break;
    case CROP_HANDLE_R:  *u = x1; *v = my; break;
    case CROP_HANDLE_T:  *u = mx; *v = y0; break;
    case CROP_HANDLE_B:  *u = mx; *v = y1; break;
    default:             *u = mx; *v = my; break;
    }
}

// Re-clamp a crop rect to stay inside [0,1], keep a minimum size, and
// keep x0<x1 / y0<y1.
void crop_rect_sanitize(ap_viewport *vp)
{
    const float min_sz = 0.02f;
    if (vp->crop_x0 < 0.0f) vp->crop_x0 = 0.0f;
    if (vp->crop_y0 < 0.0f) vp->crop_y0 = 0.0f;
    if (vp->crop_x1 > 1.0f) vp->crop_x1 = 1.0f;
    if (vp->crop_y1 > 1.0f) vp->crop_y1 = 1.0f;
    if (vp->crop_x1 - vp->crop_x0 < min_sz) {
        if (vp->crop_x0 + min_sz <= 1.0f) vp->crop_x1 = vp->crop_x0 + min_sz;
        else vp->crop_x0 = vp->crop_x1 - min_sz;
    }
    if (vp->crop_y1 - vp->crop_y0 < min_sz) {
        if (vp->crop_y0 + min_sz <= 1.0f) vp->crop_y1 = vp->crop_y0 + min_sz;
        else vp->crop_y0 = vp->crop_y1 - min_sz;
    }
}

// Apply a drag at framed-output coordinate (u,v) to the crop rect for
// the given handle. When the aspect lock is on, edge / corner drags
// hold app->crop_aspect_ratio (width/height of the framed image's crop
// in pixels). The straighten handle is handled separately.
static void crop_apply_drag(ap_app *app, ap_viewport *vp, int handle,
                            float u, float v, int img_w, int img_h)
{
    if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
    if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;

    if (handle == CROP_HANDLE_MOVE) {
        float w  = vp->crop_x1 - vp->crop_x0;
        float h  = vp->crop_y1 - vp->crop_y0;
        float nx = u - w * 0.5f;
        float ny = v - h * 0.5f;
        if (nx < 0.0f) nx = 0.0f;
        if (ny < 0.0f) ny = 0.0f;
        if (nx + w > 1.0f) nx = 1.0f - w;
        if (ny + h > 1.0f) ny = 1.0f - h;
        vp->crop_x0 = nx; vp->crop_y0 = ny;
        vp->crop_x1 = nx + w; vp->crop_y1 = ny + h;
        return;
    }

    switch (handle) {
    case CROP_HANDLE_TL: vp->crop_x0 = u; vp->crop_y0 = v; break;
    case CROP_HANDLE_TR: vp->crop_x1 = u; vp->crop_y0 = v; break;
    case CROP_HANDLE_BL: vp->crop_x0 = u; vp->crop_y1 = v; break;
    case CROP_HANDLE_BR: vp->crop_x1 = u; vp->crop_y1 = v; break;
    case CROP_HANDLE_L:  vp->crop_x0 = u; break;
    case CROP_HANDLE_R:  vp->crop_x1 = u; break;
    case CROP_HANDLE_T:  vp->crop_y0 = v; break;
    case CROP_HANDLE_B:  vp->crop_y1 = v; break;
    default: break;
    }

    // Keep ordering before the aspect step so width / height are signed
    // correctly.
    if (vp->crop_x1 < vp->crop_x0) {
        float t = vp->crop_x0; vp->crop_x0 = vp->crop_x1; vp->crop_x1 = t;
    }
    if (vp->crop_y1 < vp->crop_y0) {
        float t = vp->crop_y0; vp->crop_y0 = vp->crop_y1; vp->crop_y1 = t;
    }

    // Aspect lock: after a free drag, recover the locked ratio by
    // pulling the axis that was *not* dragged. Ratio is in pixels, so
    // convert through the framed image dimensions.
    if (app->crop_aspect_locked && app->crop_aspect_ratio > 0.0f &&
        img_w > 0 && img_h > 0) {
        float ratio_norm = app->crop_aspect_ratio
                         * (float)img_h / (float)img_w;
        bool x_edge = (handle == CROP_HANDLE_L || handle == CROP_HANDLE_R);
        bool y_edge = (handle == CROP_HANDLE_T || handle == CROP_HANDLE_B);
        float cw = vp->crop_x1 - vp->crop_x0;
        float ch = vp->crop_y1 - vp->crop_y0;
        if (y_edge) {
            cw = ch * ratio_norm;
        } else if (x_edge) {
            ch = cw / ratio_norm;
        } else {
            ch = cw / ratio_norm;
        }
        switch (handle) {
        case CROP_HANDLE_TL:
            vp->crop_x0 = vp->crop_x1 - cw;
            vp->crop_y0 = vp->crop_y1 - ch; break;
        case CROP_HANDLE_TR:
            vp->crop_x1 = vp->crop_x0 + cw;
            vp->crop_y0 = vp->crop_y1 - ch; break;
        case CROP_HANDLE_BL:
            vp->crop_x0 = vp->crop_x1 - cw;
            vp->crop_y1 = vp->crop_y0 + ch; break;
        case CROP_HANDLE_BR:
            vp->crop_x1 = vp->crop_x0 + cw;
            vp->crop_y1 = vp->crop_y0 + ch; break;
        case CROP_HANDLE_L: case CROP_HANDLE_R: {
            float mid = 0.5f * (vp->crop_y0 + vp->crop_y1);
            vp->crop_y0 = mid - ch * 0.5f;
            vp->crop_y1 = mid + ch * 0.5f; break;
        }
        case CROP_HANDLE_T: case CROP_HANDLE_B: {
            float mid = 0.5f * (vp->crop_x0 + vp->crop_x1);
            vp->crop_x0 = mid - cw * 0.5f;
            vp->crop_x1 = mid + cw * 0.5f; break;
        }
        default: break;
        }
    }

    crop_rect_sanitize(vp);
}

// Interactive crop / straighten overlay. Drives the bound Transform
// entry's crop + rotation slots from drag handles. While the tool is
// armed the canvas is fed an identity-crop viewport (full frame
// visible) so the user always sees the whole image to crop within;
// drive_canvas_view's pan is suppressed by the caller. Returns nothing
// — the drawing half is draw_crop_overlay.
static void drive_crop_tool(ap_app *app, ImGuiIO *io)
{
    ap_edit_entry *e = canvas_tool_entry(app, "transform");
    if (!e) return;

    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;
    int img_w = ap_photo_width(app->photo);
    int img_h = ap_photo_height(app->photo);

    ap_viewport vp = ap_transform_viewport(e->params);

    const float grab_r = 11.0f;

    if (io->WantCaptureMouse && app->crop_drag_handle == CROP_HANDLE_NONE) {
        return;
    }

    if (igIsMouseClicked_Bool(ImGuiMouseButton_Left, false) &&
        app->crop_drag_handle == CROP_HANDLE_NONE) {
        float best_d2 = grab_r * grab_r;
        int   best    = CROP_HANDLE_NONE;
        for (int h = CROP_HANDLE_TL; h <= CROP_HANDLE_B; h++) {
            float hu, hv, sx, sy;
            crop_handle_uv(&vp, h, &hu, &hv);
            ap_canvas_framed_uv_to_screen(app->canvas, hu, hv,
                                          win_w, win_h, &sx, &sy);
            float dx = sx - io->MousePos.x;
            float dy = sy - io->MousePos.y;
            float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best_d2 = d2; best = h; }
        }
        if (best == CROP_HANDLE_NONE) {
            float fu, fv;
            ap_canvas_screen_to_framed_uv(app->canvas, io->MousePos.x,
                                          io->MousePos.y, win_w, win_h,
                                          &fu, &fv);
            if (fu >= vp.crop_x0 && fu <= vp.crop_x1 &&
                fv >= vp.crop_y0 && fv <= vp.crop_y1) {
                if (io->KeyShift) {
                    best = CROP_HANDLE_STRAIGHTEN;
                    app->crop_drag_u0 = fu;
                    app->crop_drag_v0 = fv;
                } else {
                    best = CROP_HANDLE_MOVE;
                }
            }
        }
        if (best != CROP_HANDLE_NONE) {
            ap_app_edit_snapshot(app);
            app->crop_drag_handle = best;
        }
    }

    if (app->crop_drag_handle != CROP_HANDLE_NONE) {
        if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
            app->crop_drag_handle = CROP_HANDLE_NONE;
        } else if (app->crop_drag_handle == CROP_HANDLE_STRAIGHTEN) {
            // Straighten: the drag vector's angle off horizontal is the
            // correction; add it to the current rotation. Converting to
            // screen space keeps the angle visually true under the
            // canvas zoom (uniform scale -> angle preserved).
            float su0, sv0, su1, sv1;
            ap_canvas_framed_uv_to_screen(app->canvas, app->crop_drag_u0,
                                          app->crop_drag_v0, win_w, win_h,
                                          &su0, &sv0);
            su1 = io->MousePos.x;
            sv1 = io->MousePos.y;
            float dx = su1 - su0;
            float dy = sv1 - sv0;
            if (dx * dx + dy * dy > 4.0f) {
                float ang = atan2f(dy, dx) * (float)(180.0 / M_PI);
                // Snap the drawn line to the nearest of horizontal /
                // vertical: a near-vertical drag means "this should be
                // vertical".
                if (ang > 90.0f)       ang -= 180.0f;
                else if (ang < -90.0f) ang += 180.0f;
                if (ang > 45.0f)       ang -= 90.0f;
                else if (ang < -45.0f) ang += 90.0f;
                float rot = vp.rotation_deg - ang;
                if (rot > 180.0f)  rot -= 360.0f;
                if (rot < -180.0f) rot += 360.0f;
                if (rot > 180.0f)  rot = 180.0f;
                if (rot < -180.0f) rot = -180.0f;
                vp.rotation_deg = rot;
            }
        } else {
            float fu, fv;
            ap_canvas_screen_to_framed_uv(app->canvas, io->MousePos.x,
                                          io->MousePos.y, win_w, win_h,
                                          &fu, &fv);
            crop_apply_drag(app, &vp, app->crop_drag_handle, fu, fv,
                            img_w, img_h);
        }
        ap_transform_set_viewport(e->params, &vp);
    }
}

// Pan / zoom the canvas from the mouse and the F / 0 / 1 view keys.
// Shared by photo mode and export mode — both present a single photo
// on the canvas with the same manipulation feel. Skips entirely while
// ImGui owns the mouse so panel drags don't pan the image.
static void drive_canvas_view(ap_app *app, ImGuiIO *io)
{
    if (io->WantCaptureMouse) return;

    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;

    if (io->MouseDown[0] && (io->MouseDelta.x != 0.0f || io->MouseDelta.y != 0.0f)) {
        ap_canvas_pan(app->canvas, io->MouseDelta.x, io->MouseDelta.y,
                      win_w, win_h);
    }
    // Wheel semantics:
    //   plain wheel  → pan (vertical + horizontal). Trackpad
    //                  two-finger scroll lands here and feels native.
    //   Ctrl + wheel → zoom-at-cursor. Compositors translate trackpad
    //                  pinch into Ctrl+vertical-scroll, so pinch-to-
    //                  zoom works without GLFW gesture plumbing.
    if (io->MouseWheel != 0.0f || io->MouseWheelH != 0.0f) {
        if (io->KeyCtrl && io->MouseWheel != 0.0f) {
            float factor = io->MouseWheel > 0.0f
                ? 1.0f + ZOOM_FACTOR * io->MouseWheel
                : 1.0f / (1.0f - ZOOM_FACTOR * io->MouseWheel);
            ap_canvas_zoom_at(app->canvas, factor,
                              io->MousePos.x, io->MousePos.y,
                              win_w, win_h);
        } else {
            const float pan_step_px = 40.0f;
            ap_canvas_pan(app->canvas,
                          io->MouseWheelH * pan_step_px,
                          -io->MouseWheel  * pan_step_px,
                          win_w, win_h);
        }
    }

    if (igIsKeyPressed_Bool(ImGuiKey_F, false) ||
        igIsKeyPressed_Bool(ImGuiKey_0, false)) {
        ap_canvas_reset_view(app->canvas);
    } else if (igIsKeyPressed_Bool(ImGuiKey_1, false)) {
        ap_canvas_set_zoom(app->canvas, 1.0f, win_w, win_h);
    }
}

static void drive_canvas_input(ap_app *app)
{
    if (!app->canvas || !app->photo) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
        // Escape disarms an active canvas tool first; only with no tool
        // armed does it close the photo.
        if (app->canvas_tool != AP_CANVAS_TOOL_NONE) {
            ap_app_set_canvas_tool(app, AP_CANVAS_TOOL_NONE, -1);
        } else {
            ap_app_close_photo(app);
        }
        return;
    }

    // Prev/next photo. Gate on WantTextInput, not WantCaptureKeyboard:
    // the always-present docked panels keep WantCaptureKeyboard true,
    // which would block navigation entirely. WantTextInput is only
    // set while an actual text field (e.g. the rename box) is active,
    // which is the one case where arrows should be left to ImGui.
    if (!io->WantTextInput) {
        if (igIsKeyPressed_Bool(ImGuiKey_RightArrow, true)) {
            navigate_library_relative(app, +1);
            return;
        }
        if (igIsKeyPressed_Bool(ImGuiKey_LeftArrow, true)) {
            navigate_library_relative(app, -1);
            return;
        }
        if (igIsKeyPressed_Bool(ImGuiKey_Delete, false) &&
            app->photo && app->photo_library_idx >= 0) {
            if (io->KeyShift) {
                delete_edit_photo(app);
            } else {
                app->delete_edit_modal = true;
            }
            return;
        }
    }

    // Interactive canvas tools take the mouse when armed. The
    // eyedropper consumes a click but otherwise leaves the view free
    // to pan / zoom between picks; the crop tool owns mouse drags
    // entirely, so view panning is suppressed while it is armed.
    switch (app->canvas_tool) {
    case AP_CANVAS_TOOL_WB_EYEDROPPER:
        if (drive_wb_eyedropper(app, io)) return;
        break;
    case AP_CANVAS_TOOL_CROP:
        drive_crop_tool(app, io);
        return;
    case AP_CANVAS_TOOL_NONE:
        break;
    }

    drive_canvas_view(app, io);
}

// Export-mode canvas input. The same pan / zoom feel as photo mode,
// but Esc backs out to photo mode rather than closing the photo —
// the photo is the export subject and stays open.
static void drive_export_input(ap_app *app)
{
    if (!app->canvas || !app->photo) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    if (!io->WantTextInput && igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
        app->mode = AP_MODE_PHOTO;
        bind_mode_view(app);
        return;
    }

    drive_canvas_view(app, io);
}

void open_selected_photo(ap_app *app)
{
    if (!app->library || !app->grid) return;
    int cell = ap_grid_selected(app->grid);
    if (cell < 0 || cell >= app->grid_map_count) return;
    int idx = app->grid_map[cell];

    char abs[4096];
    if (ap_library_photo_absolute_path(app->library, idx, abs, sizeof(abs)) != 0) {
        AP_ERROR("library: photo path overflow at idx %d", idx);
        return;
    }
    app->photo_library_idx = idx;
    if (ap_app_open_photo(app, abs) != 0) {
        AP_ERROR("library: failed to open %s", abs);
    }
}

// De-index and delete every selected library photo from disk. Drains
// worker + GPU work first so no in-flight job acts on a library index
// this removal is about to shift.
void delete_grid_selection(ap_app *app)
{
    if (!app->library || !app->grid) return;
    if (ap_grid_selection_count(app->grid) <= 0) return;

    drain_all_workers(app);
    ap_app_wait_idle(app);

    int anchor_cell = -1;
    int removed     = 0;

    // grid_map is ascending in library index, so walking cells
    // high-to-low removes photos from the back — the indices of the
    // photos still pending stay valid as we go. The descending walk
    // also leaves anchor_cell holding the smallest selected cell.
    for (int c = app->grid_map_count - 1; c >= 0; c--) {
        if (!ap_grid_is_selected(app->grid, c)) continue;
        anchor_cell = c;
        int i = app->grid_map[c];
        // Leave the open photo — deleting its file out from under the
        // edit view would strand it. It can be deleted after closing.
        if (app->photo && i == app->photo_library_idx) continue;
        if (ap_library_photo_remove(app->library, i) != 0) continue;
        if (app->photo && i < app->photo_library_idx) {
            app->photo_library_idx--;
        }
        removed++;
    }
    if (removed == 0) return;

    rebuild_grid_map(app);
    if (app->grid_map_count > 0) {
        // Land on the photo that slid into the lowest deleted cell —
        // the nearest remaining neighbour — and scroll to it, since
        // rebuild_grid_map reset the grid's scroll to the top.
        int target = anchor_cell;
        if (target >= app->grid_map_count) {
            target = app->grid_map_count - 1;
        }
        if (target < 0) target = 0;
        ap_grid_select_only(app->grid, target);

        ImGuiIO *io = igGetIO_Nil();
        if (io) {
            ap_grid_ensure_visible(app->grid, target,
                                   (int)io->DisplaySize.x,
                                   (int)io->DisplaySize.y);
        }
    }
    AP_INFO("library: deleted %d photo(s)", removed);
}

// Close the currently-open photo, de-index it, delete its files from
// disk, then navigate to an adjacent photo (next if available, else
// previous, else return to the library grid).
void delete_edit_photo(ap_app *app)
{
    if (!app->library || !app->photo) return;

    int idx = app->photo_library_idx;
    if (idx < 0) return;

    drain_all_workers(app);
    ap_app_wait_idle(app);

    // Release the photo before removal so its files are not open.
    release_photo(app);
    app->photo_library_idx = -1;

    if (ap_library_photo_remove(app->library, idx) != 0) {
        // Removal failed — back to library so the user isn't stranded.
        app->mode = AP_MODE_LIBRARY;
        bind_mode_view(app);
        rebuild_grid_map(app);
        return;
    }

    int n_after = ap_library_photo_count(app->library);

    if (n_after <= 0) {
        app->mode = AP_MODE_LIBRARY;
        bind_mode_view(app);
        rebuild_grid_map(app);
        AP_INFO("library: deleted photo (library now empty)");
        return;
    }

    // Try the photo that slid into `idx` (was the next one); if `idx`
    // is now out of range fall back to the last photo.
    int next_idx = (idx < n_after) ? idx : (n_after - 1);

    char abs[4096];
    if (ap_library_photo_absolute_path(app->library, next_idx,
                                       abs, sizeof(abs)) != 0) {
        app->mode = AP_MODE_LIBRARY;
        bind_mode_view(app);
        rebuild_grid_map(app);
        return;
    }

    // Update grid selection so backing out (Esc) lands on the right cell.
    rebuild_grid_map(app);
    {
        int cell = cell_for_photo(app, next_idx);
        if (cell >= 0) ap_grid_select_only(app->grid, cell);
    }

    app->photo_library_idx = next_idx;
    ap_app_open_photo(app, abs);
    AP_INFO("library: deleted photo (navigated to idx %d)", next_idx);
}

// Zoom-level readout drawn over the canvas corner: "Fit" at the
// default view, "100%" when the effective scale is 1:1, else "N%".
// The label uses the foreground draw list so it paints over the canvas
// but under any ImGui windows.
static void draw_canvas_zoom_overlay(ap_app *app)
{
    if (!app->photo || !app->canvas) return;
    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;
    if (win_w <= 0 || win_h <= 0) return;

    float scale = ap_canvas_effective_scale(app->canvas, win_w, win_h);
    if (scale <= 0.0f) return;

    char label[32];
    float zoom = ap_canvas_zoom(app->canvas);
    if (zoom >= AP_CANVAS_DEFAULT_ZOOM - 0.001f &&
        zoom <= AP_CANVAS_DEFAULT_ZOOM + 0.001f) {
        snprintf(label, sizeof(label), "Fit");
    } else if (scale >= 0.995f && scale <= 1.005f) {
        snprintf(label, sizeof(label), "100%%");
    } else {
        snprintf(label, sizeof(label), "%d%%", (int)(scale * 100.0f + 0.5f));
    }

    ImDrawList *dl = igGetForegroundDrawList_ViewportPtr(NULL);
    if (!dl) return;

    ImVec2_c text_sz = igCalcTextSize(label, NULL, false, -1.0f);
    const float pad   = 8.0f;
    float x = (float)win_w - text_sz.x - pad;
    float y = (float)win_h - text_sz.y - pad;
    ImVec2_c bg_tl = { x - pad * 0.5f, y - pad * 0.5f };
    ImVec2_c bg_br = { x + text_sz.x + pad * 0.5f,
                       y + text_sz.y + pad * 0.5f };
    ImDrawList_AddRectFilled(dl, bg_tl, bg_br, 0xB8000000, 4.0f, 0);
    ImDrawList_AddText_Vec2(dl, (ImVec2_c){ x, y }, 0xFFEEEEEE, label, NULL);
}

// "ORIGINAL" badge drawn over the top-left corner of the canvas while
// the before/after compare bypass is active. Gives the user clear visual
// feedback that edits are bypassed.
static void draw_compare_overlay(ap_app *app)
{
    if (!app->compare_original || !app->photo) return;
    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;
    ImDrawList *dl = igGetForegroundDrawList_ViewportPtr(NULL);
    if (!dl) return;

    const char *label = "ORIGINAL";
    ImVec2_c text_sz = igCalcTextSize(label, NULL, false, -1.0f);
    const float pad = 8.0f;
    float x = pad;
    float y = pad + igGetFrameHeight();
    ImVec2_c bg_tl = { x - pad * 0.5f, y - pad * 0.5f };
    ImVec2_c bg_br = { x + text_sz.x + pad * 0.5f,
                       y + text_sz.y + pad * 0.5f };
    ImDrawList_AddRectFilled(dl, bg_tl, bg_br, 0xCC000000, 4.0f, 0);
    ImDrawList_AddText_Vec2(dl, (ImVec2_c){ x, y }, 0xFFEEEEEE, label, NULL);
}

// The crop overlay: dimmed margin outside the crop rect, a rule-of-
// thirds grid inside it, the rect outline, and eight grab handles.
// Drawn on the foreground draw list so it sits over the canvas but
// under ImGui windows. Only runs while the crop tool is armed; the
// canvas is showing the full frame (see the viewport push).
static void draw_crop_overlay(ap_app *app)
{
    if (app->canvas_tool != AP_CANVAS_TOOL_CROP) return;
    ap_edit_entry *e = canvas_tool_entry(app, "transform");
    if (!e) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;
    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;

    ImDrawList *dl = igGetForegroundDrawList_ViewportPtr(NULL);
    if (!dl) return;

    ap_viewport vp = ap_transform_viewport(e->params);

    float x0s, y0s, x1s, y1s;
    ap_canvas_framed_uv_to_screen(app->canvas, vp.crop_x0, vp.crop_y0,
                                  win_w, win_h, &x0s, &y0s);
    ap_canvas_framed_uv_to_screen(app->canvas, vp.crop_x1, vp.crop_y1,
                                  win_w, win_h, &x1s, &y1s);

    float fx0, fy0, fx1, fy1;
    ap_canvas_framed_uv_to_screen(app->canvas, 0.0f, 0.0f,
                                  win_w, win_h, &fx0, &fy0);
    ap_canvas_framed_uv_to_screen(app->canvas, 1.0f, 1.0f,
                                  win_w, win_h, &fx1, &fy1);

    const unsigned dim = 0x99000000u;
    ImDrawList_AddRectFilled(dl, (ImVec2_c){ fx0, fy0 },
                             (ImVec2_c){ fx1, y0s }, dim, 0.0f, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2_c){ fx0, y1s },
                             (ImVec2_c){ fx1, fy1 }, dim, 0.0f, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2_c){ fx0, y0s },
                             (ImVec2_c){ x0s, y1s }, dim, 0.0f, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2_c){ x1s, y0s },
                             (ImVec2_c){ fx1, y1s }, dim, 0.0f, 0);

    const unsigned grid_col = 0x66FFFFFFu;
    for (int i = 1; i <= 2; i++) {
        float t = (float)i / 3.0f;
        float gx = x0s + (x1s - x0s) * t;
        float gy = y0s + (y1s - y0s) * t;
        ImDrawList_AddLine(dl, (ImVec2_c){ gx, y0s },
                           (ImVec2_c){ gx, y1s }, grid_col, 1.0f);
        ImDrawList_AddLine(dl, (ImVec2_c){ x0s, gy },
                           (ImVec2_c){ x1s, gy }, grid_col, 1.0f);
    }

    ImDrawList_AddRect(dl, (ImVec2_c){ x0s, y0s }, (ImVec2_c){ x1s, y1s },
                       0xFFEEEEEE, 0.0f, 2.0f, 0);

    const float hs = 5.0f;
    for (int h = CROP_HANDLE_TL; h <= CROP_HANDLE_B; h++) {
        float hu, hv, sx, sy;
        crop_handle_uv(&vp, h, &hu, &hv);
        ap_canvas_framed_uv_to_screen(app->canvas, hu, hv,
                                      win_w, win_h, &sx, &sy);
        unsigned col = (h == app->crop_drag_handle) ? 0xFF55D6F2u
                                                    : 0xFFEEEEEEu;
        ImDrawList_AddRectFilled(dl, (ImVec2_c){ sx - hs, sy - hs },
                                 (ImVec2_c){ sx + hs, sy + hs }, col,
                                 0.0f, 0);
        ImDrawList_AddRect(dl, (ImVec2_c){ sx - hs, sy - hs },
                           (ImVec2_c){ sx + hs, sy + hs }, 0xFF101010u,
                           0.0f, 1.5f, 0);
    }

    if (app->crop_drag_handle == CROP_HANDLE_STRAIGHTEN) {
        float su0, sv0;
        ap_canvas_framed_uv_to_screen(app->canvas, app->crop_drag_u0,
                                      app->crop_drag_v0, win_w, win_h,
                                      &su0, &sv0);
        ImDrawList_AddLine(dl, (ImVec2_c){ su0, sv0 },
                           io->MousePos, 0xFF55D6F2u, 2.0f);
    }
}

// Aspect-ratio presets for the crop tool toolbar.
typedef struct {
    const char *label;
    float       w, h;
} crop_aspect_preset;

static const crop_aspect_preset crop_aspect_presets[] = {
    { "Free",  0.0f, 0.0f },
    { "1:1",   1.0f, 1.0f },
    { "3:2",   3.0f, 2.0f },
    { "4:3",   4.0f, 3.0f },
    { "16:9", 16.0f, 9.0f },
    { "2:3",   2.0f, 3.0f },
    { "3:4",   3.0f, 4.0f },
    { "9:16",  9.0f, 16.0f },
};

// Floating toolbar shown while the crop tool is armed: aspect-ratio
// lock, reset, and a Done button.
static void draw_crop_toolbar(ap_app *app)
{
    if (app->canvas_tool != AP_CANVAS_TOOL_CROP) return;
    ap_edit_entry *e = canvas_tool_entry(app, "transform");
    if (!e) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    igSetNextWindowBgAlpha(0.85f);
    igSetNextWindowPos((ImVec2_c){ io->DisplaySize.x * 0.5f, 36.0f },
                       ImGuiCond_Appearing, (ImVec2_c){ 0.5f, 0.0f });
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_AlwaysAutoResize
                           | ImGuiWindowFlags_NoDocking;
    if (!igBegin("Crop##overlay", NULL, flags)) {
        igEnd();
        return;
    }

    int n_presets = (int)(sizeof(crop_aspect_presets) /
                          sizeof(crop_aspect_presets[0]));

    igText("Aspect");
    igSameLine(0.0f, -1.0f);
    igSetNextItemWidth(110.0f);
    const char *cur = "Free";
    if (app->crop_aspect_locked) {
        for (int i = 1; i < n_presets; i++) {
            float r = crop_aspect_presets[i].w / crop_aspect_presets[i].h;
            if (fabsf(r - app->crop_aspect_ratio) < 0.001f) {
                cur = crop_aspect_presets[i].label;
                break;
            }
        }
    }
    if (igBeginCombo("##aspect", cur, 0)) {
        for (int i = 0; i < n_presets; i++) {
            const crop_aspect_preset *p = &crop_aspect_presets[i];
            bool selected = (i == 0) ? !app->crop_aspect_locked
                : (app->crop_aspect_locked &&
                   strcmp(cur, p->label) == 0);
            if (igSelectable_Bool(p->label, selected, 0,
                                  (ImVec2_c){ 0.0f, 0.0f })) {
                if (i == 0) {
                    app->crop_aspect_locked = false;
                } else {
                    app->crop_aspect_locked = true;
                    app->crop_aspect_ratio  = p->w / p->h;
                    ap_viewport vp = ap_transform_viewport(e->params);
                    int iw = ap_photo_width(app->photo);
                    int ih = ap_photo_height(app->photo);
                    if (iw > 0 && ih > 0) {
                        float ratio_norm = app->crop_aspect_ratio
                                         * (float)ih / (float)iw;
                        float cx = 0.5f * (vp.crop_x0 + vp.crop_x1);
                        float cy = 0.5f * (vp.crop_y0 + vp.crop_y1);
                        float cw = vp.crop_x1 - vp.crop_x0;
                        float ch = cw / ratio_norm;
                        if (ch > 1.0f) { ch = 1.0f; cw = ch * ratio_norm; }
                        ap_app_edit_snapshot(app);
                        vp.crop_x0 = cx - cw * 0.5f;
                        vp.crop_x1 = cx + cw * 0.5f;
                        vp.crop_y0 = cy - ch * 0.5f;
                        vp.crop_y1 = cy + ch * 0.5f;
                        crop_rect_sanitize(&vp);
                        ap_transform_set_viewport(e->params, &vp);
                    }
                }
            }
        }
        igEndCombo();
    }

    igSameLine(0.0f, 16.0f);
    if (igButton("Reset crop", (ImVec2_c){ 0.0f, 0.0f })) {
        ap_viewport vp = ap_transform_viewport(e->params);
        ap_app_edit_snapshot(app);
        vp.crop_x0 = 0.0f; vp.crop_y0 = 0.0f;
        vp.crop_x1 = 1.0f; vp.crop_y1 = 1.0f;
        vp.rotation_deg = 0.0f;
        ap_transform_set_viewport(e->params, &vp);
    }

    igSameLine(0.0f, 8.0f);
    if (igButton("Done", (ImVec2_c){ 0.0f, 0.0f })) {
        ap_app_set_canvas_tool(app, AP_CANVAS_TOOL_NONE, -1);
    }

    igSeparator();
    igTextDisabled("drag handles to crop  ·  shift-drag inside to straighten");

    igEnd();
}

static void draw_loading_overlay(ap_app *app)
{
    if (!app->photo_loading) return;
    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;
    ImDrawList *dl = igGetForegroundDrawList_ViewportPtr(NULL);
    if (!dl) return;

    ImVec2_c center = { io->DisplaySize.x * 0.5f, io->DisplaySize.y * 0.5f };
    char msg[5120];
    snprintf(msg, sizeof(msg), "loading %s", app->loading_path);
    ImVec2_c pos = { center.x - 200.0f, center.y - 8.0f };
    ImDrawList_AddText_Vec2(dl, pos, 0xFFEEEEEE, msg, NULL);
}

int ap_app_run_frame(ap_app *app)
{
    if (!app) return -1;

    ap_imgui_new_frame();

    draw_menubar(app);
    draw_import_modal(app);
    draw_rename_library_modal(app);
    draw_save_layout_modal(app);
    draw_delete_modal(app);
    draw_delete_edit_modal(app);
    drive_global_hotkeys(app);

    // Full-viewport invisible host window owns the dockspace that
    // every panel docks into. PassthruCentralNode keeps the middle
    // area transparent so the canvas / grid render path stays
    // visible underneath. Default layout (Image left, Edits + Tools
    // right) is built once on first launch; ImGui's .ini handles
    // every subsequent run.
    ImGuiDockNode *dock_central = NULL;
    if (app->show_panels) {
        ImGuiViewport *vp = igGetMainViewport();
        igSetNextWindowPos(vp->WorkPos, ImGuiCond_Always,
                           (ImVec2_c){ 0.0f, 0.0f });
        igSetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
        igSetNextWindowViewport(vp->ID);

        igPushStyleVar_Float(ImGuiStyleVar_WindowRounding,   0.0f);
        igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 0.0f);
        igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding,
                            (ImVec2_c){ 0.0f, 0.0f });

        ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoTitleBar
                                    | ImGuiWindowFlags_NoCollapse
                                    | ImGuiWindowFlags_NoResize
                                    | ImGuiWindowFlags_NoMove
                                    | ImGuiWindowFlags_NoBringToFrontOnFocus
                                    | ImGuiWindowFlags_NoNavFocus
                                    | ImGuiWindowFlags_NoBackground
                                    | ImGuiWindowFlags_NoDocking;

        igBegin("##aperture_dockhost", NULL, host_flags);
        igPopStyleVar(3);

        ImGuiID dockspace_id = igGetID_Str("aperture_dockspace");
        static bool dock_layout_built = false;
        // Reset-to-defaults trips the rebuild flag in layout_profiles;
        // pull it down here so the builder runs again on the next
        // frame and produces the default layout from scratch.
        if (ap_layout_consume_rebuild_request()) {
            if (igDockBuilderGetNode(dockspace_id)) {
                igDockBuilderRemoveNode(dockspace_id);
            }
            dock_layout_built = false;
        }
        if (!dock_layout_built &&
            igDockBuilderGetNode(dockspace_id) == NULL) {
            dock_layout_built = true;
            igDockBuilderAddNode(dockspace_id,
                                 ImGuiDockNodeFlags_DockSpace);
            igDockBuilderSetNodeSize(dockspace_id, vp->WorkSize);

            // Default layout. The principle: left column is "what
            // this is" (per-photo info / library navigation), right
            // column is "what to do with it" (active controls). Both
            // generalize across modes — photo-mode panels populate
            // their slots when photo is open, library-mode panels
            // populate the same column shapes when in library mode.
            // ImGui's .ini owns everything past first launch, so the
            // user can rearrange, float, or close panels freely;
            // empty dock nodes collapse and the central node grows.
            ImGuiID center = 0;
            ImGuiID left   = igDockBuilderSplitNode(dockspace_id,
                                                    ImGuiDir_Left, 0.18f,
                                                    NULL, &center);
            ImGuiID right  = igDockBuilderSplitNode(center,
                                                    ImGuiDir_Right, 0.27f,
                                                    NULL, &center);

            ImGuiID left_bot = 0;
            ImGuiID left_top = igDockBuilderSplitNode(left,
                                                      ImGuiDir_Up, 0.50f,
                                                      NULL, &left_bot);

            ImGuiID right_bot   = 0;
            ImGuiID right_upper = igDockBuilderSplitNode(right,
                                                          ImGuiDir_Up, 0.62f,
                                                          NULL, &right_bot);
            ImGuiID right_mid = 0;
            ImGuiID right_top = igDockBuilderSplitNode(right_upper,
                                                       ImGuiDir_Up, 0.45f,
                                                       NULL, &right_mid);

            igDockBuilderDockWindow("Image",             left_top);
            igDockBuilderDockWindow("Metadata",          left_bot);
            igDockBuilderDockWindow("Histogram",         right_top);
            igDockBuilderDockWindow("Tools",             right_mid);
            igDockBuilderDockWindow("Edits",             right_bot);
            igDockBuilderDockWindow("Metadata##library",      right_bot);
            igDockBuilderDockWindow("Pipelines##library",     right_bot);
            igDockBuilderDockWindow("Groups##library",        right_bot);
            igDockBuilderDockWindow("Sort & Search##library", right_bot);
            igDockBuilderDockWindow("Format##export",      right_top);
            igDockBuilderDockWindow("Quality##export",     right_mid);
            igDockBuilderDockWindow("Naming##export",      right_mid);
            igDockBuilderDockWindow("Destination##export", right_bot);
            igDockBuilderFinish(dockspace_id);
        }
        igDockSpace(dockspace_id,
                    (ImVec2_c){ 0.0f, 0.0f },
                    ImGuiDockNodeFlags_PassthruCentralNode, NULL);
        dock_central = igDockBuilderGetCentralNode(dockspace_id);
        igEnd();
    }

    // Confine the grid and the canvas to the dockspace central node so
    // docked panels don't paint over them — and so the canvas fits the
    // visible area, not the whole window. dock_central was captured
    // inside the dock-host window (correct ID scope) and stays NULL
    // when show_panels is off — both cases fall through to full-window.
    {
        bool have_central = dock_central &&
            dock_central->Size.x > 0.0f && dock_central->Size.y > 0.0f;
        int cx = have_central ? (int)dock_central->Pos.x  : 0;
        int cy = have_central ? (int)dock_central->Pos.y  : 0;
        int cw = have_central ? (int)dock_central->Size.x : 0;
        int ch = have_central ? (int)dock_central->Size.y : 0;
        if (app->grid)   ap_grid_set_render_rect(app->grid, cx, cy, cw, ch);
        if (app->canvas) ap_canvas_set_render_rect(app->canvas, cx, cy, cw, ch);
    }

    if (app->show_panels) {
        for (const ap_panel *const *p = ap_panel_registry; *p; p++) {
            const ap_panel *panel = *p;
            if (panel->mode != AP_MODE_ANY && panel->mode != app->mode) continue;
            if (panel->visible && !*panel->visible) continue;
            if (panel->draw) {
                panel->draw(app);
            }
        }
    }

    if (app->grid) {
        ImGuiIO *io = igGetIO_Nil();
        ap_grid_update(app->grid, io ? io->DeltaTime : 0.0f);
    }

    // Push the active viewport to the canvas before the mode-input
    // handlers run — the pipeline renders full-frame, the canvas
    // applies the Transform module's crop / rotation / flip / scale at
    // presentation. The panels above may have changed it this frame,
    // and the crop tool's coordinate mapping (drive_crop_tool) reads
    // the canvas viewport, so it must be current here, not pushed at
    // end of frame.
    //
    // While the interactive crop tool is armed the canvas shows the
    // *full* rotated frame (crop reset to the whole frame) so the user
    // can drag the crop rect over the entire image; the crop overlay
    // draws the rect on top. Rotation / flip / scale stay applied so
    // straighten previews live.
    if (app->canvas) {
        if (app->photo) {
            ap_viewport vp = ap_photo_viewport(app->photo);
            if (app->canvas_tool == AP_CANVAS_TOOL_CROP) {
                vp.crop_x0 = 0.0f; vp.crop_y0 = 0.0f;
                vp.crop_x1 = 1.0f; vp.crop_y1 = 1.0f;
            }
            ap_canvas_set_viewport(app->canvas, &vp);
        } else {
            ap_canvas_set_viewport(app->canvas, NULL);
        }
    }

    if (app->mode == AP_MODE_PHOTO && !app->photo_loading) {
        drive_canvas_input(app);
        draw_crop_overlay(app);
        draw_crop_toolbar(app);
        draw_canvas_zoom_overlay(app);
        draw_compare_overlay(app);
    } else if (app->mode == AP_MODE_EXPORT && !app->photo_loading) {
        drive_export_input(app);
        draw_canvas_zoom_overlay(app);
    } else if (app->mode == AP_MODE_LIBRARY && !app->photo_loading) {
        drive_grid_input(app);
        draw_grid_context_menu(app);
        draw_grid_labels(app);
        draw_selection_overlay(app);
        draw_marquee_overlay(app);
        submit_pending_thumbs(app);
    }
    drain_one_completed_job(app);
    draw_loading_overlay(app);
    ap_toast_draw();

    const ap_edit_stack *stack = NULL;
    if (app->photo) {
        stack = ap_photo_stack(app->photo);
    }
    return ap_gpu_render_frame(app->gpu, stack);
}
