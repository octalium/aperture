#define _GNU_SOURCE

#include "sidecar.h"

#include "core/compat.h"
#include "core/fs.h"
#include "core/log.h"
#include "edit/stack_toml.h"
#include "modules/module.h"

#include <toml.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Single sidecar shape; no migrations needed yet. We own this format
// end-to-end and nobody downstream is depending on a stable schema,
// so when the model changes we just change the writer/reader and the
// pre-existing files become inert (the loader leaves the stack empty
// and we re-seed from the default pipeline).

static int sidecar_path(const char *source_path, char *out, size_t out_len)
{
    int n = snprintf(out, out_len, "%s.aperture", source_path);
    if (n < 0 || (size_t)n >= out_len) {
        AP_ERROR("sidecar: path too long for %s", source_path);
        return -1;
    }
    return 0;
}

static int read_int(toml_table_t *t, const char *key, int64_t *out)
{
    toml_datum_t v = toml_int_in(t, key);
    if (!v.ok) return -1;
    *out = v.u.i;
    return 0;
}

static const char *read_string(toml_table_t *t, const char *key)
{
    toml_datum_t v = toml_string_in(t, key);
    return v.ok ? v.u.s : NULL;
}

// Fill `out` from the [metadata] table's rating / flag / color keys.
// Each is optional: a missing key leaves that field at its default.
static void read_culling(toml_table_t *metadata, ap_photo_culling *out)
{
    ap_photo_culling_clear(out);
    int64_t rating = 0;
    if (read_int(metadata, "rating", &rating) == 0) {
        out->rating = ap_rating_clamp((int)rating);
    }
    toml_datum_t flag = toml_string_in(metadata, "flag");
    if (flag.ok) {
        out->flag = ap_flag_from_key(flag.u.s);
        free(flag.u.s);
    }
    toml_datum_t color = toml_string_in(metadata, "color");
    if (color.ok) {
        out->color = ap_color_label_from_key(color.u.s);
        free(color.u.s);
    }
}

// Fill `out` from the [metadata] table's `keywords` array of strings.
// Each element is passed through ap_photo_keywords_add so normalisation
// (separator, whitespace) is applied consistently on read.
static void read_keywords(toml_table_t *metadata, ap_photo_keywords *out)
{
    ap_photo_keywords_clear(out);
    toml_array_t *arr = toml_array_in(metadata, "keywords");
    if (!arr) return;
    int n = toml_array_nelem(arr);
    for (int i = 0; i < n; i++) {
        toml_datum_t v = toml_string_at(arr, i);
        if (v.ok) {
            ap_photo_keywords_add(out, v.u.s);
            free(v.u.s);
        }
    }
}

// Fill `out` from the [aperture] table's `groups` array of strings.
static void read_groups(toml_table_t *aperture, ap_photo_groups *out)
{
    out->count = 0;
    toml_array_t *arr = toml_array_in(aperture, "groups");
    if (!arr) return;
    int n = toml_array_nelem(arr);
    for (int i = 0; i < n && out->count < AP_GROUPS_MAX; i++) {
        toml_datum_t v = toml_string_at(arr, i);
        if (v.ok) {
            snprintf(out->names[out->count], AP_GROUP_NAME_LEN, "%s", v.u.s);
            out->count++;
            free(v.u.s);
        }
    }
}

void ap_sidecar_ancillary_clear(ap_sidecar_ancillary *a)
{
    if (!a) return;
    a->respect_orientation = true;
    ap_photo_metadata_clear(&a->user_meta);
    for (int i = 0; i < AP_META_FIELD_COUNT; i++) a->user_set[i] = false;
    ap_photo_culling_clear(&a->culling);
    a->groups.count = 0;
    ap_photo_keywords_clear(&a->keywords);
}

int ap_sidecar_load_full(const char *source_path,
                         ap_edit_stack *stack,
                         ap_sidecar_ancillary *ancillary)
{
    if (!source_path || !stack || !ancillary) return -1;
    ap_edit_stack_init(stack);
    ap_sidecar_ancillary_clear(ancillary);
    return ap_sidecar_load(source_path, stack,
                           &ancillary->respect_orientation,
                           &ancillary->user_meta, ancillary->user_set,
                           &ancillary->culling, &ancillary->groups,
                           &ancillary->keywords);
}

