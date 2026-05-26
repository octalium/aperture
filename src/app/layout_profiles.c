#define _GNU_SOURCE

#include "layout_profiles.h"

#include "app/root.h"
#include "core/log.h"
#include "library/library.h"
#include "ui/imgui.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// named layout snapshots live under the data root, not the config root.
// the working layout (imgui.ini) is transient session state — it lives
// in $XDG_CONFIG_HOME. these files are explicit user-named artifacts
// (saved presets) and belong with the rest of aperture's user data.
#define LAYOUTS_DIR  "layouts"
#define CURRENT_FILE ".current"
#define LAYOUT_EXT   ".ini"

// Increment this when a new panel is added to the default dock layout
// in ap_app_run_frame. ap_layout_init compares the persisted value; when
// it is older, the adoption pass runs once to place any undocked panels
// into the existing layout without nuking the user's overall arrangement.
// v3: export mode removed; Format##export, Quality##export, Naming##export,
//     Destination##export panels dropped from the dockspace default layout.
#define LAYOUT_SCHEMA_VERSION 3

// Module-local state. The "active name" buffer + the rebuild/adoption flags.
static char g_active_name[AP_LAYOUT_NAME_LEN] = {0};
static bool g_rebuild_pending                 = false;
static bool g_panel_adoption_pending          = false;

static int layouts_dir_path(char *out, size_t out_len)
{
    if (ap_app_root_ensure() < 0) return -1;
    return ap_app_root_join(LAYOUTS_DIR, out, out_len);
}

static int layouts_dir_ensure(void)
{
    char dir[4096];
    if (layouts_dir_path(dir, sizeof(dir)) < 0) return -1;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        AP_ERROR("layouts: mkdir(%s): %s", dir, strerror(errno));
        return -1;
    }
    return 0;
}

static int profile_path(const char *name, char *out, size_t out_len)
{
    char dir[4096];
    if (layouts_dir_path(dir, sizeof(dir)) < 0) return -1;
    int n = snprintf(out, out_len, "%s/%s%s", dir, name, LAYOUT_EXT);
    return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
}

static int current_pointer_path(char *out, size_t out_len)
{
    char dir[4096];
    if (layouts_dir_path(dir, sizeof(dir)) < 0) return -1;
    int n = snprintf(out, out_len, "%s/%s", dir, CURRENT_FILE);
    return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
}

// Read a single-line text file into `out`. Strips trailing newline /
// whitespace. Returns 0 on success.
static int read_pointer(char *out, size_t out_len)
{
    char path[4096];
    if (current_pointer_path(path, sizeof(path)) < 0) return -1;
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno != ENOENT) AP_WARN("layouts: open .current: %s", strerror(errno));
        out[0] = '\0';
        return -1;
    }
    if (!fgets(out, (int)out_len, f)) {
        fclose(f);
        out[0] = '\0';
        return -1;
    }
    fclose(f);
    // Trim trailing whitespace / newline.
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' ||
                       out[len - 1] == ' '  || out[len - 1] == '\t')) {
        out[--len] = '\0';
    }
    return 0;
}

static int write_pointer(const char *name)
{
    char path[4096];
    if (current_pointer_path(path, sizeof(path)) < 0) return -1;
    if (!name || !*name) {
        // Empty name → clear pointer (delete the file).
        if (unlink(path) != 0 && errno != ENOENT) {
            AP_WARN("layouts: unlink .current: %s", strerror(errno));
        }
        return 0;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        AP_ERROR("layouts: open .current for write: %s", strerror(errno));
        return -1;
    }
    fprintf(f, "%s\n", name);
    fclose(f);
    return 0;
}

