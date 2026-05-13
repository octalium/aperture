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

#define APERTURE_SIDECAR_SCHEMA 2

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

// Migrate a v1 sidecar's flat edit fields into stack entries. v1
// implicitly applied demosaic + WB / color matrix as transport; v2
// surfaces those as Demosaic + Color in the stack, so we prepend
// them on top of the user-tunable Exposure + Tone.
static void load_v1_edit_table(toml_table_t *edit_tbl, ap_edit_stack *stack,
                               bool *respect_orientation)
{
    double d = 0.0;
    int64_t i = 0;

    if (read_int(edit_tbl, "respect_orientation", &i) == 0 && respect_orientation) {
        *respect_orientation = i != 0;
    }

    ap_edit_stack_add(stack, "demosaic");
    ap_edit_stack_add(stack, "color");

    int exp_idx = ap_edit_stack_add(stack, "exposure");
    if (exp_idx >= 0) {
        ap_edit_entry *e = ap_edit_stack_at(stack, exp_idx);
        if (read_double(edit_tbl, "exposure_ev", &d) == 0) {
            e->params[0] = (float)d;
        }
    }

    int tone_idx = ap_edit_stack_add(stack, "tone");
    if (tone_idx >= 0) {
        ap_edit_entry *e = ap_edit_stack_at(stack, tone_idx);
        if (read_double(edit_tbl, "tone_contrast", &d) == 0) e->params[0] = (float)d;
        if (read_double(edit_tbl, "tone_pivot",    &d) == 0) e->params[1] = (float)d;
    }
}

// Load a single [[edit]] table row into the stack.
static void load_v2_edit_entry(toml_table_t *t, ap_edit_stack *stack)
{
    const char *module_name = read_string(t, "module");
    if (!module_name) return;
    // Drop legacy `encode` entries — Output Transfer is now appended
    // automatically by the graph and isn't a user-stack entry.
    if (strcmp(module_name, "encode") == 0) return;
    int idx = ap_edit_stack_add(stack, module_name);
    if (idx < 0) return;
    ap_edit_entry *e = ap_edit_stack_at(stack, idx);
    const ap_module *m = ap_module_find(module_name);

    int64_t i = 0;
    if (read_int(t, "enabled", &i) == 0) e->enabled = i != 0;

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

    int64_t version = APERTURE_SIDECAR_SCHEMA;
    toml_table_t *aperture = toml_table_in(root, "aperture");
    if (aperture) {
        read_int(aperture, "schema_version", &version);
        if (respect_orientation) {
            int64_t i = 1;
            if (read_int(aperture, "respect_orientation", &i) == 0) {
                *respect_orientation = i != 0;
            }
        }
    }

    ap_edit_stack_init(stack);

    if (version == 1) {
        toml_table_t *edit_tbl = toml_table_in(root, "edit");
        if (edit_tbl) load_v1_edit_table(edit_tbl, stack, respect_orientation);
        toml_free(root);
        return 0;
    }
    if (version != APERTURE_SIDECAR_SCHEMA) {
        AP_WARN("sidecar: %s schema_version=%lld unsupported (expected %d)",
                path, (long long)version, APERTURE_SIDECAR_SCHEMA);
        toml_free(root);
        return -1;
    }

    // v2: [[edit]] array of tables.
    toml_array_t *arr = toml_array_in(root, "edit");
    if (arr) {
        int nrows = toml_array_nelem(arr);
        for (int r = 0; r < nrows; r++) {
            toml_table_t *t = toml_table_at(arr, r);
            if (t) load_v2_edit_entry(t, stack);
        }
    }

    // Legacy v2 sidecars (written before Demosaic + Color became
    // user-visible stack entries) don't carry those entries. Prepend
    // them so old photos still render correctly without forcing the
    // user to add them manually.
    bool has_demosaic = false, has_color = false;
    for (int i = 0; i < stack->count; i++) {
        if (strcmp(stack->entries[i].module_name, "demosaic") == 0) has_demosaic = true;
        if (strcmp(stack->entries[i].module_name, "color")    == 0) has_color    = true;
    }
    if (!has_demosaic || !has_color) {
        ap_edit_stack old = *stack;
        ap_edit_stack_init(stack);
        if (!has_demosaic) ap_edit_stack_add(stack, "demosaic");
        if (!has_color)    ap_edit_stack_add(stack, "color");
        for (int i = 0; i < old.count; i++) {
            int new_idx = ap_edit_stack_add(stack, old.entries[i].module_name);
            if (new_idx >= 0) {
                ap_edit_entry *e = ap_edit_stack_at(stack, new_idx);
                memcpy(e->params, old.entries[i].params, sizeof(e->params));
                e->enabled = old.entries[i].enabled;
            }
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

    int rc = -1;
    if (fprintf(f,
        "# Aperture per-photo sidecar. Schema-versioned; edits are TOML.\n"
        "\n"
        "[aperture]\n"
        "schema_version       = %d\n"
        "respect_orientation  = %d\n",
        APERTURE_SIDECAR_SCHEMA,
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
        if (m && m->params_names) {
            for (int s = 0; s < m->params_count; s++) {
                const char *name = m->params_names[s];
                if (!name) continue;
                if (fprintf(f, "%-7s = %g\n", name, (double)e->params[s]) < 0) {
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
    (void)rc;
    return -1;
}
