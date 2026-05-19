#define _GNU_SOURCE

#include "library.h"

#include "app/root.h"
#include "core/log.h"
#include "edit/stack.h"
#include "modules/module.h"
#include "photo/thumbnail.h"
#include "sidecar/sidecar.h"

#include <sqlite3.h>

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define APERTURE_DB_VERSION "1"

#ifndef APERTURE_VERSION
#error "APERTURE_VERSION must be defined at compile time (set via meson)"
#endif

static const char *RAW_EXTENSIONS[] = {
    ".nef", ".cr2", ".cr3", ".raf", ".arw",
    ".dng", ".orf", ".rw2", ".pef", ".srw",
    NULL,
};

struct ap_library {
    char     *root;        // absolute path to the photo directory
    char      id[37];      // RFC 4122 v4 UUID, 36 chars + NUL
    char      name[128];   // user-set display name; empty if unset
    sqlite3  *db;          // <app_root>/libraries/<id>.db

    char    **photo_paths; // relative to root
    int       photo_count;
    int       photo_capacity;

    ap_thumbnail **thumbs; // photo_count entries, NULL = not loaded
    int            thumb_cursor; // next idx to try; rolled forward
};

// ----- registry: <app_root>/aperture.db, libraries(id, path, created_at) -----

static const char *REGISTRY_SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS libraries ("
    "    id         TEXT PRIMARY KEY,"
    "    path       TEXT NOT NULL UNIQUE,"
    "    name       TEXT,"
    "    created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS pipelines ("
    "    id      INTEGER PRIMARY KEY,"
    "    name    TEXT NOT NULL UNIQUE,"
    "    modules TEXT NOT NULL"
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

// Comma-separated module names - order matters; resolved via
// ap_module_find when a photo opens. Kept as a single TEXT column
// rather than a join table for v1 simplicity.
static const char *DEFAULT_PIPELINE_NAME    = "default";
// Baseline edits for a fresh photo. Output Transfer is auto-appended
// by the pipeline graph; everything else is on the user-facing stack.
static const char *DEFAULT_PIPELINE_MODULES = "demosaic,wb,profile";

static int gen_uuid_v4(char buf[37])
{
    uint8_t b[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) {
        AP_ERROR("gen_uuid_v4: cannot open /dev/urandom: %s", strerror(errno));
        return -1;
    }
    if (fread(b, 1, 16, f) != 16) {
        AP_ERROR("gen_uuid_v4: short read from /dev/urandom");
        fclose(f);
        return -1;
    }
    fclose(f);

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

static int seed_default_pipeline(sqlite3 *reg)
{
    // Always overwrite the default row. The default reflects the
    // current build's baseline edits; user-defined pipelines (when we
    // add them) will live under different names.
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(reg,
            "INSERT INTO pipelines(name, modules) VALUES (?, ?) "
            "ON CONFLICT(name) DO UPDATE SET modules = excluded.modules;",
            -1, &stmt, NULL) != SQLITE_OK) {
        AP_ERROR("registry: prepare seed: %s", sqlite3_errmsg(reg));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, DEFAULT_PIPELINE_NAME,    -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, DEFAULT_PIPELINE_MODULES, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
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

static int parse_module_list(const char *src, ap_pipeline_def *out)
{
    out->module_count = 0;
    int slot = 0;
    int col  = 0;
    for (const char *p = src; ; p++) {
        if (*p == ',' || *p == '\0') {
            if (slot >= AP_PIPELINE_MAX_MODULES) {
                AP_ERROR("pipeline: too many modules (max %d)",
                         AP_PIPELINE_MAX_MODULES);
                return -1;
            }
            if (col >= AP_PIPELINE_MODULE_LEN) {
                AP_ERROR("pipeline: module name too long (max %d)",
                         AP_PIPELINE_MODULE_LEN - 1);
                return -1;
            }
            out->modules[slot][col] = '\0';
            if (col > 0) slot++;
            col = 0;
            if (*p == '\0') break;
        } else {
            if (col + 1 >= AP_PIPELINE_MODULE_LEN) {
                AP_ERROR("pipeline: module name too long (max %d)",
                         AP_PIPELINE_MODULE_LEN - 1);
                return -1;
            }
            out->modules[slot][col++] = *p;
        }
    }
    out->module_count = slot;
    return 0;
}

int ap_pipeline_get_default(ap_pipeline_def *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    sqlite3 *reg = NULL;
    if (registry_open(&reg) < 0) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(reg,
            "SELECT id, name, modules FROM pipelines WHERE name = ?;",
            -1, &stmt, NULL) != SQLITE_OK) {
        AP_ERROR("pipeline: prepare select: %s", sqlite3_errmsg(reg));
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
    out->id = sqlite3_column_int64(stmt, 0);
    const char *name    = (const char *)sqlite3_column_text(stmt, 1);
    const char *modules = (const char *)sqlite3_column_text(stmt, 2);
    snprintf(out->name, sizeof(out->name), "%s", name ? name : "");
    int parse_rc = modules ? parse_module_list(modules, out) : -1;
    sqlite3_finalize(stmt);
    sqlite3_close(reg);
    return parse_rc;
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

// ----- per-library db: photos table -----

static const char *LIBRARY_SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS schema ("
    "    key   TEXT PRIMARY KEY,"
    "    value TEXT NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS photos ("
    "    id           INTEGER PRIMARY KEY,"
    "    path         TEXT NOT NULL UNIQUE,"
    "    hash         BLOB,"
    "    capture_time INTEGER,"
    "    added_at     INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_photos_capture_time ON photos(capture_time);"
    // Edit-render thumbnail cache. Keyed by the photo's relative path
    // (same key the in-memory photo list uses). `jpeg` is a small
    // JPEG of the photo rendered through its edit stack; `updated_at`
    // is when it was rendered, compared against the .aperture
    // sidecar's mtime to decide freshness.
    "CREATE TABLE IF NOT EXISTS thumbnails ("
    "    path       TEXT PRIMARY KEY,"
    "    jpeg       BLOB NOT NULL,"
    "    updated_at INTEGER NOT NULL"
    ");";

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
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (int i = 0; RAW_EXTENSIONS[i]; i++) {
        if (strcasecmp(dot, RAW_EXTENSIONS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static int append_path(ap_library *lib, const char *rel)
{
    if (lib->photo_count == lib->photo_capacity) {
        int new_cap = lib->photo_capacity ? lib->photo_capacity * 2 : 64;
        char **np = realloc(lib->photo_paths, (size_t)new_cap * sizeof(*np));
        if (!np) {
            AP_ERROR("library: photo list grow failed");
            return -1;
        }
        lib->photo_paths    = np;
        lib->photo_capacity = new_cap;
    }
    char *dup = strdup(rel);
    if (!dup) {
        AP_ERROR("library: path dup failed");
        return -1;
    }
    lib->photo_paths[lib->photo_count++] = dup;
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

static int scan_dir(ap_library *lib, sqlite3_stmt *insert_stmt,
                    const char *abs_dir, const char *rel_prefix,
                    int64_t added_at)
{
    DIR *d = opendir(abs_dir);
    if (!d) {
        AP_WARN("library: opendir(%s): %s", abs_dir, strerror(errno));
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char abs_child[4096];
        if (snprintf(abs_child, sizeof(abs_child), "%s/%s",
                     abs_dir, ent->d_name) >= (int)sizeof(abs_child)) {
            AP_WARN("library: path too long, skipping %s/%s", abs_dir, ent->d_name);
            continue;
        }

        struct stat st;
        if (stat(abs_child, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            char child_rel[2048];
            if (rel_prefix[0] == '\0') {
                snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
            } else {
                snprintf(child_rel, sizeof(child_rel), "%s/%s",
                         rel_prefix, ent->d_name);
            }
            scan_dir(lib, insert_stmt, abs_child, child_rel, added_at);
            continue;
        }

        if (!S_ISREG(st.st_mode)) continue;
        if (!is_raw_file(ent->d_name)) continue;

        char rel[2048];
        if (rel_prefix[0] == '\0') {
            snprintf(rel, sizeof(rel), "%s", ent->d_name);
        } else {
            snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, ent->d_name);
        }
        insert_photo(lib->db, insert_stmt, rel, added_at);
    }

    closedir(d);
    return 0;
}

static int load_photo_cache(ap_library *lib)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(lib->db,
        "SELECT path FROM photos ORDER BY path;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        AP_ERROR("library: prepare photo list: %s", sqlite3_errmsg(lib->db));
        return -1;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(stmt, 0);
        if (!path) continue;
        if (append_path(lib, path) < 0) {
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

    char db_filename[64];
    snprintf(db_filename, sizeof(db_filename), "libraries/%s.db", lib->id);
    char db_path[4096];
    if (ap_app_root_join(db_filename, db_path, sizeof(db_path)) < 0) {
        AP_ERROR("ap_library_open: db path too long");
        goto fail;
    }

    if (sqlite3_open(db_path, &lib->db) != SQLITE_OK) {
        AP_ERROR("ap_library_open: sqlite3_open(%s): %s",
                 db_path, sqlite3_errmsg(lib->db));
        goto fail;
    }
    sqlite3_exec(lib->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "PRAGMA foreign_keys=ON;",  NULL, NULL, NULL);

    if (exec_simple(lib->db, LIBRARY_SCHEMA_SQL)                       < 0) goto fail;
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
    scan_dir(lib, insert_stmt, lib->root, "", now);
    sqlite3_exec(lib->db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_finalize(insert_stmt);

    if (load_photo_cache(lib) < 0) goto fail;

    if (lib->photo_count > 0) {
        lib->thumbs = calloc((size_t)lib->photo_count, sizeof(*lib->thumbs));
        if (!lib->thumbs) {
            AP_ERROR("library: thumbnail cache alloc failed");
            goto fail;
        }
    }

    AP_INFO("library: %s [%s] (%d photos)",
            lib->root, lib->id, lib->photo_count);
    return lib;

fail:
    ap_library_close(lib);
    return NULL;
}

void ap_library_close(ap_library *lib)
{
    if (!lib) return;
    if (lib->thumbs) {
        for (int i = 0; i < lib->photo_count; i++) {
            ap_thumbnail_destroy(lib->thumbs[i]);
        }
        free(lib->thumbs);
    }
    for (int i = 0; i < lib->photo_count; i++) {
        free(lib->photo_paths[i]);
    }
    free(lib->photo_paths);
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

int ap_library_photo_count(const ap_library *lib)
{
    return lib ? lib->photo_count : 0;
}

const char *ap_library_photo_relative_path(const ap_library *lib, int index)
{
    if (!lib || index < 0 || index >= lib->photo_count) return NULL;
    return lib->photo_paths[index];
}

int ap_library_photo_absolute_path(const ap_library *lib, int index,
                                   char *buf, size_t buflen)
{
    if (!lib || !buf || index < 0 || index >= lib->photo_count) {
        return -1;
    }
    int n = snprintf(buf, buflen, "%s/%s", lib->root, lib->photo_paths[index]);
    if (n < 0 || (size_t)n >= buflen) {
        return -1;
    }
    return 0;
}

ap_thumbnail *ap_library_thumbnail(const ap_library *lib, int index)
{
    if (!lib || !lib->thumbs || index < 0 || index >= lib->photo_count) {
        return NULL;
    }
    return lib->thumbs[index];
}

void ap_library_set_thumbnail(ap_library *lib, int index, ap_thumbnail *t)
{
    if (!lib || !lib->thumbs || index < 0 || index >= lib->photo_count) {
        if (t) ap_thumbnail_destroy(t);
        return;
    }
    if (lib->thumbs[index]) {
        ap_thumbnail_destroy(lib->thumbs[index]);
    }
    lib->thumbs[index] = t;
}

int ap_library_pending_thumbnail_idx(const ap_library *lib)
{
    if (!lib || !lib->thumbs || lib->photo_count <= 0) return -1;
    ap_library *m = (ap_library *)lib;
    while (m->thumb_cursor < m->photo_count) {
        int idx = m->thumb_cursor++;
        if (!m->thumbs[idx]) return idx;
    }
    return -1;
}

void ap_library_invalidate_thumbnail(ap_library *lib, int index)
{
    if (!lib || !lib->thumbs || index < 0 || index >= lib->photo_count) return;
    if (lib->thumbs[index]) {
        ap_thumbnail_destroy(lib->thumbs[index]);
        lib->thumbs[index] = NULL;
    }
    // Rewind the cursor so the per-frame pump revisits this slot.
    if (lib->thumb_cursor > index) lib->thumb_cursor = index;
}

int ap_library_thumbnail_blob(const ap_library *lib, int index,
                              unsigned char **out_jpeg, size_t *out_size)
{
    if (!lib || !out_jpeg || !out_size) return -1;
    if (index < 0 || index >= lib->photo_count) return -1;

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
    sqlite3_bind_text(stmt, 1, lib->photo_paths[index], -1, SQLITE_STATIC);

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
    if (index < 0 || index >= lib->photo_count) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(lib->db,
            "INSERT INTO thumbnails(path, jpeg, updated_at) VALUES (?, ?, ?) "
            "ON CONFLICT(path) DO UPDATE SET "
            "    jpeg = excluded.jpeg, updated_at = excluded.updated_at;",
            -1, &stmt, NULL) != SQLITE_OK) {
        AP_ERROR("library: prepare thumb upsert: %s", sqlite3_errmsg(lib->db));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, lib->photo_paths[index], -1, SQLITE_STATIC);
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

// Seed the stack with the registry default pipeline's user-visible
// modules. Mirrors what photo.c does when opening a photo that has
// no sidecar. Used so bulk metadata writes don't strip a photo's
// edits when the sidecar didn't exist before the write.
static void seed_default_stack(ap_edit_stack *stack)
{
    ap_pipeline_def def;
    if (ap_pipeline_get_default(&def) != 0) return;
    for (int i = 0; i < def.module_count; i++) {
        const ap_module *m = ap_module_find(def.modules[i]);
        if (!m || !m->user_visible) continue;
        ap_edit_stack_add(stack, def.modules[i]);
    }
}

int ap_library_apply_metadata_patch(ap_library *lib, int index,
                                    const ap_photo_metadata *patch,
                                    const bool patch_set[AP_META_FIELD_COUNT])
{
    if (!lib || !patch || !patch_set) return -1;
    if (index < 0 || index >= lib->photo_count) return -1;

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

    bool had_sidecar = (ap_sidecar_load(path, &stack, &respect_orientation,
                                        &user_meta, user_set) == 0);
    if (!had_sidecar) seed_default_stack(&stack);

    for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
        if (!patch_set[i]) continue;
        ap_photo_metadata_set(&user_meta, (ap_meta_field)i,
                              ap_photo_metadata_get(patch, (ap_meta_field)i));
        user_set[i] = true;
    }

    return ap_sidecar_save(path, &stack, respect_orientation,
                           &user_meta, user_set);
}
