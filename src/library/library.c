#define _GNU_SOURCE

#include "library.h"

#include "app/root.h"
#include "core/log.h"
#include "edit/stack.h"
#include "edit/stack_toml.h"
#include "io/raw.h"
#include "modules/module.h"
#include "output/export.h"
#include "photo/thumbnail.h"
#include "sidecar/sidecar.h"

#include <sqlite3.h>
#include <toml.h>

#include "core/compat.h"
#include "core/dir.h"
#include "core/fs.h"
#include "core/memstream.h"
#include "core/random.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#define APERTURE_DB_VERSION "1"

// In-memory cap on the per-library group registry.
#define AP_LIBRARY_GROUPS_MAX 256

#ifndef APERTURE_VERSION
#error "APERTURE_VERSION must be defined at compile time (set via meson)"
#endif

// The rebuildable slice of a library: everything derived from the db +
// sidecars that a sort / rescan / delete replaces wholesale. Split out
// so a replacement can be built off the main thread (its own sqlite
// connection + sidecar reads) and swapped in atomically between frames
// — see ap_library_cache_build / ap_library_cache_swap. The GPU-owned
// thumbnail arrays stay on ap_library: they are main-thread/GPU state,
// resized to match this cache's photo_count at swap time.
struct ap_library_cache {
    char    **photo_paths; // relative to root
    int       photo_count;
    int       photo_capacity;

    // photo_count entries, built from the sidecars.
    ap_photo_groups *photo_groups;

    // photo_count entries, mirroring the rating / flag / color columns
    // on the photos table. Seeded from the db, reconciled against the
    // sidecars (the source of truth).
    ap_photo_culling *photo_culling;

    // Group registry — the names of groups that exist, independent of
    // membership. Loaded from the `groups` table.
    char group_names[AP_LIBRARY_GROUPS_MAX][AP_GROUP_NAME_LEN];
    int  group_count;
};

struct ap_library {
    char     *root;        // absolute path to the photo directory
    char      id[37];      // RFC 4122 v4 UUID, 36 chars + NUL
    char      name[128];   // user-set display name; empty if unset
    sqlite3  *db;          // <library_root>/library.db

    // The rebuildable cache (photo list / groups / culling). Swapped
    // wholesale by ap_library_cache_swap; never NULL after open.
    ap_library_cache *cache;

    ap_thumbnail **thumbs;        // cache->photo_count entries, NULL = not loaded
    bool          *thumb_failed;  // cache->photo_count entries, true = decode failed
    int            thumb_cursor;  // next idx to try; rolled forward
};

// registry: <app_root>/aperture.db, libraries(id, path, created_at)
static const char *REGISTRY_SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS libraries ("
    "    id         TEXT PRIMARY KEY,"
    "    path       TEXT NOT NULL UNIQUE,"
    "    name       TEXT,"
    "    created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS pipelines ("
    "    id         INTEGER PRIMARY KEY,"
    "    name       TEXT NOT NULL UNIQUE,"
    "    definition TEXT NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS settings ("
    "    key   TEXT PRIMARY KEY,"
    "    value TEXT NOT NULL"
    ");";

// Backfill the `name` column on registry dbs created before it was
// added. ALTER returns SQLITE_ERROR with "duplicate column name" when
// it already exists - we treat that as success.
static void backfill_name_column(sqlite3 *reg)
{
    char *err = NULL;
    int rc = sqlite3_exec(reg,
        "ALTER TABLE libraries ADD COLUMN name TEXT;", NULL, NULL, &err);
    if (rc != SQLITE_OK && err && !strstr(err, "duplicate column")) {
        AP_WARN("registry: backfill name column: %s", err);
    }
    sqlite3_free(err);
}

// Baseline edits for a fresh photo. Output Transfer is auto-appended
// by the pipeline graph; everything else is on the user-facing stack.
// The pipeline `definition` column stores these (and any user-defined
// pipelines) as the same `[[edit]]` TOML the sidecar uses.
static const char *DEFAULT_PIPELINE_NAME = "default";
// Order: demosaic decodes the sensor; wb / profile bring the data
// into the working colour space; lens_correction undoes the optical
// distortion + vignetting reported by Lensfun. Everything past this
// list is creative and belongs to the user.
static const char *DEFAULT_PIPELINE_MODULES[] = {
    "demosaic", "wb", "profile", "lens_correction", NULL,
};

static int gen_uuid_v4(char buf[37])
{
    uint8_t b[16];
    if (ap_random_bytes(b, sizeof(b)) != 0) {
        AP_ERROR("gen_uuid_v4: entropy source unavailable");
        return -1;
    }

    // RFC 4122 v4: version in bits 12-15 of time_hi (b[6]),
    //              variant in bits 6-7 of clock_seq_hi (b[8]).
    b[6] = (b[6] & 0x0Fu) | 0x40u;
    b[8] = (b[8] & 0x3Fu) | 0x80u;

    snprintf(buf, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2],  b[3],  b[4],  b[5],  b[6],  b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return 0;
}

// Build a freshly-initialized stack containing the baseline modules.
// Each entry takes the module's default params via ap_edit_stack_add.
static int build_default_stack(ap_edit_stack *out)
{
    ap_edit_stack_init(out);
    for (int i = 0; DEFAULT_PIPELINE_MODULES[i]; i++) {
        if (ap_edit_stack_add(out, DEFAULT_PIPELINE_MODULES[i]) < 0) {
            AP_ERROR("registry: cannot add default module '%s' to stack",
                     DEFAULT_PIPELINE_MODULES[i]);
            return -1;
        }
    }
    return 0;
}

// Render a stack to a TOML `[[edit]]`-array string via open_memstream.
// Caller owns *out_buf; *out_len excludes the terminating NUL.
static int stack_to_toml_string(const ap_edit_stack *stack,
                                char **out_buf, size_t *out_len)
{
    *out_buf = NULL;
    *out_len = 0;
    ap_memstream *ms = ap_memstream_open();
    if (!ms) {
        AP_ERROR("pipeline: open_memstream: %s", strerror(errno));
        return -1;
    }
    if (ap_edit_stack_write_toml(stack, ap_memstream_file(ms)) != 0) {
        char *scrap = NULL;
        size_t scrap_len = 0;
        ap_memstream_close(ms, &scrap, &scrap_len);
        free(scrap);
        return -1;
    }
    char  *buf = NULL;
    size_t len = 0;
    if (ap_memstream_close(ms, &buf, &len) != 0) {
        free(buf);
        AP_ERROR("pipeline: fclose memstream: %s", strerror(errno));
        return -1;
    }
    *out_buf = buf;
    *out_len = len;
    return 0;
}

// Reverse direction: parse a TOML definition blob into a stack.
// Empty or NULL produces an empty stack. Returns 0 on success.
static int stack_from_toml_string(const char *toml_str, ap_edit_stack *out)
{
    if (!out) return -1;
    ap_edit_stack_init(out);
    if (!toml_str || !*toml_str) return 0;
    // toml_parse() mutates its input; copy first.
    size_t n   = strlen(toml_str);
    char  *buf = malloc(n + 1);
    if (!buf) return -1;
    memcpy(buf, toml_str, n + 1);
    char errbuf[256] = {0};
    toml_table_t *root = toml_parse(buf, errbuf, sizeof(errbuf));
    free(buf);
    if (!root) {
        AP_WARN("pipeline: parse definition: %s", errbuf);
        return -1;
    }
    toml_array_t *arr = toml_array_in(root, "edit");
    int rc = ap_edit_stack_read_toml_array(arr, out);
    toml_free(root);
    return rc;
}

// Pre-existing dbs may have a pipelines table with the old
// (modules TEXT) shape. We own this schema end-to-end and the only
// data in the table is the default seed, so the cheapest "migration"
// is to drop the old table and let CREATE TABLE IF NOT EXISTS rebuild
// on the new shape. Detection: probe pragma_table_info for the
// `definition` column.
static int migrate_pipelines_shape(sqlite3 *reg)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reg,
        "SELECT COUNT(*) FROM pragma_table_info('pipelines') "
        "WHERE name = 'definition';",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        // Table absent altogether (fresh db) — fine, CREATE handles it.
        return 0;
    }
    int has_def = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        has_def = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    if (has_def) return 0;

    char *err = NULL;
    rc = sqlite3_exec(reg, "DROP TABLE IF EXISTS pipelines;", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        AP_WARN("registry: drop legacy pipelines table: %s",
                err ? err : "(no message)");
    }
    sqlite3_free(err);
    return 0;
}

static int seed_default_pipeline(sqlite3 *reg)
{
    // Always overwrite the default row: the default reflects the
    // current build's baseline edits, not user state.
    ap_edit_stack stack;
    if (build_default_stack(&stack) != 0) return -1;

    char  *toml_buf = NULL;
    size_t toml_len = 0;
    if (stack_to_toml_string(&stack, &toml_buf, &toml_len) != 0) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(reg,
            "INSERT INTO pipelines(name, definition) VALUES (?, ?) "
            "ON CONFLICT(name) DO UPDATE SET definition = excluded.definition;",
            -1, &stmt, NULL) != SQLITE_OK) {
        AP_ERROR("registry: prepare seed: %s", sqlite3_errmsg(reg));
        free(toml_buf);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, DEFAULT_PIPELINE_NAME, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, toml_buf, (int)toml_len, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(toml_buf);
    if (rc != SQLITE_DONE) {
        AP_ERROR("registry: seed default pipeline: %s", sqlite3_errstr(rc));
        return -1;
    }
    return 0;
}

static int registry_open(sqlite3 **out_db)
{
    if (ap_app_root_ensure() < 0) return -1;

    char reg_path[4096];
    if (ap_app_root_join("aperture.db", reg_path, sizeof(reg_path)) < 0) {
        return -1;
    }

    sqlite3 *reg = NULL;
    if (sqlite3_open(reg_path, &reg) != SQLITE_OK) {
        AP_ERROR("registry: sqlite3_open(%s): %s", reg_path, sqlite3_errmsg(reg));
        if (reg) sqlite3_close(reg);
        return -1;
    }
    sqlite3_busy_timeout(reg, 5000);

    // Drop the legacy `pipelines.modules` shape *before* re-running
    // CREATE TABLE IF NOT EXISTS so the new schema can build cleanly.
    migrate_pipelines_shape(reg);

    char *err = NULL;
    if (sqlite3_exec(reg, REGISTRY_SCHEMA_SQL, NULL, NULL, &err) != SQLITE_OK) {
        AP_ERROR("registry: schema: %s", err ? err : "(no message)");
        sqlite3_free(err);
        sqlite3_close(reg);
        return -1;
    }
    backfill_name_column(reg);

    if (seed_default_pipeline(reg) < 0) {
        sqlite3_close(reg);
        return -1;
    }

    *out_db = reg;
    return 0;
}

