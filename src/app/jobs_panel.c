#include "modals.h"

#include "core/job.h"

#include "cimgui.h"

#include <stdio.h>

void draw_jobs_panel(ap_app *app)
{
    if (!app || !app->jobs_panel) return;

    ap_job_view jobs[AP_JOB_MAX];
    int n = ap_job_snapshot(jobs, AP_JOB_MAX);
    if (n == 0) {
        // auto-close once everything has finished and unlinked
        app->jobs_panel = false;
        return;
    }

    if (!igBegin("Background Jobs", &app->jobs_panel,
                 ImGuiWindowFlags_AlwaysAutoResize)) {
        igEnd();
        return;
    }

    for (int i = 0; i < n; i++) {
        ap_job_view *v = &jobs[i];
        // Push the id so per-row widgets stay stable as jobs come and
        // go, and so identically-labelled jobs don't collide.
        igPushID_Int((int)v->id);

        igTextUnformatted(v->label, NULL);

        float frac = (v->total > 0) ? (float)v->done / (float)v->total : -1.0f;
        if (frac > 1.0f) frac = 1.0f;
        char overlay[32];
        if (v->total > 0) {
            snprintf(overlay, sizeof(overlay), "%d / %d", v->done, v->total);
        } else {
            snprintf(overlay, sizeof(overlay), "...");
        }
        igProgressBar(frac, (ImVec2_c){ 220.0f, 0.0f }, overlay);
        igSameLine(0.0f, 8.0f);

        if (v->state == AP_JOB_CANCELING) {
            igTextDisabled("canceling...");
        } else {
            bool cancelable = (v->state == AP_JOB_RUNNING ||
                               v->state == AP_JOB_PENDING);
            if (!cancelable) igBeginDisabled(true);
            if (igButton("Cancel", (ImVec2_c){ 0.0f, 0.0f }) && cancelable) {
                // id-keyed: a no-op if the job already finished/freed
                ap_job_request_cancel_by_id(v->id);
            }
            if (!cancelable) igEndDisabled();
        }

        igPopID();
    }

    igEnd();
}
