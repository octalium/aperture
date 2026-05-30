// Library schema + dedupe smoke tests. Opens a tmpdir as a library,
// checks that the schema tables are created and that re-opening with
// the same raw file on disk does not duplicate the row.

#define _GNU_SOURCE

#include "aptest.h"
#include "aptest_tmpdir.h"

#include "library/library.h"

#include <sqlite3.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Library uses ap_app_root_path which is cached on first call. Setting
// XDG_DATA_HOME (Linux) and HOME (macOS fallback) to a per-test tmpdir
// before that first call confines the registry db to the test sandbox.
static void redirect_app_root(const char *root)
{
    setenv("XDG_DATA_HOME", root, 1);
    setenv("HOME", root, 1);
    unsetenv("APPDATA");
}

static void touch_raw(const char *dir, const char *name)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    AP_TEST_ASSERT(f != NULL, "touch %s: %s", path, strerror(errno));
    // Library doesn't decode pixels — any non-empty body is fine. We
    // still write a few bytes so a size-based sanity check would see
    // the file as non-empty.
    fwrite("rawbytes", 1, 8, f);
    fclose(f);
}

// Count `path` rows in the per-library db (which lives at
// <root>/library.db). Used to assert that re-imports don't duplicate.
static int photo_count_in_db(const char *root)
{
    char db_path[4096];
    snprintf(db_path, sizeof(db_path), "%s/library.db", root);
    sqlite3 *db = NULL;
    AP_TEST_ASSERT(sqlite3_open(db_path, &db) == SQLITE_OK,
                   "sqlite3_open(%s): %s", db_path,
                   db ? sqlite3_errmsg(db) : "?");
    sqlite3_stmt *st = NULL;
    AP_TEST_ASSERT(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM photos;",
                                      -1, &st, NULL) == SQLITE_OK,
                   "prepare count: %s", sqlite3_errmsg(db));
    int n = -1;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

// Assert that the named table exists in the per-library db.
static void assert_table_exists(const char *root, const char *table)
{
    char db_path[4096];
    snprintf(db_path, sizeof(db_path), "%s/library.db", root);
    sqlite3 *db = NULL;
    AP_TEST_ASSERT(sqlite3_open(db_path, &db) == SQLITE_OK,
                   "sqlite3_open(%s)", db_path);
    sqlite3_stmt *st = NULL;
    AP_TEST_ASSERT(sqlite3_prepare_v2(db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name=?;",
        -1, &st, NULL) == SQLITE_OK, "prepare master select");
    sqlite3_bind_text(st, 1, table, -1, SQLITE_STATIC);
    bool found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    sqlite3_close(db);
    AP_TEST_ASSERT(found, "expected table '%s' to exist", table);
}

static void test_schema_created(void)
{
    char approot[4096];
    aptest_tmpdir_make(approot, sizeof(approot));
    redirect_app_root(approot);

    char libroot[4096];
    aptest_tmpdir_make(libroot, sizeof(libroot));
    touch_raw(libroot, "a.cr3");

    ap_library *lib = ap_library_open(libroot);
    AP_TEST_ASSERT(lib != NULL, "ap_library_open(%s) returned NULL", libroot);
    AP_TEST_ASSERT(ap_library_photo_count(lib) == 1,
                   "expected 1 photo, got %d", ap_library_photo_count(lib));
    ap_library_close(lib);

    // The set of tables produced by LIBRARY_TABLES_SQL.
    static const char *const TABLES[] = {
        "schema", "photos", "thumbnails", "settings", "groups",
        "export_presets", NULL,
    };
    for (const char *const *t = TABLES; *t; t++) assert_table_exists(libroot, *t);

    aptest_tmpdir_rm(libroot);
    aptest_tmpdir_rm(approot);
}