static int registry_resolve_id(const char *abs_path, char id_out[37])
{
    sqlite3 *reg = NULL;
    if (registry_open(&reg) < 0) return -1;

    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(reg, "SELECT id FROM libraries WHERE path = ?;",
                           -1, &sel, NULL) != SQLITE_OK) {
        AP_ERROR("registry: prepare select: %s", sqlite3_errmsg(reg));
        sqlite3_close(reg);
        return -1;
    }
    sqlite3_bind_text(sel, 1, abs_path, -1, SQLITE_STATIC);

    int rc = sqlite3_step(sel);
    if (rc == SQLITE_ROW) {
        const char *existing = (const char *)sqlite3_column_text(sel, 0);
        snprintf(id_out, 37, "%s", existing ? existing : "");
        sqlite3_finalize(sel);
        sqlite3_close(reg);
        return id_out[0] ? 0 : -1;
    }
    sqlite3_finalize(sel);
    if (rc != SQLITE_DONE) {
        AP_ERROR("registry: select step: %s", sqlite3_errstr(rc));
        sqlite3_close(reg);
        return -1;
    }

    if (gen_uuid_v4(id_out) < 0) {
        sqlite3_close(reg);
        return -1;
    }

    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(reg,
            "INSERT INTO libraries(id, path, created_at) VALUES (?, ?, ?);",
            -1, &ins, NULL) != SQLITE_OK) {
        AP_ERROR("registry: prepare insert: %s", sqlite3_errmsg(reg));
        sqlite3_close(reg);
        return -1;
    }
    sqlite3_bind_text(ins, 1, id_out,   -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, abs_path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 3, (int64_t)time(NULL));
    rc = sqlite3_step(ins);
    sqlite3_finalize(ins);
    sqlite3_close(reg);
    if (rc != SQLITE_DONE) {
        AP_ERROR("registry: insert step: %s", sqlite3_errstr(rc));
        return -1;
    }
    AP_INFO("library: registered new id %s for %s", id_out, abs_path);
    return 0;
}

// Materialize one SELECT row (id, name, definition) into a def.
static int row_to_def(sqlite3_stmt *stmt, ap_pipeline_def *out)
{
    memset(out, 0, sizeof(*out));
    out->id = sqlite3_column_int64(stmt, 0);
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    const char *def  = (const char *)sqlite3_column_text(stmt, 2);
    snprintf(out->name, sizeof(out->name), "%s", name ? name : "");
    return stack_from_toml_string(def, &out->stack);
}

int ap_pipeline_get_default(ap_pipeline_def *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    sqlite3 *reg = NULL;
    if (registry_open(&reg) < 0) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(reg,
            "SELECT id, name, definition FROM pipelines WHERE name = ?;",
            -1, &stmt, NULL) != SQLITE_OK) {
        AP_ERROR("pipeline: prepare select default: %s", sqlite3_errmsg(reg));
        sqlite3_close(reg);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, DEFAULT_PIPELINE_NAME, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        AP_ERROR("pipeline: default missing after seed (%s)", sqlite3_errstr(rc));
        sqlite3_finalize(stmt);
        sqlite3_close(reg);
        return -1;
    }
    int load_rc = row_to_def(stmt, out);
    sqlite3_finalize(stmt);
    sqlite3_close(reg);
    return load_rc;
}

int ap_pipeline_get(int64_t id, ap_pipeline_def *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    sqlite3 *reg = NULL;
    if (registry_open(&reg) < 0) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(reg,
            "SELECT id, name, definition FROM pipelines WHERE id = ?;",
            -1, &stmt, NULL) != SQLITE_OK) {
        AP_ERROR("pipeline: prepare select id: %s", sqlite3_errmsg(reg));
        sqlite3_close(reg);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, id);

    int rc = sqlite3_step(stmt);
    int load_rc = -1;
    if (rc == SQLITE_ROW) {
        load_rc = row_to_def(stmt, out);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(reg);
    return load_rc;
}

int ap_pipeline_get_by_name(const char *name, ap_pipeline_def *out)
{
    if (!name || !*name || !out) return -1;
    memset(out, 0, sizeof(*out));

    sqlite3 *reg = NULL;
    if (registry_open(&reg) < 0) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(reg,
            "SELECT id, name, definition FROM pipelines WHERE name = ?;",
            -1, &stmt, NULL) != SQLITE_OK) {
        AP_ERROR("pipeline: prepare select name: %s", sqlite3_errmsg(reg));
        sqlite3_close(reg);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);

    int load_rc = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        load_rc = row_to_def(stmt, out);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(reg);
    return load_rc;
}

int ap_pipeline_list(ap_pipeline_def *out, int max)
{
    if (!out || max <= 0) return 0;

    sqlite3 *reg = NULL;
    if (registry_open(&reg) < 0) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(reg,
            "SELECT id, name, definition FROM pipelines ORDER BY name COLLATE NOCASE;",
            -1, &stmt, NULL) != SQLITE_OK) {
        AP_ERROR("pipeline: prepare list: %s", sqlite3_errmsg(reg));
        sqlite3_close(reg);
        return -1;
    }
    int n = 0;
    while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
        if (row_to_def(stmt, &out[n]) == 0) n++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(reg);
    return n;
}

int ap_pipeline_create(const char *name, const ap_edit_stack *stack,
                       int64_t *out_id)
{
    if (!name || !*name || !stack) return -1;

    char  *toml_buf = NULL;
    size_t toml_len = 0;
    if (stack_to_toml_string(stack, &toml_buf, &toml_len) != 0) return -1;

    sqlite3 *reg = NULL;
    if (registry_open(&reg) < 0) { free(toml_buf); return -1; }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(reg,
            "INSERT INTO pipelines(name, definition) VALUES (?, ?);",
            -1, &stmt, NULL) != SQLITE_OK) {
        AP_ERROR("pipeline: prepare insert: %s", sqlite3_errmsg(reg));
        sqlite3_close(reg);
        free(toml_buf);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, name,     -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, toml_buf, (int)toml_len, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        AP_ERROR("pipeline: insert step: %s", sqlite3_errstr(rc));
        sqlite3_close(reg);
        free(toml_buf);
        return -1;
    }
    if (out_id) *out_id = sqlite3_last_insert_rowid(reg);
    sqlite3_close(reg);
    free(toml_buf);
    return 0;
}