int ap_sidecar_load(const char *source_path, ap_edit_stack *stack,
                    bool *respect_orientation,
                    ap_photo_metadata *user_meta,
                    bool user_set[AP_META_FIELD_COUNT],
                    ap_photo_culling *culling,
                    ap_photo_groups *groups,
                    ap_photo_keywords *keywords)
{
    if (!source_path || !stack) return -1;

    char path[4096];
    if (sidecar_path(source_path, path, sizeof(path)) < 0) return -1;

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno != ENOENT) AP_WARN("sidecar: fopen(%s): %s", path, strerror(errno));
        return -1;
    }

    char errbuf[256];
    toml_table_t *root = toml_parse_file(f, errbuf, sizeof(errbuf));
    fclose(f);
    if (!root) {
        AP_WARN("sidecar: parse %s: %s", path, errbuf);
        return -1;
    }

    toml_table_t *aperture = toml_table_in(root, "aperture");
    if (aperture && respect_orientation) {
        int64_t i = 1;
        if (read_int(aperture, "respect_orientation", &i) == 0) {
            *respect_orientation = i != 0;
        }
    }
    if (groups) {
        groups->count = 0;
        if (aperture) read_groups(aperture, groups);
    }

    toml_array_t *arr = toml_array_in(root, "edit");
    ap_edit_stack_read_toml_array(arr, stack);

    if (user_meta) ap_photo_metadata_clear(user_meta);
    if (user_set) {
        for (int i = 0; i < AP_META_FIELD_COUNT; i++) user_set[i] = false;
    }
    if (culling) ap_photo_culling_clear(culling);
    if (keywords) ap_photo_keywords_clear(keywords);

    // [metadata] is sparse: only fields the user has overridden are
    // written. A present key with an empty string is a deliberate
    // override (the user blanked the field). The same table also
    // carries the typed culling fields (rating / flag / color) and
    // the `keywords` string array.
    toml_table_t *metadata = toml_table_in(root, "metadata");
    if (metadata && user_meta && user_set) {
        for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
            const char *key = ap_meta_field_key((ap_meta_field)i);
            const char *value = read_string(metadata, key);
            if (value) {
                ap_photo_metadata_set(user_meta, (ap_meta_field)i, value);
                user_set[i] = true;
            }
        }
    }
    if (metadata && culling) {
        read_culling(metadata, culling);
    }
    if (metadata && keywords) {
        read_keywords(metadata, keywords);
    }

    toml_free(root);
    return 0;
}

// TOML strings have to escape backslash + quote. Conservative writer
// good enough for the short strings the metadata fields hold; not a
// general-purpose escaper.
static int write_escaped_string(FILE *f, const char *s)
{
    if (fputc('"', f) == EOF) return -1;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' || c == '"') {
            if (fputc('\\', f) == EOF) return -1;
            if (fputc(c, f) == EOF) return -1;
        } else if (c == '\n') {
            if (fputs("\\n", f) == EOF) return -1;
        } else if (c == '\r') {
            if (fputs("\\r", f) == EOF) return -1;
        } else if (c == '\t') {
            if (fputs("\\t", f) == EOF) return -1;
        } else if (c < 0x20) {
            // Drop other control chars rather than emit invalid TOML.
        } else {
            if (fputc((int)c, f) == EOF) return -1;
        }
    }
    if (fputc('"', f) == EOF) return -1;
    return 0;
}

