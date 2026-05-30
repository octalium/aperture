#define _GNU_SOURCE

#include "library/import.h"

#include "core/compat.h"
#include "core/dir.h"
#include "core/log.h"
#include "io/exif.h"
#include "io/raw.h"

#include <blake3.h>
#include <sqlite3.h>

#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define BLAKE3_HEX_LEN (BLAKE3_OUT_LEN * 2 + 1)

#define KEY_SUBDIR    "import.subdir"
#define KEY_NAMING    "import.naming"
#define KEY_PATTERN   "import.pattern"
#define KEY_COLLISION "import.collision"
#define KEY_DEDUPE    "import.dedupe_content"
#define KEY_STRICT_ID "import.strict_identity"

void ap_import_settings_load(const ap_library *lib, ap_import_settings *out)
{
    if (!out) return;

    // Defaults.
    snprintf(out->subdir, sizeof(out->subdir), "raw");
    out->naming = AP_IMPORT_NAME_KEEP;
    snprintf(out->pattern, sizeof(out->pattern), "{YYYY}{MM}{DD}_{HH}{MIN}{SEC}");
    out->collision        = AP_IMPORT_COLLIDE_SUFFIX;
    out->dedupe_content   = true;
    out->strict_identity  = false;
    if (!lib) return;

    char buf[AP_IMPORT_PATTERN_LEN];
    char subdir[AP_IMPORT_SUBDIR_LEN];
    if (ap_library_setting_get(lib, KEY_SUBDIR, subdir,
                               sizeof(subdir)) == 0 && subdir[0]) {
        snprintf(out->subdir, sizeof(out->subdir), "%s", subdir);
    }
    if (ap_library_setting_get(lib, KEY_NAMING, buf, sizeof(buf)) == 0) {
        out->naming = (atoi(buf) == AP_IMPORT_NAME_PATTERN)
                          ? AP_IMPORT_NAME_PATTERN : AP_IMPORT_NAME_KEEP;
    }
    if (ap_library_setting_get(lib, KEY_PATTERN, buf, sizeof(buf)) == 0 && buf[0]) {
        snprintf(out->pattern, sizeof(out->pattern), "%s", buf);
    }
    if (ap_library_setting_get(lib, KEY_COLLISION, buf, sizeof(buf)) == 0) {
        int c = atoi(buf);
        out->collision = (c >= AP_IMPORT_COLLIDE_SKIP &&
                          c <= AP_IMPORT_COLLIDE_SUFFIX)
                             ? c : AP_IMPORT_COLLIDE_SUFFIX;
    }
    if (ap_library_setting_get(lib, KEY_DEDUPE, buf, sizeof(buf)) == 0) {
        out->dedupe_content = (atoi(buf) != 0);
    }
    if (ap_library_setting_get(lib, KEY_STRICT_ID, buf, sizeof(buf)) == 0) {
        out->strict_identity = (atoi(buf) != 0);
    }
}

void ap_import_settings_save(ap_library *lib, const ap_import_settings *s)
{
    if (!lib || !s) return;
    char num[16];
    ap_library_setting_set(lib, KEY_SUBDIR, s->subdir);
    snprintf(num, sizeof(num), "%d", s->naming);
    ap_library_setting_set(lib, KEY_NAMING, num);
    ap_library_setting_set(lib, KEY_PATTERN, s->pattern);
    snprintf(num, sizeof(num), "%d", s->collision);
    ap_library_setting_set(lib, KEY_COLLISION, num);
    snprintf(num, sizeof(num), "%d", s->dedupe_content ? 1 : 0);
    ap_library_setting_set(lib, KEY_DEDUPE, num);
    snprintf(num, sizeof(num), "%d", s->strict_identity ? 1 : 0);
    ap_library_setting_set(lib, KEY_STRICT_ID, num);
}

// Worst-case substitution length. {ORIG} expands to the source stem,
// which import_one caps at AP_IMPORT_STEM_LEN bytes; the date/time
// tokens fit trivially. Keep these two in lockstep.
#define AP_IMPORT_REP_LEN AP_IMPORT_STEM_LEN