int ap_pipeline_update(int64_t id, const char *name,
                       const ap_edit_stack *stack)
{
    if (id <= 0) return -1;
    if (!name && !stack) return 0;  // nothing to change

    sqlite3 *reg = NULL;
    if (registry_open(&reg) < 0) return -1;

    // Refuse to mutate the seeded default pipeline. The default
    // reflects the current build's baseline and gets re-seeded on
    // every library open; letting the UI overwrite or rename it would
    // either lose the user's changes on the next open or strand the
    // library without a sensible default. Users wanting a custom
    // baseline duplicate the default and set the copy as the library
    // default via the pipelines panel.
    sqlite3_stmt *guard = NULL;
    if (sqlite3_prepare_v2(reg,
            "SELECT 1 FROM pipelines WHERE id = ? AND name = ?;",
            -1, &guard, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(guard, 1, id);
        sqlite3_bind_text(guard, 2, DEFAULT_PIPELINE_NAME, -1, SQLITE_STATIC);
        bool is_default = (sqlite3_step(guard) == SQLITE_ROW);
        sqlite3_finalize(guard);
        if (is_default) {
            AP_WARN("pipeline: refusing to modify the default pipeline");
            sqlite3_close(reg);
            return -1;
        }
    }

    char  *toml_buf = NULL;
    size_t toml_len = 0;
    if (stack) {
        if (stack_to_toml_string(stack, &toml_buf, &toml_len) != 0) {
            sqlite3_close(reg);
            return -1;
        }
    }

    // Build the right statement based on which fields are present.
    const char *sql = NULL;
    if (name && stack) {
        sql = "UPDATE pipelines SET name = ?, definition = ? WHERE id = ?;";
    } else if (name) {
        sql = "UPDATE pipelines SET name = ? WHERE id = ?;";
    } else {
        sql = "UPDATE pipelines SET definition = ? WHERE id = ?;";
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(reg, sql, -1, &stmt, NULL) != SQLITE_OK) {
        AP_ERROR("pipeline: prepare update: %s", sqlite3_errmsg(reg));
        sqlite3_close(reg);
        free(toml_buf);
        return -1;
    }
    int col = 1;
    if (name) sqlite3_bind_text(stmt, col++, name, -1, SQLITE_STATIC);
    if (stack) sqlite3_bind_text(stmt, col++, toml_buf, (int)toml_len, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, col, id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(reg);
    free(toml_buf);
    if (rc != SQLITE_DONE) {
        AP_ERROR("pipeline: update step: %s", sqlite3_errstr(rc));
        return -1;
    }
    return 0;
}

int ap_pipeline_delete(int64_t id)
{
    if (id <= 0) return -1;

    sqlite3 *reg = NULL;
    if (registry_open(&reg) < 0) return -1;

    // The default pipeline is protected: it's the user-invisible
    // seed of every fresh photo's stack.
    sqlite3_stmt *guard = NULL;
    if (sqlite3_prepare_v2(reg,
            "SELECT 1 FROM pipelines WHERE id = ? AND name = ?;",
            -1, &guard, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(guard, 1, id);
        sqlite3_bind_text(guard, 2, DEFAULT_PIPELINE_NAME, -1, SQLITE_STATIC);
        bool is_default = (sqlite3_step(guard) == SQLITE_ROW);
        sqlite3_finalize(guard);
        if (is_default) {
            AP_WARN("pipeline: refusing to delete the default pipeline");
            sqlite3_close(reg);
            return -1;
        }
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(reg,
            "DELETE FROM pipelines WHERE id = ?;",
            -1, &stmt, NULL) != SQLITE_OK) {
        AP_ERROR("pipeline: prepare delete: %s", sqlite3_errmsg(reg));
        sqlite3_close(reg);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(reg);
    if (rc != SQLITE_DONE) {
        AP_ERROR("pipeline: delete step: %s", sqlite3_errstr(rc));
        return -1;
    }
    return 0;
}

// Replace the contents of `out` with the pipeline's stack, filtering
// to user-visible modules (transport modules like demosaic are graph-
// managed) and preserving params + display_name + enabled.
static void copy_pipeline_to_stack(const ap_pipeline_def *def,
                                   ap_edit_stack *out)
{
    ap_edit_stack_init(out);
    int count = ap_edit_stack_count(&def->stack);
    for (int i = 0; i < count; i++) {
        const ap_edit_entry *e = ap_edit_stack_at_const(&def->stack, i);
        if (!e) continue;
        const ap_module *m = ap_module_find(e->module_name);
        if (!m || !m->user_visible) continue;
        int idx = ap_edit_stack_add(out, e->module_name);
        if (idx < 0) continue;
        ap_edit_entry *dst = ap_edit_stack_at(out, idx);
        if (!dst) continue;
        memcpy(dst->params, e->params, sizeof(dst->params));
        snprintf(dst->display_name, sizeof(dst->display_name),
                 "%s", e->display_name);
        dst->enabled = e->enabled;
    }
}

int ap_pipeline_apply_to_stack(int64_t pipeline_id, ap_edit_stack *out)
{
    if (!out) return -1;
    ap_pipeline_def def;
    if (ap_pipeline_get(pipeline_id, &def) != 0) return -1;
    copy_pipeline_to_stack(&def, out);
    return 0;
}

int ap_pipeline_apply_default_to_stack(ap_edit_stack *out)
{
    if (!out) return -1;
    ap_pipeline_def def;
    if (ap_pipeline_get_default(&def) != 0) return -1;
    copy_pipeline_to_stack(&def, out);
    return 0;
}

int ap_settings_get(const char *key, char *out, size_t out_len)
{
    if (!key || !out || out_len == 0) return -1;
    sqlite3 *reg = NULL;
    if (registry_open(&reg) < 0) return -1;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(reg, "SELECT value FROM settings WHERE key = ?;",
                           -1, &st, NULL) != SQLITE_OK) {
        AP_ERROR("settings: prepare get: %s", sqlite3_errmsg(reg));
        sqlite3_close(reg);
        return -1;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    int result = -1;
    if (rc == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) {
            snprintf(out, out_len, "%s", v);
            result = 0;
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(reg);
    return result;
}

int ap_settings_set(const char *key, const char *value)
{
    if (!key) return -1;
    sqlite3 *reg = NULL;
    if (registry_open(&reg) < 0) return -1;

    sqlite3_stmt *st = NULL;
    if (!value || !*value) {
        if (sqlite3_prepare_v2(reg, "DELETE FROM settings WHERE key = ?;",
                               -1, &st, NULL) != SQLITE_OK) {
            AP_ERROR("settings: prepare delete: %s", sqlite3_errmsg(reg));
            sqlite3_close(reg);
            return -1;
        }
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    } else {
        if (sqlite3_prepare_v2(reg,
                "INSERT INTO settings(key, value) VALUES (?, ?) "
                "ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
                -1, &st, NULL) != SQLITE_OK) {
            AP_ERROR("settings: prepare upsert: %s", sqlite3_errmsg(reg));
            sqlite3_close(reg);
            return -1;
        }
        sqlite3_bind_text(st, 1, key,   -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, value, -1, SQLITE_STATIC);
    }
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_close(reg);
    if (rc != SQLITE_DONE) {
        AP_ERROR("settings: step: %s", sqlite3_errstr(rc));
        return -1;
    }
    return 0;
}

int ap_registry_list(ap_registry_entry *out, int max)
{
    if (!out || max <= 0) return 0;

    sqlite3 *reg = NULL;
    if (registry_open(&reg) < 0) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(reg,
            "SELECT id, path, name, created_at FROM libraries "
            "ORDER BY created_at DESC;",
            -1, &stmt, NULL) != SQLITE_OK) {
        AP_ERROR("registry: prepare list: %s", sqlite3_errmsg(reg));
        sqlite3_close(reg);
        return -1;
    }

    int n = 0;
    while (n < max) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            AP_ERROR("registry: list step: %s", sqlite3_errstr(rc));
            sqlite3_finalize(stmt);
            sqlite3_close(reg);
            return -1;
        }
        const char *id   = (const char *)sqlite3_column_text(stmt, 0);
        const char *path = (const char *)sqlite3_column_text(stmt, 1);
        const char *name = (const char *)sqlite3_column_text(stmt, 2);
        int64_t   ctime  = sqlite3_column_int64(stmt, 3);
        if (!id || !path) continue;
        snprintf(out[n].id,   sizeof(out[n].id),   "%s", id);
        snprintf(out[n].path, sizeof(out[n].path), "%s", path);
        snprintf(out[n].name, sizeof(out[n].name), "%s", name ? name : "");
        out[n].created_at = ctime;
        n++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(reg);
    return n;
}

// Tables are created first so backfill_culling_columns can ALTER an
// existing photos table to add the rating / flag / color columns before
// the indexes below try to reference them. CREATE TABLE IF NOT EXISTS
// is a no-op for an existing photos table predating those columns; the
// backfill's ALTER TABLE statements are what actually adds them.
static const char *LIBRARY_TABLES_SQL =
    "CREATE TABLE IF NOT EXISTS schema ("
    "    key   TEXT PRIMARY KEY,"
    "    value TEXT NOT NULL"
    ");"
    // The rating / flag / color columns cache each photo's culling
    // state (the sidecar is the source of truth) so grid filtering and
    // sorting stay a single indexed query. rating is 0-5; flag is an
    // ap_flag ordinal; color is an ap_color_label ordinal.
    "CREATE TABLE IF NOT EXISTS photos ("
    "    id           INTEGER PRIMARY KEY,"
    "    path         TEXT NOT NULL UNIQUE,"
    "    hash         BLOB,"
    "    identity     TEXT,"
    "    size         INTEGER,"
    "    capture_time INTEGER,"
    "    added_at     INTEGER NOT NULL,"
    "    rating       INTEGER NOT NULL DEFAULT 0,"
    "    flag         INTEGER NOT NULL DEFAULT 0,"
    "    color        INTEGER NOT NULL DEFAULT 0"
    ");"
    // Edit-render thumbnail cache. Keyed by the photo's relative path
    // (same key the in-memory photo list uses). `jpeg` is a small
    // JPEG of the photo rendered through its edit stack; `updated_at`
    // is when it was rendered, compared against the sidecar's mtime to
    // decide freshness.
    "CREATE TABLE IF NOT EXISTS thumbnails ("
    "    path       TEXT PRIMARY KEY,"
    "    jpeg       BLOB NOT NULL,"
    "    updated_at INTEGER NOT NULL"
    ");"
    // Per-library settings as opaque key/value strings. The `schema`
    // table above carries db-version metadata; this one is for
    // user-facing settings the library tracks (e.g. the per-library
    // default pipeline id).
    "CREATE TABLE IF NOT EXISTS settings ("
    "    key   TEXT PRIMARY KEY,"
    "    value TEXT NOT NULL"
    ");"
    // Registry of group names. Membership lives in the photo sidecars;
    // this table records which groups exist, so an empty group (one
    // with no photos) still persists.
    "CREATE TABLE IF NOT EXISTS groups ("
    "    name TEXT PRIMARY KEY"
    ");"
    // Named export-setting bundles (web JPEG, print TIFF, etc.).
    // `settings_blob` is a NUL-terminated key=value text block with
    // one assignment per line, as written by preset_to_blob().
    "CREATE TABLE IF NOT EXISTS export_presets ("
    "    id            INTEGER PRIMARY KEY,"
    "    name          TEXT NOT NULL UNIQUE,"
    "    settings_blob TEXT NOT NULL"
    ")";

static const char *LIBRARY_INDEXES_SQL =
    "CREATE INDEX IF NOT EXISTS idx_photos_capture_time ON photos(capture_time);"
    "CREATE INDEX IF NOT EXISTS idx_photos_hash         ON photos(hash);"
    "CREATE INDEX IF NOT EXISTS idx_photos_identity     ON photos(identity);"
    "CREATE INDEX IF NOT EXISTS idx_photos_rating       ON photos(rating);"
    "CREATE INDEX IF NOT EXISTS idx_photos_flag         ON photos(flag);"
    "CREATE INDEX IF NOT EXISTS idx_photos_color        ON photos(color);"
    // Drop the now-dead size index left over from the removed
    // size-pre-filter dedupe path; legacy libraries carried it from
    // before the EXIF identity switch.
    "DROP INDEX IF EXISTS idx_photos_size;";

// Backfill the culling columns on a photos table created before they
// were added. Each ALTER returns SQLITE_ERROR with "duplicate column
// name" when the column already exists — that case is success.
static void backfill_culling_columns(sqlite3 *db)
{
    static const char *ALTERS[] = {
        "ALTER TABLE photos ADD COLUMN rating INTEGER NOT NULL DEFAULT 0;",
        "ALTER TABLE photos ADD COLUMN flag   INTEGER NOT NULL DEFAULT 0;",
        "ALTER TABLE photos ADD COLUMN color  INTEGER NOT NULL DEFAULT 0;",
    };
    for (size_t i = 0; i < sizeof(ALTERS) / sizeof(ALTERS[0]); i++) {
        char *err = NULL;
        int rc = sqlite3_exec(db, ALTERS[i], NULL, NULL, &err);
        if (rc != SQLITE_OK && err && !strstr(err, "duplicate column")) {
            AP_WARN("library: backfill culling column: %s", err);
        }
        sqlite3_free(err);
    }
}

// Add the `size` column on a photos table predating the size pre-
// filter. As above, "duplicate column name" means the column already
// exists.
static void backfill_size_column(sqlite3 *db)
{
    char *err = NULL;
    int rc = sqlite3_exec(db,
        "ALTER TABLE photos ADD COLUMN size INTEGER;",
        NULL, NULL, &err);
    if (rc != SQLITE_OK && err && !strstr(err, "duplicate column")) {
        AP_WARN("library: backfill size column: %s", err);
    }
    sqlite3_free(err);
}

// Add the `identity` column on a photos table predating the EXIF
// identity dedupe. New imports populate both `identity` and `hash`.
// Rows from before the upgrade have a NULL identity, so a re-import
// of their source will copy again (the importer's dedupe path skips
// rows whose identity it can't recover); rebuild the library or
// clean by hand if that matters.
static void backfill_identity_column(sqlite3 *db)
{
    char *err = NULL;
    int rc = sqlite3_exec(db,
        "ALTER TABLE photos ADD COLUMN identity TEXT;",
        NULL, NULL, &err);
    if (rc != SQLITE_OK && err && !strstr(err, "duplicate column")) {
        AP_WARN("library: backfill identity column: %s", err);
    }
    sqlite3_free(err);
}

// Stat every photo with a NULL `size` and write the on-disk byte
// count back. One-shot pass on library open; subsequent imports
// store the size at copy time. Wrapped in a single transaction so
// the WAL doesn't grow per-row.
static void backfill_photo_sizes(ap_library *lib)
{
    if (!lib || !lib->db || !lib->root) return;

    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(lib->db,
            "SELECT path FROM photos WHERE size IS NULL;",
            -1, &sel, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_stmt *upd = NULL;
    if (sqlite3_prepare_v2(lib->db,
            "UPDATE photos SET size = ? WHERE path = ?;",
            -1, &upd, NULL) != SQLITE_OK) {
        sqlite3_finalize(sel);
        return;
    }

    sqlite3_exec(lib->db, "BEGIN;", NULL, NULL, NULL);
    int filled = 0;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        const char *rel = (const char *)sqlite3_column_text(sel, 0);
        if (!rel) continue;
        char abs[4096];
        if (snprintf(abs, sizeof(abs), "%s/%s",
                     lib->root, rel) >= (int)sizeof(abs)) {
            continue;
        }
        struct stat st;
        if (stat(abs, &st) != 0) continue;

        sqlite3_reset(upd);
        sqlite3_clear_bindings(upd);
        sqlite3_bind_int64(upd, 1, (int64_t)st.st_size);
        sqlite3_bind_text (upd, 2, rel, -1, SQLITE_STATIC);
        if (sqlite3_step(upd) == SQLITE_DONE) filled++;
    }
    sqlite3_exec(lib->db, "COMMIT;", NULL, NULL, NULL);

    sqlite3_finalize(upd);
    sqlite3_finalize(sel);

    if (filled > 0) {
        AP_INFO("library: backfilled size for %d photo%s",
                filled, filled == 1 ? "" : "s");
    }
}

static int exec_simple(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        AP_ERROR("library: sqlite exec failed: %s", err ? err : "(no message)");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int set_schema_kv(sqlite3 *db, const char *key, const char *value)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO schema(key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        AP_ERROR("library: prepare schema upsert: %s", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, key,   -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        AP_ERROR("library: schema upsert step: %s", sqlite3_errstr(rc));
        return -1;
    }
    return 0;
}

static bool is_raw_file(const char *name)
{
    return ap_raw_is_raw_path(name);
}

static int cache_append_path(ap_library_cache *cache, const char *rel)
{
    if (cache->photo_count == cache->photo_capacity) {
        int new_cap = cache->photo_capacity ? cache->photo_capacity * 2 : 64;
        char **np = realloc(cache->photo_paths, (size_t)new_cap * sizeof(*np));
        if (!np) {
            AP_ERROR("library: photo list grow failed");
            return -1;
        }
        cache->photo_paths    = np;
        cache->photo_capacity = new_cap;
    }
    char *dup = strdup(rel);
    if (!dup) {
        AP_ERROR("library: path dup failed");
        return -1;
    }
    cache->photo_paths[cache->photo_count++] = dup;
    return 0;
}

// Absolute path of the cache's n-th photo into `buf`. Mirrors
// ap_library_photo_absolute_path but works on a detached cache, so the
// off-thread build can resolve sidecar paths without an ap_library.
static int cache_abs_path(const ap_library_cache *cache, const char *root,
                          int index, char *buf, size_t buflen)
{
    if (!cache || !root || !buf || index < 0 || index >= cache->photo_count) {
        return -1;
    }
    int n = snprintf(buf, buflen, "%s/%s", root, cache->photo_paths[index]);
    if (n < 0 || (size_t)n >= buflen) return -1;
    return 0;
}

static int insert_photo(sqlite3 *db, sqlite3_stmt *stmt, const char *rel,
                        int64_t added_at)
{
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, rel, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, added_at);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        AP_ERROR("library: insert photo '%s': %s", rel, sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

static int scan_dir(sqlite3 *db, sqlite3_stmt *insert_stmt,
                    const char *abs_dir, const char *rel_prefix,
                    int64_t added_at)
{
    ap_dir *d = ap_dir_open(abs_dir);
    if (!d) {
        AP_WARN("library: opendir(%s): %s", abs_dir, strerror(ap_dir_open_errno()));
        return 0;
    }

    const char *ename;
    while ((ename = ap_dir_read(d)) != NULL) {
        if (strcmp(ename, ".")  == 0 ||
            strcmp(ename, "..") == 0) continue;

        char abs_child[4096];
        if (snprintf(abs_child, sizeof(abs_child), "%s/%s",
                     abs_dir, ename) >= (int)sizeof(abs_child)) {
            AP_WARN("library: path too long, skipping %s/%s", abs_dir, ename);
            continue;
        }

        struct stat st;
        if (stat(abs_child, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            char child_rel[2048];
            if (rel_prefix[0] == '\0') {
                snprintf(child_rel, sizeof(child_rel), "%s", ename);
            } else {
                snprintf(child_rel, sizeof(child_rel), "%s/%s",
                         rel_prefix, ename);
            }
            scan_dir(db, insert_stmt, abs_child, child_rel, added_at);
            continue;
        }

        if (!S_ISREG(st.st_mode)) continue;
        if (!is_raw_file(ename)) continue;

        char rel[2048];
        if (rel_prefix[0] == '\0') {
            snprintf(rel, sizeof(rel), "%s", ename);
        } else {
            snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, ename);
        }
        insert_photo(db, insert_stmt, rel, added_at);
    }

    ap_dir_close(d);
    return 0;
}

// Progress + cancel for an off-thread cache build. `cb` mirrors the
// importer's callback contract: it returns false to request cancel, at
// which point the build aborts and frees the partial cache. NULL `cb`
// (the synchronous open path) never cancels.
typedef struct {
    bool (*cb)(int done, int total, void *ud);
    void  *ud;
    int    done;
    int    total;
    bool   cancellable;  // false => report progress but never abort the build
} cache_build_ctx;

// Tick one unit of progress; returns false when the caller asked to
// cancel and this build honors cancellation. A NULL ctx (or NULL
// callback) always continues. A non-cancellable build still reports
// progress but always continues — used by DELETE, whose post-mutation
// cache must be built in full so the swap reflects the removed files.
static bool cache_progress_tick(cache_build_ctx *bp)
{
    if (!bp || !bp->cb) return true;
    bp->done++;
    bool keep = bp->cb(bp->done, bp->total, bp->ud);
    return bp->cancellable ? keep : true;
}

static int cache_load_groups(sqlite3 *db, ap_library_cache *cache,
                             const char *root, cache_build_ctx *bp);
static int cache_load_culling(sqlite3 *db, ap_library_cache *cache,
                              const char *root, cache_build_ctx *bp);

static const char *sort_order_clause(ap_library_sort sort)
{
    switch (sort) {
    case AP_SORT_CAPTURE_TIME:
        return "ORDER BY COALESCE(capture_time, 0), path";
    case AP_SORT_ADDED_AT:
        return "ORDER BY added_at, path";
    case AP_SORT_RATING:
        return "ORDER BY rating DESC, path";
    case AP_SORT_PATH:
    default:
        return "ORDER BY path";
    }
}

// Read the photo paths from `db` into `cache`, in the order given by
// `sort`. Does not touch thumbs or the group index; callers are
// responsible for those.
static int cache_load_photos(sqlite3 *db, ap_library_cache *cache,
                             ap_library_sort sort)
{
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT path FROM photos %s;",
             sort_order_clause(sort));

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        AP_ERROR("library: prepare photo list: %s", sqlite3_errmsg(db));
        return -1;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(stmt, 0);
        if (!path) continue;
        if (cache_append_path(cache, path) < 0) {
            sqlite3_finalize(stmt);
            return -1;
        }
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        AP_ERROR("library: photo list iteration: %s", sqlite3_errstr(rc));
        return -1;
    }
    return 0;
}

static void prune_missing_photos(sqlite3 *db, const char *root);

// Run the disk -> db reconciliation for a rescan on `db`: INSERT OR
// IGNORE every raw file currently under `root`, then prune rows whose
// files are gone. Run on the cache build's own connection for the
// RESCAN op. Returns 0 on success.
static int db_rescan(sqlite3 *db, const char *root)
{
    sqlite3_stmt *insert_stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO photos(path, added_at) VALUES (?, ?);",
            -1, &insert_stmt, NULL) != SQLITE_OK) {
        AP_ERROR("library: rescan prepare insert: %s", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    scan_dir(db, insert_stmt, root, "", (int64_t)time(NULL));
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_finalize(insert_stmt);

    // Scan only adds; reconcile the other direction too. prune drops
    // db rows whose files have been removed.
    prune_missing_photos(db, root);
    return 0;
}

// Free a cache's heap-owned contents (paths, groups, culling) but not
// the container. Used by the swap, close, and ap_library_cache_free.
static void cache_clear(ap_library_cache *cache)
{
    if (!cache) return;
    for (int i = 0; i < cache->photo_count; i++) {
        free(cache->photo_paths[i]);
    }
    free(cache->photo_paths);
    cache->photo_paths    = NULL;
    cache->photo_count    = 0;
    cache->photo_capacity = 0;
    free(cache->photo_groups);
    cache->photo_groups = NULL;
    free(cache->photo_culling);
    cache->photo_culling = NULL;
    cache->group_count = 0;
}

// Open an independent connection to <root>/library.db with the same
// WAL / busy-timeout / foreign-keys pragmas the live handle uses. The
// schema already exists (the library is open), so no DDL runs here.
// Caller closes. Returns NULL on failure.
static sqlite3 *open_library_db(const char *root)
{
    char db_path[4096];
    if (snprintf(db_path, sizeof(db_path), "%s/library.db", root)
            >= (int)sizeof(db_path)) {
        return NULL;
    }
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        AP_ERROR("library: cache build open(%s): %s",
                 db_path, db ? sqlite3_errmsg(db) : "(no db)");
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON;",  NULL, NULL, NULL);
    return db;
}

// Delete the del_count relative paths in `del_rel` from disk + db on
// `db`, in one transaction for the db rows. Honors cancel between
// photos (the committed rows stay durable + consistent). Mirrors
// ap_library_photo_remove's per-photo disk + db work.
static void db_delete_paths(sqlite3 *db, const char *root,
                            const char (*del_rel)[4096], int del_count,
                            bool (*progress)(int, int, void *), void *ud)
{
    sqlite3_stmt *del_p = NULL, *del_t = NULL;
    sqlite3_prepare_v2(db, "DELETE FROM photos WHERE path = ?;",
                       -1, &del_p, NULL);
    sqlite3_prepare_v2(db, "DELETE FROM thumbnails WHERE path = ?;",
                       -1, &del_t, NULL);
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    for (int i = 0; i < del_count; i++) {
        char abs[4096];
        if (snprintf(abs, sizeof(abs), "%s/%s", root, del_rel[i])
                < (int)sizeof(abs)) {
            if (unlink(abs) != 0 && errno != ENOENT) {
                AP_WARN("library: unlink %s: %s", abs, strerror(errno));
            }
            ap_sidecar_remove(abs);
        }
        if (del_p) {
            sqlite3_reset(del_p);
            sqlite3_bind_text(del_p, 1, del_rel[i], -1, SQLITE_STATIC);
            sqlite3_step(del_p);
        }
        if (del_t) {
            sqlite3_reset(del_t);
            sqlite3_bind_text(del_t, 1, del_rel[i], -1, SQLITE_STATIC);
            sqlite3_step(del_t);
        }
        // Poll cancel (without disturbing the bar's total): stop
        // scheduling further deletes, but still COMMIT what is done.
        if (progress && !progress(0, 0, ud)) break;
    }
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_finalize(del_p);
    sqlite3_finalize(del_t);
}

ap_library_cache *ap_library_cache_build(
    const char *root, ap_library_op op, ap_library_sort sort,
    const char (*del_rel)[4096], int del_count,
    bool (*progress)(int, int, void *), void *ud)
{
    if (!root) return NULL;

    sqlite3 *db = open_library_db(root);
    if (!db) return NULL;

    // Phase 1: the op's db mutation, on our own connection.
    if (op == AP_LIBRARY_OP_DELETE && del_rel && del_count > 0) {
        db_delete_paths(db, root, del_rel, del_count, progress, ud);
    } else if (op == AP_LIBRARY_OP_RESCAN) {
        db_rescan(db, root);
    }

    // Phase 2: build the replacement cache from the (now-mutated) db.
    ap_library_cache *cache = calloc(1, sizeof(*cache));
    if (!cache) {
        sqlite3_close(db);
        return NULL;
    }
    if (cache_load_photos(db, cache, sort) < 0) {
        ap_library_cache_free(cache);
        sqlite3_close(db);
        return NULL;
    }

    // The sidecar passes are cancellable for RELOAD/RESCAN (no db
    // mutation to reflect), but not for DELETE — its post-delete cache
    // must be complete so the swap matches the files actually removed.
    cache_build_ctx ctx = {
        .cb          = progress,
        .ud          = ud,
        .done        = 0,
        .total       = 2 * cache->photo_count,
        .cancellable = (op != AP_LIBRARY_OP_DELETE),
    };

    int gr = cache_load_groups(db, cache, root, &ctx);
    int cr = (gr == 0) ? cache_load_culling(db, cache, root, &ctx) : 0;
    sqlite3_close(db);

    if (gr < 0 || cr < 0 || gr == 1 || cr == 1) {
        // <0 is an error, ==1 is a cancel during a cancellable build.
        ap_library_cache_free(cache);
        return NULL;
    }
    return cache;
}

void ap_library_cache_swap(ap_library *lib, ap_library_cache *fresh)
{
    if (!lib || !fresh) return;

    int old_n = lib->cache ? lib->cache->photo_count : 0;
    if (lib->thumbs) {
        for (int i = 0; i < old_n; i++) {
            ap_thumbnail_destroy(lib->thumbs[i]);
        }
        free(lib->thumbs);
        lib->thumbs = NULL;
    }
    free(lib->thumb_failed);
    lib->thumb_failed = NULL;
    lib->thumb_cursor = 0;

    if (lib->cache) {
        cache_clear(lib->cache);
        free(lib->cache);
    }
    lib->cache = fresh;

    if (lib->cache->photo_count > 0) {
        lib->thumbs = calloc((size_t)lib->cache->photo_count,
                             sizeof(*lib->thumbs));
        lib->thumb_failed = calloc((size_t)lib->cache->photo_count,
                                   sizeof(*lib->thumb_failed));
        if (!lib->thumbs || !lib->thumb_failed) {
            AP_ERROR("library: thumbnail cache realloc failed after swap");
        }
    }
}

void ap_library_cache_free(ap_library_cache *cache)
{
    if (!cache) return;
    cache_clear(cache);
    free(cache);
}

// Index of a group name in the cache's registry, or -1.
static int cache_registry_index(const ap_library_cache *cache,
                                const char *name)
{
    for (int i = 0; i < cache->group_count; i++) {
        if (strcmp(cache->group_names[i], name) == 0) return i;
    }
    return -1;
}

// Register a group name — into the cache's registry and, when `db` is
// non-NULL, the `groups` table. Idempotent.
static void cache_registry_add(sqlite3 *db, ap_library_cache *cache,
                               const char *name)
{
    if (!name || !*name || cache_registry_index(cache, name) >= 0) {
        return;
    }
    if (cache->group_count < AP_LIBRARY_GROUPS_MAX) {
        snprintf(cache->group_names[cache->group_count], AP_GROUP_NAME_LEN,
                 "%s", name);
        cache->group_count++;
    } else {
        AP_WARN("library: group registry full (%d)", AP_LIBRARY_GROUPS_MAX);
    }
    if (db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO groups(name) VALUES (?);",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
}

// Remove a group name from the cache registry + the `groups` table.
// Photo membership in the sidecars is the caller's separate
// responsibility.
static void cache_registry_remove(sqlite3 *db, ap_library_cache *cache,
                                  const char *name)
{
    int idx = cache_registry_index(cache, name);
    if (idx >= 0) {
        for (int i = idx; i + 1 < cache->group_count; i++) {
            memcpy(cache->group_names[i], cache->group_names[i + 1],
                   AP_GROUP_NAME_LEN);
        }
        cache->group_count--;
    }
    if (db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db,
                "DELETE FROM groups WHERE name = ?;",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
}

// Build the group registry + per-photo membership index. The `groups`
// table seeds the registry; any group a sidecar references that the
// table is missing gets folded in, keeping the registry a superset of
// all membership.
static int cache_load_groups(sqlite3 *db, ap_library_cache *cache,
                             const char *root, cache_build_ctx *bp)
{
    cache->group_count = 0;
    if (db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT name FROM groups ORDER BY name;",
                -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW &&
                   cache->group_count < AP_LIBRARY_GROUPS_MAX) {
                const char *n = (const char *)sqlite3_column_text(st, 0);
                if (n) {
                    snprintf(cache->group_names[cache->group_count],
                             AP_GROUP_NAME_LEN, "%s", n);
                    cache->group_count++;
                }
            }
            sqlite3_finalize(st);
        }
    }

    if (cache->photo_count <= 0) {
        return 0;
    }
    cache->photo_groups = calloc((size_t)cache->photo_count,
                                 sizeof(*cache->photo_groups));
    if (!cache->photo_groups) {
        AP_ERROR("library: group index alloc failed");
        return -1;
    }
    for (int i = 0; i < cache->photo_count; i++) {
        char path[4096];
        if (cache_abs_path(cache, root, i, path, sizeof(path)) != 0) {
            continue;
        }
        ap_sidecar_load_groups(path, &cache->photo_groups[i]);
        for (int g = 0; g < cache->photo_groups[i].count; g++) {
            cache_registry_add(db, cache, cache->photo_groups[i].names[g]);
        }
        if (!cache_progress_tick(bp)) return 1;  // cancelled
    }
    return 0;
}

// Write a photo's cached culling columns from its rel path.
static void store_culling_row(sqlite3 *db, const char *rel,
                              const ap_photo_culling *c)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "UPDATE photos SET rating = ?, flag = ?, color = ? "
            "WHERE path = ?;",
            -1, &st, NULL) != SQLITE_OK) {
        AP_WARN("library: prepare culling update: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int(st, 1, c->rating);
    sqlite3_bind_int(st, 2, (int)c->flag);
    sqlite3_bind_int(st, 3, (int)c->color);
    sqlite3_bind_text(st, 4, rel, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) {
        AP_WARN("library: culling update step: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(st);
}

// Build the in-memory culling cache. The sidecar is the source of
// truth: each photo's sidecar is parsed and, where it disagrees with
// the cached db columns, the db is reconciled (so culling set outside
// aperture is picked up). Mirrors load_group_cache.
static int cache_load_culling(sqlite3 *db, ap_library_cache *cache,
                              const char *root, cache_build_ctx *bp)
{
    if (cache->photo_count <= 0) {
        return 0;
    }
    cache->photo_culling = calloc((size_t)cache->photo_count,
                                  sizeof(*cache->photo_culling));
    if (!cache->photo_culling) {
        AP_ERROR("library: culling cache alloc failed");
        return -1;
    }

    // Seed from the db columns first — the fast path when sidecars and
    // cache already agree.
    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT rating, flag, color FROM photos WHERE path = ?;",
            -1, &sel, NULL) != SQLITE_OK) {
        AP_ERROR("library: prepare culling select: %s", sqlite3_errmsg(db));
        return -1;
    }

    for (int i = 0; i < cache->photo_count; i++) {
        ap_photo_culling *cached = &cache->photo_culling[i];
        ap_photo_culling_clear(cached);

        sqlite3_reset(sel);
        sqlite3_bind_text(sel, 1, cache->photo_paths[i], -1, SQLITE_STATIC);
        if (sqlite3_step(sel) == SQLITE_ROW) {
            cached->rating = ap_rating_clamp(sqlite3_column_int(sel, 0));
            cached->flag   = (ap_flag)sqlite3_column_int(sel, 1);
            cached->color  = (ap_color_label)sqlite3_column_int(sel, 2);
        }

        // Reconcile against the sidecar (source of truth). When no
        // sidecar exists the cached db value stands.
        char path[4096];
        if (cache_abs_path(cache, root, i, path, sizeof(path)) == 0) {
            ap_photo_culling side;
            if (ap_sidecar_load_culling(path, &side) == 0) {
                if (side.rating != cached->rating ||
                    side.flag   != cached->flag   ||
                    side.color  != cached->color) {
                    *cached = side;
                    store_culling_row(db, cache->photo_paths[i], cached);
                }
            }
        }
        if (!cache_progress_tick(bp)) {           // cancelled
            sqlite3_finalize(sel);
            return 1;
        }
    }
    sqlite3_finalize(sel);
    return 0;
}

// Reconcile the photos table with the filesystem: drop rows (and their
// cached thumbnails) for files no longer on disk. The scan only ever
// adds — without this, files moved or deleted outside aperture leave
// phantom entries behind.
static void prune_missing_photos(sqlite3 *db, const char *root)
{
    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(db, "SELECT path FROM photos;",
                           -1, &sel, NULL) != SQLITE_OK) {
        return;
    }
    char **gone = NULL;
    int    n = 0, cap = 0;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        const char *rel = (const char *)sqlite3_column_text(sel, 0);
        if (!rel) continue;
        char abs[4096];
        if (snprintf(abs, sizeof(abs), "%s/%s",
                     root, rel) >= (int)sizeof(abs)) {
            continue;
        }
        struct stat st;
        if (stat(abs, &st) == 0) continue;       // still on disk
        if (n == cap) {
            int nc = cap ? cap * 2 : 32;
            char **np = realloc(gone, (size_t)nc * sizeof(*np));
            if (!np) break;
            gone = np;
            cap  = nc;
        }
        gone[n] = strdup(rel);
        if (gone[n]) n++;
    }
    sqlite3_finalize(sel);

    if (n > 0) {
        sqlite3_stmt *del_p = NULL, *del_t = NULL;
        sqlite3_prepare_v2(db, "DELETE FROM photos WHERE path = ?;",
                           -1, &del_p, NULL);
        sqlite3_prepare_v2(db, "DELETE FROM thumbnails WHERE path = ?;",
                           -1, &del_t, NULL);
        sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
        for (int i = 0; i < n; i++) {
            if (del_p) {
                sqlite3_reset(del_p);
                sqlite3_bind_text(del_p, 1, gone[i], -1, SQLITE_STATIC);
                sqlite3_step(del_p);
            }
            if (del_t) {
                sqlite3_reset(del_t);
                sqlite3_bind_text(del_t, 1, gone[i], -1, SQLITE_STATIC);
                sqlite3_step(del_t);
            }
        }
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
        sqlite3_finalize(del_p);
        sqlite3_finalize(del_t);
        AP_INFO("library: pruned %d photo(s) no longer on disk", n);
    }
    for (int i = 0; i < n; i++) {
        free(gone[i]);
    }
    free(gone);
}

