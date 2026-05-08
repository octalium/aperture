#define _GNU_SOURCE

#include "sidecar.h"

#include "core/log.h"

#include <toml.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define APERTURE_SIDECAR_SCHEMA 1

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

int ap_sidecar_load_edit(const char *source_path, ap_edit_state *edit)
{
    if (!source_path || !edit) return -1;

    char path[4096];
    if (sidecar_path(source_path, path, sizeof(path)) < 0) return -1;

    FILE *f = fopen(path, "r");
    if (!f) {
        // Treat missing as "no sidecar yet" — defaults apply.
        if (errno == ENOENT) return -1;
        AP_WARN("sidecar: fopen(%s): %s", path, strerror(errno));
        return -1;
    }

    char errbuf[256];
    toml_table_t *root = toml_parse_file(f, errbuf, sizeof(errbuf));
    fclose(f);
    if (!root) {
        AP_WARN("sidecar: parse %s: %s", path, errbuf);
        return -1;
    }

    int rc = -1;

    toml_table_t *aperture = toml_table_in(root, "aperture");
    if (aperture) {
        int64_t version = 0;
        if (read_int(aperture, "schema_version", &version) == 0) {
            if (version != APERTURE_SIDECAR_SCHEMA) {
                AP_WARN("sidecar: %s schema_version=%lld unsupported (expected %d)",
                        path, (long long)version, APERTURE_SIDECAR_SCHEMA);
                goto done;
            }
        }
    }

    toml_table_t *edit_tbl = toml_table_in(root, "edit");
    if (edit_tbl) {
        double d = 0.0;
        if (read_double(edit_tbl, "exposure_ev",   &d) == 0) edit->exposure_ev   = (float)d;
        if (read_double(edit_tbl, "tone_contrast", &d) == 0) edit->tone_contrast = (float)d;
        if (read_double(edit_tbl, "tone_pivot",    &d) == 0) edit->tone_pivot    = (float)d;
        int64_t i = 0;
        if (read_int(edit_tbl, "respect_orientation", &i) == 0) {
            edit->respect_orientation = (int)(i != 0);
        }
        rc = 0;
    }

done:
    toml_free(root);
    return rc;
}

int ap_sidecar_save_edit(const char *source_path, const ap_edit_state *edit)
{
    if (!source_path || !edit) return -1;

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

    int written = fprintf(f,
        "# Aperture per-photo sidecar. Schema-versioned; edits are TOML.\n"
        "\n"
        "[aperture]\n"
        "schema_version = %d\n"
        "\n"
        "[edit]\n"
        "exposure_ev         = %g\n"
        "tone_contrast       = %g\n"
        "tone_pivot          = %g\n"
        "respect_orientation = %d\n",
        APERTURE_SIDECAR_SCHEMA,
        (double)edit->exposure_ev,
        (double)edit->tone_contrast,
        (double)edit->tone_pivot,
        edit->respect_orientation ? 1 : 0);

    if (written < 0) {
        AP_ERROR("sidecar: fprintf(%s): %s", tmp_path, strerror(errno));
        fclose(f);
        unlink(tmp_path);
        return -1;
    }

    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        AP_ERROR("sidecar: fsync(%s): %s", tmp_path, strerror(errno));
        fclose(f);
        unlink(tmp_path);
        return -1;
    }
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        AP_ERROR("sidecar: rename(%s -> %s): %s", tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return -1;
    }
    return 0;
}