int ap_layout_init(void)
{
    if (layouts_dir_ensure() < 0) return -1;

    // The working layout is auto-persisted by ImGui itself
    // (io.IniFilename -> <app_config>/imgui.ini) and loads on the first
    // frame. ap_layout_init only records which named profile was last
    // made active, for the View > Layout menu — it does not force a
    // profile load at startup.
    char name[AP_LAYOUT_NAME_LEN];
    if (read_pointer(name, sizeof(name)) == 0 && *name) {
        snprintf(g_active_name, sizeof(g_active_name), "%s", name);
    } else {
        g_active_name[0] = '\0';
    }

    // Check whether new panels have been added since the last run. The
    // schema version is persisted in app settings; when it is absent or
    // older than LAYOUT_SCHEMA_VERSION, the dockspace runner schedules a
    // one-shot adoption pass that places any undocked panels into the
    // existing layout without disturbing the user's overall arrangement.
    char ver_buf[16];
    int persisted = 0;
    if (ap_settings_get("layout.schema_version", ver_buf, sizeof(ver_buf)) == 0) {
        persisted = atoi(ver_buf);
    }
    if (persisted < LAYOUT_SCHEMA_VERSION) {
        g_panel_adoption_pending = true;
        snprintf(ver_buf, sizeof(ver_buf), "%d", LAYOUT_SCHEMA_VERSION);
        ap_settings_set("layout.schema_version", ver_buf);
        AP_INFO("layout: schema v%d -> v%d, scheduling panel adoption pass",
                persisted, LAYOUT_SCHEMA_VERSION);
    }

    return 0;
}

int ap_layout_list(char names[][AP_LAYOUT_NAME_LEN], int max)
{
    if (!names || max <= 0) return 0;
    char dir[4096];
    if (layouts_dir_path(dir, sizeof(dir)) < 0) return -1;

    DIR *d = opendir(dir);
    if (!d) {
        if (errno == ENOENT) return 0;
        AP_WARN("layouts: opendir(%s): %s", dir, strerror(errno));
        return -1;
    }
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;             // skip hidden + .current
        size_t len = strlen(e->d_name);
        size_t ext = strlen(LAYOUT_EXT);
        if (len <= ext) continue;
        if (strcmp(e->d_name + len - ext, LAYOUT_EXT) != 0) continue;
        size_t name_len = len - ext;
        if (name_len + 1 >= AP_LAYOUT_NAME_LEN) continue;
        memcpy(names[n], e->d_name, name_len);
        names[n][name_len] = '\0';
        n++;
    }
    closedir(d);
    return n;
}

const char *ap_layout_active_name(void)
{
    return g_active_name;
}

int ap_layout_set_active(const char *name)
{
    if (!name || !*name) return -1;
    char path[4096];
    if (profile_path(name, path, sizeof(path)) < 0) return -1;
    struct stat st;
    if (stat(path, &st) != 0) {
        AP_WARN("layouts: profile '%s' not on disk", name);
        return -1;
    }
    ap_imgui_clear_settings();
    ap_imgui_load_layout(path);
    snprintf(g_active_name, sizeof(g_active_name), "%s", name);
    write_pointer(name);
    return 0;
}

int ap_layout_save_current_as(const char *name)
{
    if (!name || !*name) return -1;
    if (layouts_dir_ensure() < 0) return -1;
    char path[4096];
    if (profile_path(name, path, sizeof(path)) < 0) return -1;
    ap_imgui_save_layout(path);
    snprintf(g_active_name, sizeof(g_active_name), "%s", name);
    write_pointer(name);
    return 0;
}

int ap_layout_reload_active(void)
{
    if (!g_active_name[0]) return 0;
    char path[4096];
    if (profile_path(g_active_name, path, sizeof(path)) < 0) return -1;
    ap_imgui_clear_settings();
    ap_imgui_load_layout(path);
    return 0;
}

void ap_layout_reset_to_default(void)
{
    ap_imgui_clear_settings();
    g_active_name[0]  = '\0';
    g_rebuild_pending = true;
    write_pointer(NULL);
}

bool ap_layout_consume_rebuild_request(void)
{
    bool r = g_rebuild_pending;
    g_rebuild_pending = false;
    return r;
}

bool ap_layout_consume_panel_adoption_request(void)
{
    bool r = g_panel_adoption_pending;
    g_panel_adoption_pending = false;
    return r;
}