// Move a file, falling back to copy + unlink across filesystems.
// Returns 0 on success, -1 on failure (with a warning logged).
static int move_file(const char *src_path, const char *dst_path)
{
    if (ap_rename_replace(src_path, dst_path) == 0) return 0;
    if (errno != EXDEV) {
        AP_WARN("library: rename(%s, %s): %s",
                src_path, dst_path, strerror(errno));
        return -1;
    }
    // cross-volume fallback: copy atomically (temp + durable rename) so
    // an interrupted migration never leaves a truncated db at dst_path.
    FILE *src = fopen(src_path, "rb");
    ap_atomic *dst = ap_atomic_open(dst_path);
    bool ok = src && dst;
    if (ok) {
        FILE *df = ap_atomic_file(dst);
        char cbuf[65536];
        size_t nr;
        while ((nr = fread(cbuf, 1, sizeof(cbuf), src)) > 0) {
            if (fwrite(cbuf, 1, nr, df) != nr) { ok = false; break; }
        }
        ok = ok && !ferror(src);
    }
    if (src) fclose(src);
    if (!ok) {
        if (dst) ap_atomic_abort(dst);
        AP_WARN("library: copy(%s, %s) failed", src_path, dst_path);
        return -1;
    }
    if (ap_atomic_commit(dst) != 0) {
        AP_WARN("library: commit(%s) failed", dst_path);
        return -1;
    }
    unlink(src_path);
    return 0;
}