// Expand the rename pattern. `stem` is the source basename without its
// extension; the extension is appended by the caller.
static void format_name(const char *pattern, const char *stem,
                        time_t when, int seq, char *out, size_t out_len)
{
    struct tm tm;
    localtime_r(&when, &tm);

    size_t o = 0;
    for (const char *p = pattern; *p && o + 1 < out_len; ) {
        if (*p != '{') {
            out[o++] = *p++;
            continue;
        }
        const char *end = strchr(p, '}');
        if (!end) {
            out[o++] = *p++;
            continue;
        }
        char tok[16];
        size_t tlen = (size_t)(end - p - 1);
        char rep[AP_IMPORT_REP_LEN];
        rep[0] = '\0';
        if (tlen < sizeof(tok)) {
            memcpy(tok, p + 1, tlen);
            tok[tlen] = '\0';
            if      (strcmp(tok, "ORIG") == 0) snprintf(rep, sizeof(rep), "%s", stem);
            else if (strcmp(tok, "YYYY") == 0) snprintf(rep, sizeof(rep), "%04d", tm.tm_year + 1900);
            else if (strcmp(tok, "MM")   == 0) snprintf(rep, sizeof(rep), "%02d", tm.tm_mon + 1);
            else if (strcmp(tok, "DD")   == 0) snprintf(rep, sizeof(rep), "%02d", tm.tm_mday);
            else if (strcmp(tok, "HH")   == 0) snprintf(rep, sizeof(rep), "%02d", tm.tm_hour);
            else if (strcmp(tok, "MIN")  == 0) snprintf(rep, sizeof(rep), "%02d", tm.tm_min);
            else if (strcmp(tok, "SEC")  == 0) snprintf(rep, sizeof(rep), "%02d", tm.tm_sec);
            else if (strcmp(tok, "SEQ")  == 0) snprintf(rep, sizeof(rep), "%04d", seq);
            // An unrecognised token expands to nothing.
        }
        for (const char *r = rep; *r && o + 1 < out_len; r++) {
            out[o++] = *r;
        }
        p = end + 1;
    }
    out[o] = '\0';
}

// Encode `len` bytes from `src` as lowercase hex into `dst`, which must
// be at least 2*len+1 bytes long.
static void hex_encode(const uint8_t *src, size_t len, char *dst)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        dst[2 * i]     = hx[src[i] >> 4];
        dst[2 * i + 1] = hx[src[i] & 0xf];
    }
    dst[2 * len] = '\0';
}

// Copy `src` to `dst`, streaming through BLAKE3 as data flows. On
// success writes the lowercase hex digest into `out_hex` (must be at
// least BLAKE3_HEX_LEN bytes) and returns 0. On error unlinks `dst`
// and returns -1.
static int copy_file_hash(const char *src, const char *dst,
                          char out_hex[BLAKE3_HEX_LEN])
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        AP_WARN("import: open %s: %s", src, strerror(errno));
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        AP_WARN("import: create %s: %s", dst, strerror(errno));
        fclose(in);
        return -1;
    }

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);

    char   buf[1 << 16];
    size_t n;
    int    rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        blake3_hasher_update(&hasher, buf, n);
        if (fwrite(buf, 1, n, out) != n) {
            AP_ERROR("import: write %s: %s", dst, strerror(errno));
            rc = -1;
            break;
        }
    }
    if (rc == 0 && ferror(in)) {
        AP_ERROR("import: read %s", src);
        rc = -1;
    }
    fclose(in);
    if (fclose(out) != 0) {
        AP_ERROR("import: close %s: %s", dst, strerror(errno));
        rc = -1;
    }
    if (rc != 0) {
        unlink(dst);
        return rc;
    }

    uint8_t digest[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&hasher, digest, BLAKE3_OUT_LEN);
    hex_encode(digest, BLAKE3_OUT_LEN, out_hex);

    // Carry the source's modification time onto the copy.
    struct stat ss;
    if (stat(src, &ss) == 0) {
        struct utimbuf ut = { ss.st_atime, ss.st_mtime };
        utime(dst, &ut);
    }
    return 0;
}