static void test_dedupe_on_reopen(void)
{
    char approot[4096];
    aptest_tmpdir_make(approot, sizeof(approot));
    redirect_app_root(approot);

    char libroot[4096];
    aptest_tmpdir_make(libroot, sizeof(libroot));
    touch_raw(libroot, "one.cr3");
    touch_raw(libroot, "two.cr3");
    touch_raw(libroot, "three.cr3");

    ap_library *lib = ap_library_open(libroot);
    AP_TEST_ASSERT(lib != NULL, "open 1");
    AP_TEST_ASSERT(ap_library_photo_count(lib) == 3,
                   "first open: count=%d", ap_library_photo_count(lib));
    ap_library_close(lib);
    AP_TEST_ASSERT(photo_count_in_db(libroot) == 3,
                   "db count after first open != 3");

    // Re-open: same files on disk, must not produce duplicate rows.
    lib = ap_library_open(libroot);
    AP_TEST_ASSERT(lib != NULL, "open 2");
    AP_TEST_ASSERT(ap_library_photo_count(lib) == 3,
                   "second open: count=%d", ap_library_photo_count(lib));
    ap_library_close(lib);
    AP_TEST_ASSERT(photo_count_in_db(libroot) == 3,
                   "db count after second open != 3");

    aptest_tmpdir_rm(libroot);
    aptest_tmpdir_rm(approot);
}

static void test_photo_remove(void)
{
    char approot[4096];
    aptest_tmpdir_make(approot, sizeof(approot));
    redirect_app_root(approot);

    char libroot[4096];
    aptest_tmpdir_make(libroot, sizeof(libroot));
    touch_raw(libroot, "keep.cr3");
    touch_raw(libroot, "trash.cr3");

    ap_library *lib = ap_library_open(libroot);
    AP_TEST_ASSERT(lib != NULL, "open");
    AP_TEST_ASSERT(ap_library_photo_count(lib) == 2,
                   "init count=%d", ap_library_photo_count(lib));

    // Find the index of "trash.cr3" and remove it.
    int trash_idx = -1;
    for (int i = 0; i < ap_library_photo_count(lib); i++) {
        const char *rel = ap_library_photo_relative_path(lib, i);
        if (rel && strcmp(rel, "trash.cr3") == 0) { trash_idx = i; break; }
    }
    AP_TEST_ASSERT(trash_idx >= 0, "trash.cr3 not found in library");
    AP_TEST_ASSERT(ap_library_photo_remove(lib, trash_idx) == 0,
                   "photo_remove returned non-zero");
    AP_TEST_ASSERT(ap_library_photo_count(lib) == 1,
                   "post-remove count=%d", ap_library_photo_count(lib));

    // The file should be gone from disk, and the row from the db.
    char abs[4200];
    snprintf(abs, sizeof(abs), "%s/trash.cr3", libroot);
    struct stat st;
    AP_TEST_ASSERT(stat(abs, &st) != 0,
                   "trash.cr3 still on disk after remove");
    ap_library_close(lib);
    AP_TEST_ASSERT(photo_count_in_db(libroot) == 1,
                   "db count after remove != 1");

    aptest_tmpdir_rm(libroot);
    aptest_tmpdir_rm(approot);
}

// Non-raw files in the library root must be ignored. A stray .txt or
// .jpg should not appear in the photo list — confirms the import path's
// extension filter is honored end-to-end.
static void test_non_raw_ignored(void)
{
    char approot[4096];
    aptest_tmpdir_make(approot, sizeof(approot));
    redirect_app_root(approot);

    char libroot[4096];
    aptest_tmpdir_make(libroot, sizeof(libroot));
    touch_raw(libroot, "shot.cr3");
    touch_raw(libroot, "readme.txt");
    touch_raw(libroot, "preview.jpg");

    ap_library *lib = ap_library_open(libroot);
    AP_TEST_ASSERT(lib != NULL, "open");
    AP_TEST_ASSERT(ap_library_photo_count(lib) == 1,
                   "expected only the raw to be imported, count=%d",
                   ap_library_photo_count(lib));
    ap_library_close(lib);

    aptest_tmpdir_rm(libroot);
    aptest_tmpdir_rm(approot);
}

int main(void)
{
    test_schema_created();
    test_dedupe_on_reopen();
    test_photo_remove();
    test_non_raw_ignored();
    printf("library/library: OK\n");
    return 0;
}