ap_library *ap_library_open(const char *path)
{
    if (!path) {
        AP_ERROR("ap_library_open: NULL path");
        return NULL;
    }

    char *root = realpath(path, NULL);
    if (!root) {
        AP_ERROR("ap_library_open: realpath(%s): %s", path, strerror(errno));
        return NULL;
    }
    struct stat st;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        AP_ERROR("ap_library_open: %s is not a directory", root);
        free(root);
        return NULL;
    }

    ap_library *lib = calloc(1, sizeof(*lib));
    if (!lib) {
        AP_ERROR("ap_library_open: out of memory");
        free(root);
        return NULL;
    }
    lib->root = root;

    if (registry_resolve_id(lib->root, lib->id) < 0) {
        goto fail;
    }

    // Load existing display name from the registry, if any.
    {
        sqlite3 *reg = NULL;
        if (registry_open(&reg) == 0) {
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(reg,
                    "SELECT name FROM libraries WHERE id = ?;",
                    -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, lib->id, -1, SQLITE_STATIC);
                if (sqlite3_step(st) == SQLITE_ROW) {
                    const char *n = (const char *)sqlite3_column_text(st, 0);
                    snprintf(lib->name, sizeof(lib->name), "%s", n ? n : "");
                }
                sqlite3_finalize(st);
            }
            sqlite3_close(reg);
        }
    }

    // The library's db lives in the library root: <library>/library.db.
    // No hidden subdirectory — aperture's per-library state is one file
    // that moves with the folder.
    char db_path[4096];
    if (snprintf(db_path, sizeof(db_path), "%s/library.db",
                 lib->root) >= (int)sizeof(db_path)) {
        AP_ERROR("ap_library_open: db path too long");
        goto fail;
    }

    if (access(db_path, F_OK) != 0) {
        // Migrate from the original app-root registry-keyed layout
        // (<app_root>/libraries/<uuid>.db) if present. The earlier
        // <library>/.aperture/library.db layout has been retired.
        char legacy_filename[64];
        snprintf(legacy_filename, sizeof(legacy_filename),
                 "libraries/%s.db", lib->id);
        char legacy_path[4096];
        if (ap_app_root_join(legacy_filename, legacy_path,
                             sizeof(legacy_path)) == 0 &&
            access(legacy_path, F_OK) == 0 &&
            move_file(legacy_path, db_path) == 0) {
            AP_INFO("library: migrated db %s -> %s",
                    legacy_path, db_path);
        }
    }

    if (sqlite3_open(db_path, &lib->db) != SQLITE_OK) {
        AP_ERROR("ap_library_open: sqlite3_open(%s): %s",
                 db_path, sqlite3_errmsg(lib->db));
        goto fail;
    }
    // wait on a contended write lock rather than failing immediately;
    // the import path opens its own handle to this WAL db.
    sqlite3_busy_timeout(lib->db, 5000);
    sqlite3_exec(lib->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "PRAGMA foreign_keys=ON;",  NULL, NULL, NULL);

    if (exec_simple(lib->db, LIBRARY_TABLES_SQL)                       < 0) goto fail;
    backfill_culling_columns(lib->db);
    backfill_size_column(lib->db);
    backfill_identity_column(lib->db);
    if (exec_simple(lib->db, LIBRARY_INDEXES_SQL)                      < 0) goto fail;
    if (set_schema_kv(lib->db, "version",          APERTURE_DB_VERSION) < 0) goto fail;
    if (set_schema_kv(lib->db, "aperture_version", APERTURE_VERSION)    < 0) goto fail;
    if (set_schema_kv(lib->db, "library_id",       lib->id)             < 0) goto fail;
    if (set_schema_kv(lib->db, "library_path",     lib->root)           < 0) goto fail;

    sqlite3_stmt *insert_stmt = NULL;
    int rc = sqlite3_prepare_v2(lib->db,
        "INSERT OR IGNORE INTO photos(path, added_at) VALUES (?, ?);",
        -1, &insert_stmt, NULL);
    if (rc != SQLITE_OK) {
        AP_ERROR("ap_library_open: prepare insert: %s", sqlite3_errmsg(lib->db));
        goto fail;
    }

    int64_t now = (int64_t)time(NULL);

    sqlite3_exec(lib->db, "BEGIN;", NULL, NULL, NULL);
    scan_dir(lib->db, insert_stmt, lib->root, "", now);
    sqlite3_exec(lib->db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_finalize(insert_stmt);

    // Scan only adds; reconcile the other direction too.
    prune_missing_photos(lib->db, lib->root);

    // One-shot: stat any photos with NULL size and fill the column.
    // Subsequent imports store size at copy time, so this only does
    // real work the first time the library is opened after upgrade.
    backfill_photo_sizes(lib);

    lib->cache = calloc(1, sizeof(*lib->cache));
    if (!lib->cache) {
        AP_ERROR("library: cache alloc failed");
        goto fail;
    }

    if (cache_load_photos(lib->db, lib->cache, AP_SORT_PATH) < 0) goto fail;

    if (lib->cache->photo_count > 0) {
        lib->thumbs = calloc((size_t)lib->cache->photo_count, sizeof(*lib->thumbs));
        if (!lib->thumbs) {
            AP_ERROR("library: thumbnail cache alloc failed");
            goto fail;
        }
        lib->thumb_failed = calloc((size_t)lib->cache->photo_count,
                                   sizeof(*lib->thumb_failed));
        if (!lib->thumb_failed) {
            AP_ERROR("library: thumbnail failed-flag alloc failed");
            goto fail;
        }
    }

    if (cache_load_groups(lib->db, lib->cache, lib->root, NULL)   < 0) goto fail;
    if (cache_load_culling(lib->db, lib->cache, lib->root, NULL) < 0) goto fail;

    AP_INFO("library: %s [%s] (%d photos)",
            lib->root, lib->id, lib->cache->photo_count);
    return lib;

fail:
    ap_library_close(lib);
    return NULL;
}