int ap_sidecar_save(const char *source_path, const ap_edit_stack *stack,
                    bool respect_orientation,
                    const ap_photo_metadata *user_meta,
                    const bool user_set[AP_META_FIELD_COUNT],
                    const ap_photo_culling *culling,
                    const ap_photo_groups *groups,
                    const ap_photo_keywords *keywords)
{
    if (!source_path || !stack) return -1;

    char path[4096];
    if (sidecar_path(source_path, path, sizeof(path)) < 0) return -1;

    char tmp_path[4096];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        AP_ERROR("sidecar: tmp path too long");
        return -1;
    }

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        AP_ERROR("sidecar: fopen(%s, 'w'): %s", tmp_path, strerror(errno));
        return -1;
    }

    if (fprintf(f,
        "# Aperture per-photo sidecar.\n"
        "\n"
        "[aperture]\n"
        "respect_orientation = %d\n",
        respect_orientation ? 1 : 0) < 0) goto io_fail;

    if (groups && groups->count > 0) {
        if (fputs("groups = [", f) == EOF) goto io_fail;
        for (int i = 0; i < groups->count; i++) {
            if (i > 0 && fputs(", ", f) == EOF) goto io_fail;
            if (write_escaped_string(f, groups->names[i]) != 0) goto io_fail;
        }
        if (fputs("]\n", f) == EOF) goto io_fail;
    }
    if (fputc('\n', f) == EOF) goto io_fail;

    if (ap_edit_stack_write_toml(stack, f) != 0) goto io_fail;

    // Sparse [metadata] table: the string overrides the user has set,
    // plus the typed culling fields (rating / flag / color) and the
    // `keywords` array whenever they are off their defaults. The table
    // header is emitted once, only when at least one section has content.
    {
        bool any_meta = false;
        if (user_meta && user_set) {
            for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
                if (user_set[i]) { any_meta = true; break; }
            }
        }
        bool any_culling  = culling  && !ap_photo_culling_is_empty(culling);
        bool any_keywords = keywords && !ap_photo_keywords_is_empty(keywords);
        if (any_meta || any_culling || any_keywords) {
            if (fprintf(f, "\n[metadata]\n") < 0) goto io_fail;
        }
        if (any_meta) {
            for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
                if (!user_set[i]) continue;
                const char *key = ap_meta_field_key((ap_meta_field)i);
                const char *val = ap_photo_metadata_get(user_meta,
                                                        (ap_meta_field)i);
                if (fprintf(f, "%s = ", key) < 0) goto io_fail;
                if (write_escaped_string(f, val ? val : "") != 0) goto io_fail;
                if (fputc('\n', f) == EOF) goto io_fail;
            }
        }
        if (any_culling) {
            if (culling->rating != 0) {
                if (fprintf(f, "rating = %d\n", culling->rating) < 0) {
                    goto io_fail;
                }
            }
            if (culling->flag != AP_FLAG_NONE) {
                if (fputs("flag = ", f) == EOF) goto io_fail;
                if (write_escaped_string(f, ap_flag_key(culling->flag)) != 0) {
                    goto io_fail;
                }
                if (fputc('\n', f) == EOF) goto io_fail;
            }
            if (culling->color != AP_COLOR_NONE) {
                if (fputs("color = ", f) == EOF) goto io_fail;
                if (write_escaped_string(f,
                        ap_color_label_key(culling->color)) != 0) {
                    goto io_fail;
                }
                if (fputc('\n', f) == EOF) goto io_fail;
            }
        }
        if (any_keywords) {
            if (fputs("keywords = [", f) == EOF) goto io_fail;
            for (int i = 0; i < keywords->count; i++) {
                if (i > 0 && fputs(", ", f) == EOF) goto io_fail;
                if (write_escaped_string(f, keywords->kw[i]) != 0) goto io_fail;
            }
            if (fputs("]\n", f) == EOF) goto io_fail;
        }
    }

    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        AP_ERROR("sidecar: fsync(%s): %s", tmp_path, strerror(errno));
        goto io_fail;
    }
    fclose(f);
    f = NULL;

    if (ap_rename_replace(tmp_path, path) != 0) {
        AP_ERROR("sidecar: rename(%s -> %s): %s", tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return -1;
    }
    return 0;

io_fail:
    if (f) fclose(f);
    unlink(tmp_path);
    AP_ERROR("sidecar: write %s: %s", tmp_path,
             errno ? strerror(errno) : "i/o error");
    return -1;
}

int ap_sidecar_load_groups(const char *source_path, ap_photo_groups *out)
{
    if (!source_path || !out) return -1;
    out->count = 0;

    char path[4096];
    if (sidecar_path(source_path, path, sizeof(path)) < 0) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char errbuf[256];
    toml_table_t *root = toml_parse_file(f, errbuf, sizeof(errbuf));
    fclose(f);
    if (!root) return -1;

    toml_table_t *aperture = toml_table_in(root, "aperture");
    if (aperture) read_groups(aperture, out);
    toml_free(root);
    return 0;
}

int ap_sidecar_load_culling(const char *source_path, ap_photo_culling *out)
{
    if (!source_path || !out) return -1;
    ap_photo_culling_clear(out);

    char path[4096];
    if (sidecar_path(source_path, path, sizeof(path)) < 0) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char errbuf[256];
    toml_table_t *root = toml_parse_file(f, errbuf, sizeof(errbuf));
    fclose(f);
    if (!root) return -1;

    toml_table_t *metadata = toml_table_in(root, "metadata");
    if (metadata) read_culling(metadata, out);
    toml_free(root);
    return 0;
}

int ap_sidecar_remove(const char *source_path)
{
    if (!source_path) return -1;

    char path[4096];
    if (sidecar_path(source_path, path, sizeof(path)) < 0) return -1;

    if (unlink(path) != 0 && errno != ENOENT) {
        AP_WARN("sidecar: unlink %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}
