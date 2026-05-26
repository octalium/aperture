#include "ui/file_dialog.h"

#include "core/log.h"

#include <nfd.h>

#include <stdio.h>

// Thin wrapper over nativefiledialog-extended. NFD is initialised and
// torn down around each call — the dialog is a rare, user-driven
// action, so there is no global lifecycle to manage.

bool ap_file_dialog_pick_folder(char *out, size_t out_len,
                                const char *default_path)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    if (NFD_Init() != NFD_OKAY) {
        AP_WARN("file dialog: NFD_Init failed: %s", NFD_GetError());
        return false;
    }

    nfdnchar_t *picked = NULL;
    nfdresult_t r = NFD_PickFolderN(
        &picked, (default_path && *default_path) ? default_path : NULL);

    bool ok = false;
    if (r == NFD_OKAY) {
        snprintf(out, out_len, "%s", picked);
        NFD_FreePathN(picked);
        ok = true;
    } else if (r == NFD_ERROR) {
        AP_WARN("file dialog: %s", NFD_GetError());
    }
    // NFD_CANCEL — the user dismissed the dialog; not an error.

    NFD_Quit();
    return ok;
}