void ap_library_close(ap_library *lib)
{
    if (!lib) return;
    int n = lib->cache ? lib->cache->photo_count : 0;
    if (lib->thumbs) {
        for (int i = 0; i < n; i++) {
            ap_thumbnail_destroy(lib->thumbs[i]);
        }
        free(lib->thumbs);
    }
    free(lib->thumb_failed);
    if (lib->cache) {
        cache_clear(lib->cache);
        free(lib->cache);
    }
    if (lib->db) {
        sqlite3_close(lib->db);
    }
    free(lib->root);
    free(lib);
}

const char *ap_library_root(const ap_library *lib)
{
    return lib ? lib->root : NULL;
}

const char *ap_library_name(const ap_library *lib)
{
    return lib ? lib->name : "";
}

int ap_library_set_name(ap_library *lib, const char *name)
{
    if (!lib) return -1;
    sqlite3 *reg = NULL;
    if (registry_open(&reg) < 0) return -1;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(reg,
            "UPDATE libraries SET name = ? WHERE id = ?;",
            -1, &st, NULL) != SQLITE_OK) {
        AP_ERROR("registry: prepare set name: %s", sqlite3_errmsg(reg));
        sqlite3_close(reg);
        return -1;
    }
    if (name && *name) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(st, 1);
    }
    sqlite3_bind_text(st, 2, lib->id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_close(reg);
    if (rc != SQLITE_DONE) {
        AP_ERROR("registry: set name step: %s", sqlite3_errstr(rc));
        return -1;
    }

    snprintf(lib->name, sizeof(lib->name), "%s", (name && *name) ? name : "");
    return 0;
}

// Per-library settings live in the per-library db's `settings` table;
// the key namespace is internal and not part of any public schema.
static const char *LIB_SETTING_DEFAULT_PIPELINE = "default_pipeline_id";

int64_t ap_library_default_pipeline_id(const ap_library *lib)
{
    if (!lib || !lib->db) return 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(lib->db,
            "SELECT value FROM settings WHERE key = ?;",
            -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, LIB_SETTING_DEFAULT_PIPELINE,
                      -1, SQLITE_STATIC);
    int64_t id = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) id = (int64_t)strtoll(v, NULL, 10);
    }
    sqlite3_finalize(st);

    // Validate against the registry: if the pointed-to pipeline was
    // deleted, drop the stale pointer (caller falls back to the
    // app-wide default).
    if (id > 0) {
        ap_pipeline_def probe;
        if (ap_pipeline_get(id, &probe) != 0) {
            return 0;
        }
    }
    return id;
}

int ap_library_set_default_pipeline_id(ap_library *lib, int64_t id)
{
    if (!lib || !lib->db) return -1;
    if (id == 0) {
        // Clear: delete the setting row.
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(lib->db,
                "DELETE FROM settings WHERE key = ?;",
                -1, &st, NULL) != SQLITE_OK) {
            AP_ERROR("library: prepare clear default pipeline: %s",
                     sqlite3_errmsg(lib->db));
            return -1;
        }
        sqlite3_bind_text(st, 1, LIB_SETTING_DEFAULT_PIPELINE,
                          -1, SQLITE_STATIC);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        return (rc == SQLITE_DONE) ? 0 : -1;
    }

    char value[32];
    snprintf(value, sizeof(value), "%lld", (long long)id);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(lib->db,
            "INSERT INTO settings(key, value) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
            -1, &st, NULL) != SQLITE_OK) {
        AP_ERROR("library: prepare set default pipeline: %s",
                 sqlite3_errmsg(lib->db));
        return -1;
    }
    sqlite3_bind_text(st, 1, LIB_SETTING_DEFAULT_PIPELINE,
                      -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int ap_library_setting_get(const ap_library *lib, const char *key,
                           char *out, size_t out_len)
{
    if (!lib || !lib->db || !key || !out || out_len == 0) return -1;
    out[0] = '\0';
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(lib->db,
            "SELECT value FROM settings WHERE key = ?;",
            -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    int rc = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) {
            snprintf(out, out_len, "%s", v);
            rc = 0;
        }
    }
    sqlite3_finalize(st);
    return rc;
}

int ap_library_setting_set(ap_library *lib, const char *key,
                           const char *value)
{
    if (!lib || !lib->db || !key) return -1;
    sqlite3_stmt *st = NULL;
    if (!value || !*value) {
        if (sqlite3_prepare_v2(lib->db,
                "DELETE FROM settings WHERE key = ?;",
                -1, &st, NULL) != SQLITE_OK) {
            return -1;
        }
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        return (rc == SQLITE_DONE) ? 0 : -1;
    }
    if (sqlite3_prepare_v2(lib->db,
            "INSERT INTO settings(key, value) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
            -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, key,   -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int ap_library_photo_count(const ap_library *lib)
{
    return lib ? lib->cache->photo_count : 0;
}

const char *ap_library_photo_relative_path(const ap_library *lib, int index)
{
    if (!lib || index < 0 || index >= lib->cache->photo_count) return NULL;
    return lib->cache->photo_paths[index];
}

int ap_library_photo_absolute_path(const ap_library *lib, int index,
                                   char *buf, size_t buflen)
{
    if (!lib || !buf || index < 0 || index >= lib->cache->photo_count) {
        return -1;
    }
    int n = snprintf(buf, buflen, "%s/%s", lib->root, lib->cache->photo_paths[index]);
    if (n < 0 || (size_t)n >= buflen) {
        return -1;
    }
    return 0;
}

int ap_library_photo_remove(ap_library *lib, int index)
{
    if (!lib || index < 0 || index >= lib->cache->photo_count) {
        return -1;
    }
    const char *rel = lib->cache->photo_paths[index];

    // Delete the raw file and its sidecar from disk. The library holds
    // an imported copy; the photographer's originals live elsewhere. A
    // failure here (permissions, already gone) is logged but not fatal
    // — the de-index proceeds and a later scan reconciles.
    char abs[4096];
    if (snprintf(abs, sizeof(abs), "%s/%s", lib->root, rel)
            < (int)sizeof(abs)) {
        if (unlink(abs) != 0 && errno != ENOENT) {
            AP_WARN("library: unlink %s: %s", abs, strerror(errno));
        }
        ap_sidecar_remove(abs);
    }

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(lib->db, "DELETE FROM photos WHERE path = ?;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, rel, -1, SQLITE_STATIC);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(lib->db, "DELETE FROM thumbnails WHERE path = ?;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, rel, -1, SQLITE_STATIC);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);

    free(lib->cache->photo_paths[index]);
    if (lib->thumbs && lib->thumbs[index]) {
        // may still be grid-bound; defer (gates pass immediately when the
        // caller already idled the device).
        ap_thumbnail_retire(lib->thumbs[index]);
    }

    int tail = lib->cache->photo_count - index - 1;
    if (tail > 0) {
        memmove(&lib->cache->photo_paths[index], &lib->cache->photo_paths[index + 1],
                (size_t)tail * sizeof(*lib->cache->photo_paths));
        if (lib->thumbs) {
            memmove(&lib->thumbs[index], &lib->thumbs[index + 1],
                    (size_t)tail * sizeof(*lib->thumbs));
        }
        if (lib->thumb_failed) {
            memmove(&lib->thumb_failed[index], &lib->thumb_failed[index + 1],
                    (size_t)tail * sizeof(*lib->thumb_failed));
        }
        if (lib->cache->photo_groups) {
            memmove(&lib->cache->photo_groups[index], &lib->cache->photo_groups[index + 1],
                    (size_t)tail * sizeof(*lib->cache->photo_groups));
        }
        if (lib->cache->photo_culling) {
            memmove(&lib->cache->photo_culling[index], &lib->cache->photo_culling[index + 1],
                    (size_t)tail * sizeof(*lib->cache->photo_culling));
        }
    }
    lib->cache->photo_count--;
    lib->cache->photo_paths[lib->cache->photo_count] = NULL;
    if (lib->thumbs) {
        lib->thumbs[lib->cache->photo_count] = NULL;
    }
    if (lib->thumb_failed) {
        lib->thumb_failed[lib->cache->photo_count] = false;
    }
    if (lib->thumb_cursor > index) {
        lib->thumb_cursor--;
    }
    return 0;
}

ap_thumbnail *ap_library_thumbnail(const ap_library *lib, int index)
{
    if (!lib || !lib->thumbs || index < 0 || index >= lib->cache->photo_count) {
        return NULL;
    }
    return lib->thumbs[index];
}

void ap_library_set_thumbnail(ap_library *lib, int index, ap_thumbnail *t)
{
    if (!lib || !lib->thumbs || index < 0 || index >= lib->cache->photo_count) {
        // never bound to a descriptor: immediate destroy is safe.
        if (t) ap_thumbnail_destroy(t);
        return;
    }
    if (lib->thumbs[index]) {
        // the old thumbnail may be grid-bound and sampled by in-flight
        // frames; defer its destruction past the in-flight window.
        ap_thumbnail_retire(lib->thumbs[index]);
    }
    lib->thumbs[index] = t;
}

int ap_library_pending_thumbnail_idx(const ap_library *lib)
{
    if (!lib || !lib->thumbs || lib->cache->photo_count <= 0) return -1;
    ap_library *m = (ap_library *)lib;
    while (m->thumb_cursor < m->cache->photo_count) {
        int idx = m->thumb_cursor++;
        if (!m->thumbs[idx] && !(m->thumb_failed && m->thumb_failed[idx]))
            return idx;
    }
    return -1;
}

void ap_library_invalidate_thumbnail(ap_library *lib, int index)
{
    if (!lib || !lib->thumbs || index < 0 || index >= lib->cache->photo_count) return;
    if (lib->thumbs[index]) {
        // up to APERTURE_FRAMES_IN_FLIGHT submitted frames may still sample
        // the view through the grid's descriptor array; the retire list
        // keeps it alive past that window (#587).
        ap_thumbnail_retire(lib->thumbs[index]);
        lib->thumbs[index] = NULL;
    }
    if (lib->thumb_failed) {
        lib->thumb_failed[index] = false;
    }
    // Rewind the cursor so the per-frame pump revisits this slot.
    if (lib->thumb_cursor > index) lib->thumb_cursor = index;
}

void ap_library_mark_thumbnail_failed(ap_library *lib, int index)
{
    if (!lib || !lib->thumb_failed || index < 0 || index >= lib->cache->photo_count)
        return;
    lib->thumb_failed[index] = true;
}

int ap_library_thumbnail_blob(const ap_library *lib, int index,
                              unsigned char **out_jpeg, size_t *out_size)
{
    if (!lib || !out_jpeg || !out_size) return -1;
    if (index < 0 || index >= lib->cache->photo_count) return -1;

    // Freshness gate: the render must be at least as new as the
    // photo's edit sidecar. No sidecar -> nothing to be fresh
    // against -> fall back to the embedded preview.
    char abs[4096];
    if (ap_library_photo_absolute_path(lib, index, abs, sizeof(abs)) != 0) {
        return -1;
    }
    char sidecar[4096];
    int n = snprintf(sidecar, sizeof(sidecar), "%s.aperture", abs);
    if (n < 0 || (size_t)n >= sizeof(sidecar)) return -1;
    struct stat side_st;
    if (stat(sidecar, &side_st) != 0) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(lib->db,
            "SELECT jpeg, updated_at FROM thumbnails WHERE path = ?;",
            -1, &stmt, NULL) != SQLITE_OK) {
        AP_ERROR("library: prepare thumb select: %s", sqlite3_errmsg(lib->db));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, lib->cache->photo_paths[index], -1, SQLITE_STATIC);

    int rc = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t updated_at = sqlite3_column_int64(stmt, 1);
        if (updated_at >= (int64_t)side_st.st_mtime) {
            const void *blob = sqlite3_column_blob(stmt, 0);
            int          len = sqlite3_column_bytes(stmt, 0);
            if (blob && len > 0) {
                unsigned char *copy = malloc((size_t)len);
                if (copy) {
                    memcpy(copy, blob, (size_t)len);
                    *out_jpeg = copy;
                    *out_size = (size_t)len;
                    rc = 0;
                }
            }
        }
    }
    sqlite3_finalize(stmt);
    return rc;
}

