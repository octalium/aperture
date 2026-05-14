#define _GNU_SOURCE

#include "sidecar.h"

#include "core/log.h"
#include "modules/module.h"

#include <toml.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Pre-release: one sidecar shape, no migrations. We own this format
// end-to-end and nobody downstream is depending on a stable schema
// yet, so when the model changes we just change the writer/reader
// and the pre-existing files become inert (the loader leaves the
// stack empty and we re-seed from the default pipeline).

static int sidecar_path(const char *source_path, char *out, size_t out_len)
{
    int n = snprintf(out, out_len, "%s.aperture", source_path);
    if (n < 0 || (size_t)n >= out_len) {
        AP_ERROR("sidecar: path too long for %s", source_path);
        return -1;
    }
    return 0;
}

static int read_double(toml_table_t *t, const char *key, double *out)
{
    toml_datum_t v = toml_double_in(t, key);
    if (!v.ok) return -1;
    *out = v.u.d;
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

// Load a single [[edit]] table row into the stack.
static void load_edit_entry(toml_table_t *t, ap_edit_stack *stack)
{
    const char *module_name = read_string(t, "module");
    if (!module_name) return;
    int idx = ap_edit_stack_add(stack, module_name);
    if (idx < 0) return;
    ap_edit_entry *e = ap_edit_stack_at(stack, idx);
    const ap_module *m = ap_module_find(module_name);

    int64_t i = 0;
    if (read_int(t, "enabled", &i) == 0) e->enabled = i != 0;
    const char *display = read_string(t, "name");
    if (display && *display) {
        snprintf(e->display_name, sizeof(e->display_name), "%s", display);
    }

    if (m && m->params_names) {
        for (int s = 0; s < m->params_count; s++) {
            const char *name = m->params_names[s];
            if (!name) continue;
            double d = 0.0;
            if (read_double(t, name, &d) == 0) {
                e->params[s] = (float)d;
            }
        }
    }
}

int ap_sidecar_load(const char *source_path, ap_edit_stack *stack,
                    bool *respect_orientation)
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

    ap_edit_stack_init(stack);

    toml_array_t *arr = toml_array_in(root, "edit");
    if (arr) {
        int nrows = toml_array_nelem(arr);
        for (int r = 0; r < nrows; r++) {
            toml_table_t *t = toml_table_at(arr, r);
            if (t) load_edit_entry(t, stack);
        }
    }

    toml_free(root);
    return 0;
}

int ap_sidecar_save(const char *source_path, const ap_edit_stack *stack,
                    bool respect_orientation)
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

    int count = ap_edit_stack_count(stack);
    for (int i = 0; i < count; i++) {
        const ap_edit_entry *e = ap_edit_stack_at_const(stack, i);
        if (!e) continue;
        const ap_module *m = ap_module_find(e->module_name);
        if (fprintf(f,
            "\n[[edit]]\n"
            "module  = \"%s\"\n"
            "enabled = %d\n",
            e->module_name, e->enabled ? 1 : 0) < 0) goto io_fail;
        if (e->display_name[0]) {
            if (fprintf(f, "name    = \"%s\"\n", e->display_name) < 0) {
                goto io_fail;
            }
        }
        if (m && m->params_names) {
            for (int s = 0; s < m->params_count; s++) {
                const char *name = m->params_names[s];
                if (!name) continue;
                if (fprintf(f, "%-9s = %g\n", name, (double)e->params[s]) < 0) {
                    goto io_fail;
                }
            }
        }
    }

    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        AP_ERROR("sidecar: fsync(%s): %s", tmp_path, strerror(errno));
        goto io_fail;
    }
    fclose(f);
    f = NULL;

    if (rename(tmp_path, path) != 0) {
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