// Hash `path` without copying. Returns 0 on success.
static int hash_file(const char *path, char out_hex[BLAKE3_HEX_LEN])
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);

    char   buf[1 << 16];
    size_t n;
    int    rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        blake3_hasher_update(&hasher, buf, n);
    }
    if (ferror(f)) rc = -1;
    fclose(f);
    if (rc != 0) return rc;

    uint8_t digest[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&hasher, digest, BLAKE3_OUT_LEN);
    hex_encode(digest, BLAKE3_OUT_LEN, out_hex);
    return 0;
}

// True when `a` and `b` resolve to the same existing file.
static bool same_file(const char *a, const char *b)
{
    char ra[PATH_MAX];
    char rb[PATH_MAX];
    return realpath(a, ra) && realpath(b, rb) && strcmp(ra, rb) == 0;
}

// Upsert the dedupe columns for `rel_path` in `db`. The import runs
// before the post-import library reopen has scanned the new files
// into the photos table, so a plain UPDATE would silently affect
// zero rows and the columns would never persist. The INSERT branch
// creates the row with the imported file's `added_at`; the post-
// import scan's `INSERT OR IGNORE` then leaves this row alone and
// the next import's dedupe lookups find it. `identity` may be NULL
// or empty when the source carried no recoverable EXIF tags.
static void db_store_dedupe(sqlite3 *db, const char *rel_path,
                          const char *hex_hash, const char *identity,
                          int64_t size)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO photos(path, added_at, hash, identity, size) "
            "VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(path) DO UPDATE SET "
            "    hash     = excluded.hash, "
            "    identity = excluded.identity, "
            "    size     = excluded.size;",
            -1, &st, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text (st, 1, rel_path,        -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (int64_t)time(NULL));
    sqlite3_bind_text (st, 3, hex_hash,        -1, SQLITE_STATIC);
    if (identity && identity[0]) {
        sqlite3_bind_text(st, 4, identity, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(st, 4);
    }
    sqlite3_bind_int64(st, 5, size);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

// Look up `identity` in `db`. On a match copies the stored relative
// path into `out_rel` and returns 1. Returns 0 when not found or when
// `identity` is NULL/empty.
static int db_find_identity(sqlite3 *db, const char *identity,
                            char *out_rel, size_t out_len)
{
    if (!db || !identity || !identity[0]) return 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT path FROM photos WHERE identity = ? LIMIT 1;",
            -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, identity, -1, SQLITE_STATIC);
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(st, 0);
        if (p && out_rel && out_len > 0) {
            snprintf(out_rel, out_len, "%s", p);
        }
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

typedef enum {
    IMPORT_ONE_IMPORTED,
    IMPORT_ONE_DUP_CONTENT,
    IMPORT_ONE_RENAMED,
    IMPORT_ONE_SKIP_COLLISION,
    IMPORT_ONE_SKIP_INCOMPLETE_IDENTITY,
    IMPORT_ONE_ERROR,
} import_one_result;

// Import one source raw into dest_dir. `db` may be NULL (no dedupe).
// `subdir` is the single-level destination subdir name (for relative
// path construction when storing the hash). Returns an import_one_result.
static import_one_result import_one(const char *src, const char *dest_dir,
                                    const char *subdir,
                                    const ap_import_settings *s, int seq,
                                    sqlite3 *db)
{
    const char *slash = strrchr(src, '/');
    const char *base  = slash ? slash + 1 : src;
    const char *ext   = strrchr(base, '.');
    if (!ext) {
        ext = "";
    }

    char   stem[AP_IMPORT_STEM_LEN];
    size_t stem_len = ext[0] ? (size_t)(ext - base) : strlen(base);
    if (stem_len >= sizeof(stem)) {
        stem_len = sizeof(stem) - 1;
    }
    memcpy(stem, base, stem_len);
    stem[stem_len] = '\0';

    // EXIF carries the capture time (for pattern naming) and the
    // identity tuple (for dedupe). The identity is only built when
    // all four tuple fields (make, model, capture time, sub-second)
    // are present — partial tuples can collide across cameras or
    // across same-camera bursts and must not be used as a dedupe
    // key. Naming falls back to mtime when EXIF has no capture time.
    ap_exif_fields exif;
    bool have_exif = (ap_exif_read(src, &exif) == 0);
    char identity[256];
    identity[0] = '\0';
    if (have_exif && ap_exif_identity_is_unique(&exif)) {
        ap_exif_identity(&exif, identity, sizeof(identity));
    }

    char name[AP_IMPORT_PATTERN_LEN + 64];
    if (s->naming == AP_IMPORT_NAME_PATTERN) {
        time_t when = have_exif ? exif.capture_time : 0;
        if (when == 0) {
            struct stat ss;
            when = (stat(src, &ss) == 0) ? ss.st_mtime : time(NULL);
        }
        char formatted[AP_IMPORT_PATTERN_LEN];
        format_name(s->pattern, stem, when, seq, formatted, sizeof(formatted));
        if (!formatted[0]) {
            snprintf(formatted, sizeof(formatted), "%s", stem);
        }
        snprintf(name, sizeof(name), "%s%s", formatted, ext);
    } else {
        snprintf(name, sizeof(name), "%s%s", stem, ext);
    }

    char dst[PATH_MAX];
    if (snprintf(dst, sizeof(dst), "%s/%s", dest_dir, name) >= (int)sizeof(dst)) {
        AP_WARN("import: destination path too long, skipping %s", base);
        return IMPORT_ONE_ERROR;
    }

    // Stat source for the size we will store alongside the hash; the
    // size pre-filter on the dedupe path is gone now that identity is
    // an O(log n) index lookup with no file I/O.
    struct stat ss;
    if (stat(src, &ss) != 0) {
        AP_WARN("import: stat %s: %s", src, strerror(errno));
        return IMPORT_ONE_ERROR;
    }
    int64_t src_size = (int64_t)ss.st_size;

    // Identity dedupe: a ~10ms EXIF parse + index lookup beats the
    // 5-10s LibRaw-open + full-source-BLAKE3 pre-pass it replaces.
    // Gated on s->dedupe_content so the user can intentionally keep
    // two copies of the same shot in different subdirs. When the
    // identity tuple is incomplete (`identity` empty), the import
    // either skips the file or copies it depending on strict_identity.
    if (db && s->dedupe_content && !identity[0] && s->strict_identity) {
        AP_INFO("import: %s has incomplete EXIF identity, skipping "
                "(strict_identity is on)", base);
        return IMPORT_ONE_SKIP_INCOMPLETE_IDENTITY;
    }
    if (db && s->dedupe_content && identity[0]) {
        char existing_rel[4096];
        if (db_find_identity(db, identity, existing_rel,
                             sizeof(existing_rel)) == 1) {
            AP_INFO("import: %s already in library at %s, skipping",
                    base, existing_rel);
            return IMPORT_ONE_DUP_CONTENT;
        }
    }

    struct stat ds;
    bool dst_exists = (stat(dst, &ds) == 0);
    bool renamed    = false;

    if (dst_exists) {
        // Narrow byte-equality check at the destination only. Sources
        // with an EXIF identity were already caught by the library-
        // wide identity lookup above; this branch matters for sources
        // that carry no recoverable EXIF (or legacy library rows that
        // predate the identity column) but happen to land at the same
        // dst path with identical bytes. Same-size is a cheap gate
        // before paying for the two BLAKE3 reads.
        bool same_size = ((int64_t)ds.st_size == src_size);
        if (same_size) {
            char src_hex[BLAKE3_HEX_LEN];
            char dst_hex[BLAKE3_HEX_LEN];
            if (hash_file(src, src_hex) == 0
                    && hash_file(dst, dst_hex) == 0
                    && strcmp(src_hex, dst_hex) == 0) {
                return IMPORT_ONE_DUP_CONTENT;
            }
        }

        // Different content at the same destination path.
        if (s->collision == AP_IMPORT_COLLIDE_SKIP) {
            AP_WARN("import: name collision (different content), skipping %s",
                    base);
            return IMPORT_ONE_SKIP_COLLISION;
        }
        if (s->collision == AP_IMPORT_COLLIDE_SUFFIX) {
            char   nstem[AP_IMPORT_PATTERN_LEN + 64];
            char   next[16] = "";
            const char *ndot = strrchr(name, '.');
            if (ndot) {
                snprintf(next, sizeof(next), "%s", ndot);
                size_t nlen = (size_t)(ndot - name);
                if (nlen >= sizeof(nstem)) nlen = sizeof(nstem) - 1;
                memcpy(nstem, name, nlen);
                nstem[nlen] = '\0';
            } else {
                snprintf(nstem, sizeof(nstem), "%s", name);
            }
            bool free_slot = false;
            for (int i = 1; i < 10000; i++) {
                if (snprintf(dst, sizeof(dst), "%s/%s_%04d%s",
                             dest_dir, nstem, i, next) >= (int)sizeof(dst)) {
                    return IMPORT_ONE_ERROR;
                }
                if (stat(dst, &ds) != 0) {
                    free_slot = true;
                    break;
                }
            }
            if (!free_slot) {
                AP_WARN("import: no free suffix slot for %s", base);
                return IMPORT_ONE_ERROR;
            }
            renamed = true;
        } else {
            // OVERWRITE — guard against copying a file onto itself.
            if (same_file(src, dst)) {
                return IMPORT_ONE_DUP_CONTENT;
            }
        }
    }

    char copied_hex[BLAKE3_HEX_LEN];
    if (copy_file_hash(src, dst, copied_hex) != 0) {
        return IMPORT_ONE_ERROR;
    }

    if (db) {
        const char *final_slash = strrchr(dst, '/');
        const char *final_name  = final_slash ? final_slash + 1 : dst;
        char rel[PATH_MAX];
        rel[0] = '\0';
        strncat(rel, subdir, sizeof(rel) - 1);
        size_t slen = strlen(rel);
        if (slen + 1 < sizeof(rel)) {
            rel[slen] = '/';
            rel[slen + 1] = '\0';
            strncat(rel, final_name, sizeof(rel) - slen - 2);
        }
        db_store_dedupe(db, rel, copied_hex, identity, src_size);
    }

    return renamed ? IMPORT_ONE_RENAMED : IMPORT_ONE_IMPORTED;
}

typedef struct {
    char  **paths;
    int     count;
    int     capacity;
} raw_list;

static int raw_list_push(raw_list *list, const char *path)
{
    if (list->count == list->capacity) {
        int cap = list->capacity ? list->capacity * 2 : 64;
        char **p = realloc(list->paths, (size_t)cap * sizeof(*p));
        if (!p) return -1;
        list->paths = p;
        list->capacity = cap;
    }
    list->paths[list->count] = strdup(path);
    if (!list->paths[list->count]) return -1;
    list->count++;
    return 0;
}

static void raw_list_free(raw_list *list)
{
    for (int i = 0; i < list->count; i++) free(list->paths[i]);
    free(list->paths);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void import_collect(const char *src_dir, raw_list *list)
{
    ap_dir *d = ap_dir_open(src_dir);
    if (!d) {
        AP_WARN("import: opendir(%s): %s", src_dir, strerror(ap_dir_open_errno()));
        return;
    }
    const char *ename;
    while ((ename = ap_dir_read(d)) != NULL) {
        if (ename[0] == '.') {
            continue;
        }
        char child[PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s",
                     src_dir, ename) >= (int)sizeof(child)) {
            continue;
        }
        struct stat st;
        if (stat(child, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            import_collect(child, list);
        } else if (S_ISREG(st.st_mode) && ap_raw_is_raw_path(ename)) {
            raw_list_push(list, child);
        }
    }
    ap_dir_close(d);
}

int ap_import_run_into(const char *lib_root, const char *db_path,
                       const char *src_dir, const ap_import_settings *s,
                       ap_import_report *report,
                       ap_import_progress_fn progress, void *userdata)
{
    if (report) {
        memset(report, 0, sizeof(*report));
    }
    if (!lib_root || !src_dir || !s) {
        return -1;
    }

    char subdir[AP_IMPORT_SUBDIR_LEN];
    snprintf(subdir, sizeof(subdir), "%s", s->subdir);
    if (!subdir[0] || strchr(subdir, '/') || strcmp(subdir, "..") == 0) {
        snprintf(subdir, sizeof(subdir), "raw");
    }

    char dest_dir[PATH_MAX];
    if (snprintf(dest_dir, sizeof(dest_dir), "%s/%s",
                 lib_root, subdir) >= (int)sizeof(dest_dir)) {
        AP_ERROR("import: destination path too long");
        return -1;
    }
    if (mkdir(dest_dir, 0755) != 0 && errno != EEXIST) {
        AP_ERROR("import: mkdir(%s): %s", dest_dir, strerror(errno));
        return -1;
    }

    sqlite3 *db = NULL;
    if (db_path) {
        if (sqlite3_open(db_path, &db) != SQLITE_OK) {
            AP_WARN("import: cannot open db %s: %s -- dedupe disabled",
                    db_path, sqlite3_errmsg(db));
            if (db) { sqlite3_close(db); db = NULL; }
        } else {
            sqlite3_exec(db, "PRAGMA journal_mode=WAL;",     NULL, NULL, NULL);
            // Cheaper fsync regime during the bulk write: with WAL,
            // synchronous=NORMAL is safe across crashes (the WAL is
            // checkpointed atomically) and skips per-commit fsync.
            sqlite3_exec(db, "PRAGMA synchronous=NORMAL;",   NULL, NULL, NULL);
            sqlite3_exec(db, "PRAGMA temp_store=MEMORY;",    NULL, NULL, NULL);
            sqlite3_exec(db,
                "CREATE INDEX IF NOT EXISTS idx_photos_identity "
                "ON photos(identity);",
                NULL, NULL, NULL);
            // Single transaction for the whole run -- without this
            // each db_store_dedupe is its own implicit transaction and
            // pays a WAL append per file.
            sqlite3_exec(db, "BEGIN IMMEDIATE;",             NULL, NULL, NULL);
        }
    }

    raw_list list = {0};
    import_collect(src_dir, &list);
    qsort(list.paths, (size_t)list.count, sizeof(*list.paths), cmp_str);

    ap_import_report r = {0};
    bool             cancelled = false;
    int              i;
    for (i = 0; i < list.count; i++) {
        import_one_result res = import_one(list.paths[i], dest_dir, subdir,
                                           s, i + 1, db);
        switch (res) {
        case IMPORT_ONE_IMPORTED:                 r.imported++;                  break;
        case IMPORT_ONE_DUP_CONTENT:              r.dup_content++;               break;
        case IMPORT_ONE_RENAMED:                  r.imported++;
                                                  r.renamed_collision++;         break;
        case IMPORT_ONE_SKIP_COLLISION:           r.skip_collision++;            break;
        case IMPORT_ONE_SKIP_INCOMPLETE_IDENTITY: r.skip_incomplete_identity++;  break;
        case IMPORT_ONE_ERROR:                    r.errored++;                   break;
        }
        if (progress) {
            // false from the caller means "stop now"; the partial
            // results so far are kept and reported.
            if (!progress(i + 1, list.count, userdata)) {
                cancelled = true;
                break;
            }
        }
    }
    int total = list.count;
    raw_list_free(&list);

    if (db) {
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
        sqlite3_close(db);
    }

    r.cancelled = cancelled;
    if (report) *report = r;
    AP_INFO("import: %d imported (%d renamed), %d dup, %d skip-collision, "
            "%d skip-incomplete-identity, %d error of %d from %s into %s%s",
            r.imported, r.renamed_collision, r.dup_content,
            r.skip_collision, r.skip_incomplete_identity, r.errored,
            total, src_dir, dest_dir,
            cancelled ? " (cancelled)" : "");
    return 0;
}

int ap_import_run_ex(ap_library *lib, const char *src_dir,
                     const ap_import_settings *s, ap_import_report *report,
                     ap_import_progress_fn progress, void *userdata)
{
    if (!lib) return -1;
    const char *root = ap_library_root(lib);
    if (!root) return -1;

    char db_path[4096];
    if (snprintf(db_path, sizeof(db_path), "%s/library.db",
                 root) >= (int)sizeof(db_path)) {
        AP_WARN("import: db path too long, skipping dedupe");
        return ap_import_run_into(root, NULL, src_dir, s, report,
                                  progress, userdata);
    }
    return ap_import_run_into(root, db_path, src_dir, s, report,
                              progress, userdata);
}

int ap_import_run(ap_library *lib, const char *src_dir,
                  const ap_import_settings *s, ap_import_report *report)
{
    return ap_import_run_ex(lib, src_dir, s, report, NULL, NULL);
}