int ap_library_store_thumbnail(ap_library *lib, int index,
                               const unsigned char *jpeg, size_t size)
{
    if (!lib || !jpeg || size == 0) return -1;
    if (index < 0 || index >= lib->cache->photo_count) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(lib->db,
            "INSERT INTO thumbnails(path, jpeg, updated_at) VALUES (?, ?, ?) "
            "ON CONFLICT(path) DO UPDATE SET "
            "    jpeg = excluded.jpeg, updated_at = excluded.updated_at;",
            -1, &stmt, NULL) != SQLITE_OK) {
        AP_ERROR("library: prepare thumb upsert: %s", sqlite3_errmsg(lib->db));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, lib->cache->photo_paths[index], -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 2, jpeg, (int)size, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, (int64_t)time(NULL));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        AP_ERROR("library: thumb upsert step: %s", sqlite3_errstr(rc));
        return -1;
    }
    return 0;
}

// Seed the stack with the registry default pipeline. Mirrors what
// photo.c does on photo-open without a sidecar. Used so bulk metadata
// writes don't strip a photo's edits when the sidecar didn't exist
// before the write.
static void seed_default_stack(ap_edit_stack *stack)
{
    ap_pipeline_apply_default_to_stack(stack);
}

int ap_library_apply_stack_to_path(const char *path,
                                   const ap_edit_stack *stack,
                                   const ap_sidecar_ancillary *prefetched)
{
    if (!path || !stack) return -1;

    ap_sidecar_ancillary local;
    const ap_sidecar_ancillary *a = prefetched;
    if (!a) {
        ap_edit_stack existing_stack;
        ap_edit_stack_init(&existing_stack);
        ap_sidecar_ancillary_clear(&local);
        ap_sidecar_load(path, &existing_stack, &local.respect_orientation,
                        &local.user_meta, local.user_set, &local.culling,
                        &local.groups, &local.keywords);
        a = &local;
    }

    return ap_sidecar_save(path, stack, a->respect_orientation,
                           &a->user_meta, a->user_set, &a->culling,
                           &a->groups, &a->keywords);
}

int ap_library_apply_metadata_patch_to_path(
    const char *path, const ap_photo_metadata *patch,
    const bool patch_set[AP_META_FIELD_COUNT])
{
    if (!path || !patch || !patch_set) return -1;

    ap_edit_stack stack;
    ap_edit_stack_init(&stack);
    bool respect_orientation = true;
    ap_photo_metadata user_meta;
    ap_photo_metadata_clear(&user_meta);
    bool user_set[AP_META_FIELD_COUNT] = {0};

    ap_photo_culling culling;
    ap_photo_culling_clear(&culling);
    ap_photo_groups groups;
    groups.count = 0;
    ap_photo_keywords keywords;
    ap_photo_keywords_clear(&keywords);
    bool had_sidecar = (ap_sidecar_load(path, &stack, &respect_orientation,
                                        &user_meta, user_set, &culling,
                                        &groups, &keywords) == 0);
    if (!had_sidecar) seed_default_stack(&stack);

    for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
        if (!patch_set[i]) continue;
        ap_photo_metadata_set(&user_meta, (ap_meta_field)i,
                              ap_photo_metadata_get(patch, (ap_meta_field)i));
        user_set[i] = true;
    }

    return ap_sidecar_save(path, &stack, respect_orientation,
                           &user_meta, user_set, &culling, &groups, &keywords);
}

int ap_library_apply_pipeline_to_photo(ap_library *lib, int index,
                                       int64_t pipeline_id)
{
    if (!lib || index < 0 || index >= lib->cache->photo_count) return -1;

    ap_edit_stack new_stack;
    if (ap_pipeline_apply_to_stack(pipeline_id, &new_stack) != 0) return -1;

    char path[4096];
    if (ap_library_photo_absolute_path(lib, index, path, sizeof(path)) != 0) {
        return -1;
    }
    return ap_library_apply_stack_to_path(path, &new_stack, NULL);
}

int ap_library_apply_stack_to_photo(ap_library *lib, int index,
                                    const ap_edit_stack *stack,
                                    const ap_sidecar_ancillary *prefetched)
{
    if (!lib || !stack || index < 0 || index >= lib->cache->photo_count) return -1;

    char path[4096];
    if (ap_library_photo_absolute_path(lib, index, path, sizeof(path)) != 0) {
        return -1;
    }
    return ap_library_apply_stack_to_path(path, stack, prefetched);
}

int ap_library_apply_metadata_patch(ap_library *lib, int index,
                                    const ap_photo_metadata *patch,
                                    const bool patch_set[AP_META_FIELD_COUNT])
{
    if (!lib || !patch || !patch_set) return -1;
    if (index < 0 || index >= lib->cache->photo_count) return -1;

    char path[4096];
    if (ap_library_photo_absolute_path(lib, index, path, sizeof(path)) != 0) {
        return -1;
    }
    return ap_library_apply_metadata_patch_to_path(path, patch, patch_set);
}

// Load-modify-save the n-th photo's sidecar so its on-disk `groups`
// match the in-memory index. Mirrors apply_metadata_patch: preserves
// the edit stack, orientation and metadata, seeding the default
// pipeline when the photo has no sidecar yet.
static int write_photo_sidecar_groups(ap_library *lib, int index)
{
    char path[4096];
    if (ap_library_photo_absolute_path(lib, index, path, sizeof(path)) != 0) {
        return -1;
    }
    ap_edit_stack stack;
    ap_edit_stack_init(&stack);
    bool respect_orientation = true;
    ap_photo_metadata user_meta;
    ap_photo_metadata_clear(&user_meta);
    bool user_set[AP_META_FIELD_COUNT] = {0};
    ap_photo_culling culling;
    ap_photo_culling_clear(&culling);
    ap_photo_groups discard_groups;
    discard_groups.count = 0;
    ap_photo_keywords keywords;
    ap_photo_keywords_clear(&keywords);

    bool had = (ap_sidecar_load(path, &stack, &respect_orientation,
                                &user_meta, user_set, &culling,
                                &discard_groups, &keywords) == 0);
    if (!had) seed_default_stack(&stack);

    return ap_sidecar_save(path, &stack, respect_orientation,
                           &user_meta, user_set, &culling,
                           &lib->cache->photo_groups[index], &keywords);
}

int ap_library_write_culling_to_path(const char *path, ap_photo_culling culling)
{
    if (!path) return -1;
    culling.rating = ap_rating_clamp(culling.rating);

    // Load-modify-save: preserve the edit stack + every other ancillary
    // field. ap_sidecar_load_full clears both outputs even on failure, so
    // for a photo with no sidecar yet we only seed the default pipeline
    // (so the write doesn't strip its edits).
    ap_edit_stack stack;
    ap_sidecar_ancillary anc;
    if (ap_sidecar_load_full(path, &stack, &anc) != 0) {
        seed_default_stack(&stack);
    }
    anc.culling = culling;
    return ap_library_apply_stack_to_path(path, &stack, &anc);
}

int ap_library_modify_group_in_sidecar(const char *path, const char *group,
                                       bool member)
{
    if (!path || !group || !*group) return -1;

    ap_edit_stack stack;
    ap_sidecar_ancillary anc;
    if (ap_sidecar_load_full(path, &stack, &anc) != 0) {
        seed_default_stack(&stack);  // load_full already cleared anc
    }

    ap_photo_groups *g = &anc.groups;
    int found = -1;
    for (int i = 0; i < g->count; i++) {
        if (strcmp(g->names[i], group) == 0) { found = i; break; }
    }
    if (member) {
        if (found >= 0) return 0;                 // already a member
        if (g->count >= AP_GROUPS_MAX) return -1;
        snprintf(g->names[g->count], AP_GROUP_NAME_LEN, "%s", group);
        g->count++;
    } else {
        if (found < 0) return 0;                  // not a member
        for (int i = found; i + 1 < g->count; i++) {
            memcpy(g->names[i], g->names[i + 1], AP_GROUP_NAME_LEN);
        }
        g->count--;
    }
    return ap_library_apply_stack_to_path(path, &stack, &anc);
}

