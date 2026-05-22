#define _GNU_SOURCE

#include "library/import.h"

#include "core/log.h"
#include "io/raw.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

// ----- settings -----------------------------------------------------

#define KEY_SUBDIR    "import.subdir"
#define KEY_NAMING    "import.naming"
#define KEY_PATTERN   "import.pattern"
#define KEY_COLLISION "import.collision"

void ap_import_settings_load(const ap_library *lib, ap_import_settings *out)
{
    if (!out) return;

    // Defaults.
    snprintf(out->subdir, sizeof(out->subdir), "raw");
    out->naming = AP_IMPORT_NAME_KEEP;
    snprintf(out->pattern, sizeof(out->pattern), "{YYYY}{MM}{DD}_{HH}{MIN}{SEC}");
    out->collision = AP_IMPORT_COLLIDE_SKIP;
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
                             ? c : AP_IMPORT_COLLIDE_SKIP;
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
}

// ----- filename formatting ------------------------------------------

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
        char rep[64];
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

// ----- file copy ----------------------------------------------------

static int copy_file(const char *src, const char *dst)
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

    char   buf[1 << 16];
    size_t n;
    int    rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
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

    // Carry the source's modification time onto the copy.
    struct stat ss;
    if (stat(src, &ss) == 0) {
        struct utimbuf ut = { ss.st_atime, ss.st_mtime };
        utime(dst, &ut);
    }
    return 0;
}

// True when `a` and `b` resolve to the same existing file.
static bool same_file(const char *a, const char *b)
{
    char ra[PATH_MAX];
    char rb[PATH_MAX];
    return realpath(a, ra) && realpath(b, rb) && strcmp(ra, rb) == 0;
}

// ----- per-file import ----------------------------------------------

// Copy one source raw into dest_dir. Returns 1 when a file was copied,
// 0 when it was skipped (collision policy, self-copy, or a copy error
// — errors are logged inside).
static int import_one(const char *src, const char *dest_dir,
                      const ap_import_settings *s, int seq)
{
    const char *slash = strrchr(src, '/');
    const char *base  = slash ? slash + 1 : src;
    const char *ext   = strrchr(base, '.');
    if (!ext) {
        ext = "";
    }

    char   stem[256];
    size_t stem_len = ext[0] ? (size_t)(ext - base) : strlen(base);
    if (stem_len >= sizeof(stem)) {
        stem_len = sizeof(stem) - 1;
    }
    memcpy(stem, base, stem_len);
    stem[stem_len] = '\0';

    char name[AP_IMPORT_PATTERN_LEN + 64];
    if (s->naming == AP_IMPORT_NAME_PATTERN) {
        time_t when;
        if (ap_raw_capture_time(src, &when) != 0) {
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
        return 0;
    }

    struct stat ds;
    if (stat(dst, &ds) == 0) {
        if (s->collision == AP_IMPORT_COLLIDE_SKIP) {
            return 0;
        }
        if (s->collision == AP_IMPORT_COLLIDE_SUFFIX) {
            char        nstem[AP_IMPORT_PATTERN_LEN + 64];
            char        next[16] = "";
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
                if (snprintf(dst, sizeof(dst), "%s/%s_%d%s",
                             dest_dir, nstem, i, next) >= (int)sizeof(dst)) {
                    return 0;
                }
                if (stat(dst, &ds) != 0) {
                    free_slot = true;
                    break;
                }
            }
            if (!free_slot) {
                return 0;
            }
        }
        // OVERWRITE — guard against copying a file onto itself.
        else if (same_file(src, dst)) {
            return 0;
        }
    }

    if (copy_file(src, dst) != 0) {
        return 0;
    }
    return 1;
}

// ----- recursive walk -----------------------------------------------

// Dynamic list of raw-file paths collected during the directory walk.
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
    DIR *d = opendir(src_dir);
    if (!d) {
        AP_WARN("import: opendir(%s): %s", src_dir, strerror(errno));
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char child[PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s",
                     src_dir, ent->d_name) >= (int)sizeof(child)) {
            continue;
        }
        struct stat st;
        if (stat(child, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            import_collect(child, list);
        } else if (S_ISREG(st.st_mode) && ap_raw_is_raw_path(ent->d_name)) {
            raw_list_push(list, child);
        }
    }
    closedir(d);
}

int ap_import_run(ap_library *lib, const char *src_dir,
                  const ap_import_settings *s, int *out_imported)
{
    if (out_imported) {
        *out_imported = 0;
    }
    if (!lib || !src_dir || !s) {
        return -1;
    }
    const char *root = ap_library_root(lib);
    if (!root) {
        return -1;
    }

    // The destination subdir is a single directory level under the
    // library root — reject anything with a separator or `..`.
    char subdir[AP_IMPORT_SUBDIR_LEN];
    snprintf(subdir, sizeof(subdir), "%s", s->subdir);
    if (!subdir[0] || strchr(subdir, '/') || strcmp(subdir, "..") == 0) {
        snprintf(subdir, sizeof(subdir), "raw");
    }

    char dest_dir[PATH_MAX];
    if (snprintf(dest_dir, sizeof(dest_dir), "%s/%s",
                 root, subdir) >= (int)sizeof(dest_dir)) {
        AP_ERROR("import: destination path too long");
        return -1;
    }
    if (mkdir(dest_dir, 0755) != 0 && errno != EEXIST) {
        AP_ERROR("import: mkdir(%s): %s", dest_dir, strerror(errno));
        return -1;
    }

    // Collect all raw paths, sort for deterministic {SEQ} numbering,
    // then process in order.
    raw_list list = {0};
    import_collect(src_dir, &list);
    qsort(list.paths, (size_t)list.count, sizeof(*list.paths), cmp_str);

    int copied = 0;
    for (int i = 0; i < list.count; i++) {
        copied += import_one(list.paths[i], dest_dir, s, i + 1);
    }
    int total = list.count;
    raw_list_free(&list);

    if (out_imported) {
        *out_imported = copied;
    }
    AP_INFO("import: copied %d of %d raw file(s) from %s into %s",
            copied, total, src_dir, dest_dir);
    return 0;
}