void ap_library_commit_culling_batch(ap_library *lib, const int *indices,
                                     const ap_photo_culling *cull, int count)
{
    if (!lib || !lib->db || !lib->cache || !lib->cache->photo_culling ||
        !indices || !cull) {
        return;
    }
    // The worker already wrote every sidecar; here we update only the
    // in-memory cells + the cached db columns, in one transaction so the
    // WAL takes a single commit rather than one per photo.
    sqlite3_exec(lib->db, "BEGIN;", NULL, NULL, NULL);
    for (int k = 0; k < count; k++) {
        int idx = indices[k];
        if (idx < 0 || idx >= lib->cache->photo_count) continue;
        ap_photo_culling c = cull[k];
        c.rating = ap_rating_clamp(c.rating);
        lib->cache->photo_culling[idx] = c;
        store_culling_row(lib->db, lib->cache->photo_paths[idx], &c);
    }
    sqlite3_exec(lib->db, "COMMIT;", NULL, NULL, NULL);
}

void ap_library_apply_group_cache(ap_library *lib, int index,
                                  const char *group, bool member)
{
    if (!lib || !lib->cache || !lib->cache->photo_groups || !group || !*group) {
        return;
    }
    if (index < 0 || index >= lib->cache->photo_count) return;

    ap_photo_groups *g = &lib->cache->photo_groups[index];
    int found = -1;
    for (int i = 0; i < g->count; i++) {
        if (strcmp(g->names[i], group) == 0) { found = i; break; }
    }
    if (member) {
        if (found >= 0) return;
        if (g->count >= AP_GROUPS_MAX) return;
        snprintf(g->names[g->count], AP_GROUP_NAME_LEN, "%s", group);
        g->count++;
        cache_registry_add(lib->db, lib->cache, group);
    } else {
        if (found < 0) return;
        for (int i = found; i + 1 < g->count; i++) {
            memcpy(g->names[i], g->names[i + 1], AP_GROUP_NAME_LEN);
        }
        g->count--;
    }
}

ap_photo_culling ap_library_photo_culling(const ap_library *lib, int index)
{
    if (!lib || !lib->cache->photo_culling ||
        index < 0 || index >= lib->cache->photo_count) {
        ap_photo_culling empty;
        ap_photo_culling_clear(&empty);
        return empty;
    }
    return lib->cache->photo_culling[index];
}

int ap_library_set_photo_culling(ap_library *lib, int index,
                                 ap_photo_culling culling)
{
    if (!lib || !lib->cache->photo_culling) return -1;
    if (index < 0 || index >= lib->cache->photo_count) return -1;

    culling.rating = ap_rating_clamp(culling.rating);

    char path[4096];
    if (ap_library_photo_absolute_path(lib, index, path, sizeof(path)) != 0) {
        return -1;
    }
    if (ap_library_write_culling_to_path(path, culling) != 0) {
        return -1;
    }
    lib->cache->photo_culling[index] = culling;
    store_culling_row(lib->db, lib->cache->photo_paths[index], &culling);
    return 0;
}

const ap_photo_groups *ap_library_photo_groups(const ap_library *lib,
                                               int index)
{
    if (!lib || !lib->cache->photo_groups ||
        index < 0 || index >= lib->cache->photo_count) {
        return NULL;
    }
    return &lib->cache->photo_groups[index];
}

int ap_library_group_list(const ap_library *lib,
                          char names[][AP_GROUP_NAME_LEN], int max)
{
    if (!lib || !names || max <= 0) {
        return 0;
    }
    int n = (lib->cache->group_count < max) ? lib->cache->group_count : max;
    for (int i = 0; i < n; i++) {
        snprintf(names[i], AP_GROUP_NAME_LEN, "%s", lib->cache->group_names[i]);
    }
    return n;
}

int ap_library_group_create(ap_library *lib, const char *name)
{
    if (!lib || !name || !*name) return -1;
    cache_registry_add(lib->db, lib->cache, name);
    return 0;
}

int ap_library_set_photo_group(ap_library *lib, int index,
                               const char *group, bool member)
{
    if (!lib || !lib->cache->photo_groups || !group || !*group) return -1;
    if (index < 0 || index >= lib->cache->photo_count) return -1;

    // No-op when already in the desired state — skip the sidecar rewrite.
    ap_photo_groups *g = &lib->cache->photo_groups[index];
    bool present = false;
    for (int i = 0; i < g->count; i++) {
        if (strcmp(g->names[i], group) == 0) { present = true; break; }
    }
    if (member == present) return 0;
    if (member && g->count >= AP_GROUPS_MAX) {
        AP_WARN("library: photo %d is already in the max %d groups",
                index, AP_GROUPS_MAX);
        return -1;
    }

    ap_library_apply_group_cache(lib, index, group, member);
    return write_photo_sidecar_groups(lib, index);
}

int ap_library_rename_group(ap_library *lib, const char *old_name,
                            const char *new_name)
{
    if (!lib || !old_name || !*old_name || !new_name || !*new_name) {
        return -1;
    }
    if (strcmp(old_name, new_name) == 0) return 0;

    for (int p = 0; p < lib->cache->photo_count; p++) {
        const ap_photo_groups *g = &lib->cache->photo_groups[p];
        bool has = false;
        for (int i = 0; i < g->count; i++) {
            if (strcmp(g->names[i], old_name) == 0) {
                has = true;
                break;
            }
        }
        if (has) {
            ap_library_set_photo_group(lib, p, old_name, false);
            ap_library_set_photo_group(lib, p, new_name, true);
        }
    }
    cache_registry_remove(lib->db, lib->cache, old_name);
    cache_registry_add(lib->db, lib->cache, new_name);
    return 0;
}

int ap_library_delete_group(ap_library *lib, const char *group)
{
    if (!lib || !group || !*group) return -1;
    for (int p = 0; p < lib->cache->photo_count; p++) {
        ap_library_set_photo_group(lib, p, group, false);
    }
    cache_registry_remove(lib->db, lib->cache, group);
    return 0;
}

// Serialise `s` into a NUL-terminated key=value text block. Each line
// is "key=value\n". Caller owns the returned buffer (free it).
static char *preset_to_blob(const ap_export_settings *s)
{
    ap_memstream *ms = ap_memstream_open();
    if (!ms) return NULL;
    FILE *mf = ap_memstream_file(ms);
    fprintf(mf, "format=%d\n",       s->format);
    fprintf(mf, "jpeg_quality=%d\n", s->jpeg_quality);
    fprintf(mf, "png_depth=%d\n",    s->png_depth);
    fprintf(mf, "tiff_depth=%d\n",   s->tiff_depth);
    fprintf(mf, "tiff_compress=%d\n",s->tiff_compress);
    fprintf(mf, "naming=%d\n",       s->naming);
    fprintf(mf, "pattern=%s\n",      s->pattern);
    fprintf(mf, "destination=%d\n",  s->destination);
    fprintf(mf, "dest_subdir=%s\n",  s->dest_subdir);
    fprintf(mf, "dest_dir=%s\n",     s->dest_dir);
    fprintf(mf, "collision=%d\n",    s->collision);
    char  *buf = NULL;
    size_t len = 0;
    if (ap_memstream_close(ms, &buf, &len) != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

// Parse a preset blob (as written by preset_to_blob) into `out`.
static void preset_from_blob(const char *blob, ap_export_settings *out)
{
    if (!blob || !out) return;
    const char *p = blob;
    while (*p) {
        const char *eq  = strchr(p, '=');
        const char *nl  = strchr(p, '\n');
        if (!eq) break;
        const char *end = nl ? nl : p + strlen(p);
        size_t klen = (size_t)(eq - p);
        size_t vlen = (size_t)(end - eq - 1);
        char key[64];
        char val[AP_EXPORT_DEST_LEN];
        if (klen < sizeof(key) && vlen < sizeof(val)) {
            memcpy(key, p, klen); key[klen] = '\0';
            memcpy(val, eq + 1, vlen); val[vlen] = '\0';
            if      (strcmp(key, "format")       == 0) out->format        = atoi(val);
            else if (strcmp(key, "jpeg_quality") == 0) out->jpeg_quality  = atoi(val);
            else if (strcmp(key, "png_depth")    == 0) out->png_depth     = atoi(val);
            else if (strcmp(key, "tiff_depth")   == 0) out->tiff_depth    = atoi(val);
            else if (strcmp(key, "tiff_compress")== 0) out->tiff_compress = atoi(val);
            else if (strcmp(key, "naming")       == 0) out->naming        = atoi(val);
            else if (strcmp(key, "pattern")      == 0) snprintf(out->pattern,     sizeof(out->pattern),     "%.*s", (int)(sizeof(out->pattern) - 1), val);
            else if (strcmp(key, "destination")  == 0) out->destination   = atoi(val);
            else if (strcmp(key, "dest_subdir")  == 0) snprintf(out->dest_subdir, sizeof(out->dest_subdir), "%s", val);
            else if (strcmp(key, "dest_dir")     == 0) snprintf(out->dest_dir,    sizeof(out->dest_dir),    "%s", val);
            else if (strcmp(key, "collision")    == 0) out->collision     = atoi(val);
        }
        p = nl ? nl + 1 : end;
    }
}

int ap_export_preset_save(ap_library *lib, const char *name,
                          const ap_export_settings *s)
{
    if (!lib || !lib->db || !name || !*name || !s) return -1;
    char *blob = preset_to_blob(s);
    if (!blob) return -1;

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(lib->db,
        "INSERT INTO export_presets(name, settings_blob) VALUES (?, ?) "
        "ON CONFLICT(name) DO UPDATE SET settings_blob = excluded.settings_blob;",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        AP_ERROR("export_preset_save: prepare: %s", sqlite3_errmsg(lib->db));
        free(blob);
        return -1;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, blob, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    free(blob);
    if (rc != SQLITE_DONE) {
        AP_ERROR("export_preset_save: step: %s", sqlite3_errstr(rc));
        return -1;
    }
    return 0;
}

int ap_export_preset_list(const ap_library *lib,
                          ap_export_preset *out, int max)
{
    if (!lib || !lib->db || !out || max <= 0) return -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(lib->db,
            "SELECT id, name, settings_blob FROM export_presets ORDER BY name LIMIT ?;",
            -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int(st, 1, max);
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < max) {
        out[n].id = sqlite3_column_int64(st, 0);
        const char *nm = (const char *)sqlite3_column_text(st, 1);
        snprintf(out[n].name, sizeof(out[n].name), "%s", nm ? nm : "");
        ap_export_settings_load(NULL, &out[n].settings);
        const char *bl = (const char *)sqlite3_column_text(st, 2);
        if (bl) preset_from_blob(bl, &out[n].settings);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int ap_export_preset_load(const ap_library *lib, int64_t id,
                          ap_export_preset *out)
{
    if (!lib || !lib->db || !out) return -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(lib->db,
            "SELECT id, name, settings_blob FROM export_presets WHERE id = ?;",
            -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, id);
    int rc = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out->id = sqlite3_column_int64(st, 0);
        const char *nm = (const char *)sqlite3_column_text(st, 1);
        snprintf(out->name, sizeof(out->name), "%s", nm ? nm : "");
        ap_export_settings_load(NULL, &out->settings);
        const char *bl = (const char *)sqlite3_column_text(st, 2);
        if (bl) preset_from_blob(bl, &out->settings);
        rc = 0;
    }
    sqlite3_finalize(st);
    return rc;
}

int ap_export_preset_delete(ap_library *lib, int64_t id)
{
    if (!lib || !lib->db) return -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(lib->db,
            "DELETE FROM export_presets WHERE id = ?;",
            -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}
