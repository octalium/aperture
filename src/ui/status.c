#include "status.h"

#include "cimgui.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define NOTIFY_HOLD_SECS  3.5
#define NOTIFY_FADE_SECS  0.5
#define NOTIFY_TTL_SECS   (NOTIFY_HOLD_SECS + NOTIFY_FADE_SECS)
#define NOTIFY_MAX        8

#define PROGRESS_MAX      8
#define PROGRESS_HOLD_SECS 1.2

#define STATUS_W    300.0f
#define STATUS_PAD  8.0f

typedef struct {
    ap_status_kind kind;
    char           msg[256];
    double         expire;
} notify_entry;

typedef struct {
    ap_status_id id;
    char         label[128];
    int          done;
    int          total;
    int          finished;  // 1 = done, -1 = error
    double       finish_expire;
} progress_entry;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static notify_entry   g_notifs[NOTIFY_MAX];
static int            g_notif_count = 0;

static progress_entry g_progress[PROGRESS_MAX];
static int            g_prog_count = 0;
static unsigned int   g_next_id    = 1;

void ap_status_notify(ap_status_kind kind, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_mu);
    if (g_notif_count == NOTIFY_MAX) {
        memmove(&g_notifs[0], &g_notifs[1],
                (size_t)(NOTIFY_MAX - 1) * sizeof(g_notifs[0]));
        g_notif_count = NOTIFY_MAX - 1;
    }
    notify_entry *e = &g_notifs[g_notif_count++];
    e->kind   = kind;
    e->expire = 0.0;  // set to real time in draw (igGetTime is main-thread only)
    snprintf(e->msg, sizeof(e->msg), "%s", buf);
    pthread_mutex_unlock(&g_mu);
}

ap_status_id ap_status_progress_begin(const char *label, int total)
{
    ap_status_id id = 0;
    pthread_mutex_lock(&g_mu);
    if (g_prog_count < PROGRESS_MAX) {
        progress_entry *e = &g_progress[g_prog_count++];
        id         = g_next_id++;
        if (g_next_id == 0) g_next_id = 1;
        e->id      = id;
        snprintf(e->label, sizeof(e->label), "%s", label ? label : "");
        e->done    = 0;
        e->total   = total;
        e->finished = 0;
        e->finish_expire = 0.0;
    }
    pthread_mutex_unlock(&g_mu);
    return id;
}

void ap_status_progress_update(ap_status_id id, int done, int total)
{
    if (!id) return;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_prog_count; i++) {
        if (g_progress[i].id == id) {
            g_progress[i].done = done;
            if (total > 0) g_progress[i].total = total;
            break;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void ap_status_progress_finish(ap_status_id id, int ok)
{
    if (!id) return;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_prog_count; i++) {
        if (g_progress[i].id == id) {
            g_progress[i].finished    = ok ? 1 : -1;
            g_progress[i].finish_expire = 0.0;  // set in draw
            break;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void ap_status_draw(void)
{
    pthread_mutex_lock(&g_mu);

    double now = igGetTime();

    // Stamp new notifications that haven't been given a real time yet.
    for (int i = 0; i < g_notif_count; i++) {
        if (g_notifs[i].expire == 0.0) {
            g_notifs[i].expire = now + NOTIFY_TTL_SECS;
        }
    }

    // Stamp newly-finished progress entries.
    for (int i = 0; i < g_prog_count; i++) {
        if (g_progress[i].finished != 0 && g_progress[i].finish_expire == 0.0) {
            g_progress[i].finish_expire = now + PROGRESS_HOLD_SECS;
        }
    }

    // Expire finished progress entries whose hold has elapsed.
    int plive = 0;
    for (int i = 0; i < g_prog_count; i++) {
        if (g_progress[i].finished != 0 &&
            g_progress[i].finish_expire > 0.0 &&
            g_progress[i].finish_expire <= now) {
            continue;
        }
        if (plive != i) g_progress[plive] = g_progress[i];
        plive++;
    }
    g_prog_count = plive;

    // Expire stale notifications.
    int nlive = 0;
    for (int i = 0; i < g_notif_count; i++) {
        if (g_notifs[i].expire <= now) continue;
        if (nlive != i) g_notifs[nlive] = g_notifs[i];
        nlive++;
    }
    g_notif_count = nlive;

    int total_count = g_notif_count + g_prog_count;

    // Take local copies so we can unlock before doing any ImGui work.
    notify_entry   notifs[NOTIFY_MAX];
    progress_entry progs[PROGRESS_MAX];
    int ncount = g_notif_count;
    int pcount = g_prog_count;
    memcpy(notifs, g_notifs, (size_t)ncount * sizeof(notifs[0]));
    memcpy(progs,  g_progress, (size_t)pcount * sizeof(progs[0]));

    pthread_mutex_unlock(&g_mu);

    if (total_count == 0) return;

    ImGuiViewport *vp = igGetMainViewport();
    float vp_x = vp->WorkPos.x;
    float vp_y = vp->WorkPos.y;
    float vp_w = vp->WorkSize.x;
    float vp_h = vp->WorkSize.y;

    const float card_h_approx = 40.0f;
    float bottom_y = vp_y + vp_h - STATUS_PAD;

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoSavedSettings
                           | ImGuiWindowFlags_NoFocusOnAppearing
                           | ImGuiWindowFlags_NoNav
                           | ImGuiWindowFlags_NoDocking
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_AlwaysAutoResize;

    // Draw progress entries first (they stack from the bottom upward,
    // so render oldest first = lowest on screen).
    for (int i = 0; i < pcount; i++) {
        progress_entry *e = &progs[i];

        float alpha = 1.0f;
        if (e->finished != 0 && e->finish_expire > 0.0) {
            double remaining = e->finish_expire - now;
            if (remaining < NOTIFY_FADE_SECS) {
                alpha = (float)(remaining / NOTIFY_FADE_SECS);
                if (alpha < 0.0f) alpha = 0.0f;
            }
        }

        ImVec4_c accent;
        if (e->finished < 0) {
            accent = (ImVec4_c){ 0.85f, 0.25f, 0.20f, alpha };
        } else if (e->finished > 0) {
            accent = (ImVec4_c){ 0.20f, 0.65f, 0.35f, alpha };
        } else {
            accent = (ImVec4_c){ 0.35f, 0.55f, 0.85f, alpha };
        }

        ImVec2_c pos   = { vp_x + vp_w - STATUS_PAD, bottom_y };
        ImVec2_c pivot = { 1.0f, 1.0f };
        igSetNextWindowPos(pos, ImGuiCond_Always, pivot);
        igSetNextWindowSize((ImVec2_c){ STATUS_W, 0.0f }, ImGuiCond_Always);
        igSetNextWindowBgAlpha(alpha * 0.88f);

        igPushStyleVar_Float(ImGuiStyleVar_WindowRounding,   6.0f);
        igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 1.5f);
        igPushStyleColor_Vec4(ImGuiCol_Border, accent);
        igPushStyleColor_Vec4(ImGuiCol_Text,
                              (ImVec4_c){ 1.0f, 1.0f, 1.0f, alpha });
        igPushStyleColor_Vec4(ImGuiCol_PlotHistogram, accent);

        char win_id[32];
        snprintf(win_id, sizeof(win_id), "##status_p%u", e->id);
        if (igBegin(win_id, NULL, flags)) {
            igTextWrapped("%s", e->label);
            if (e->finished == 0) {
                if (e->total > 0) {
                    float frac = (float)e->done / (float)e->total;
                    if (frac > 1.0f) frac = 1.0f;
                    char overlay[32];
                    snprintf(overlay, sizeof(overlay), "%d / %d",
                             e->done, e->total);
                    igProgressBar(frac, (ImVec2_c){ -1.0f, 0.0f }, overlay);
                } else {
                    // Indeterminate: animate a spinner-like oscillation.
                    float t = (float)now;
                    float frac = 0.5f + 0.5f * igGetTime();
                    frac = frac - (float)(int)frac;
                    (void)t;
                    igProgressBar(-frac, (ImVec2_c){ -1.0f, 0.0f }, "");
                }
            } else {
                float frac = 1.0f;
                const char *done_text = (e->finished > 0) ? "Done" : "Failed";
                igProgressBar(frac, (ImVec2_c){ -1.0f, 0.0f }, done_text);
            }
        }
        igEnd();

        igPopStyleColor(3);
        igPopStyleVar(2);

        bottom_y -= card_h_approx + STATUS_PAD;
    }

    // Draw transient notifications above the progress entries.
    for (int i = 0; i < ncount; i++) {
        notify_entry *e = &notifs[i];

        double remaining = e->expire - now;
        float alpha = 1.0f;
        if (remaining < NOTIFY_FADE_SECS) {
            alpha = (float)(remaining / NOTIFY_FADE_SECS);
            if (alpha < 0.0f) alpha = 0.0f;
        }

        ImVec4_c accent;
        if (e->kind == AP_STATUS_ERROR) {
            accent = (ImVec4_c){ 0.85f, 0.25f, 0.20f, alpha };
        } else {
            accent = (ImVec4_c){ 0.20f, 0.65f, 0.35f, alpha };
        }

        ImVec2_c pos   = { vp_x + vp_w - STATUS_PAD, bottom_y };
        ImVec2_c pivot = { 1.0f, 1.0f };
        igSetNextWindowPos(pos, ImGuiCond_Always, pivot);
        igSetNextWindowSize((ImVec2_c){ STATUS_W, 0.0f }, ImGuiCond_Always);
        igSetNextWindowBgAlpha(alpha * 0.88f);

        igPushStyleVar_Float(ImGuiStyleVar_WindowRounding,   6.0f);
        igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 1.5f);
        igPushStyleColor_Vec4(ImGuiCol_Border, accent);
        igPushStyleColor_Vec4(ImGuiCol_Text,
                              (ImVec4_c){ 1.0f, 1.0f, 1.0f, alpha });

        char win_id[32];
        snprintf(win_id, sizeof(win_id), "##status_n%d", i);
        if (igBegin(win_id, NULL, flags)) {
            igTextWrapped("%s", e->msg);
        }
        igEnd();

        igPopStyleColor(2);
        igPopStyleVar(2);

        bottom_y -= card_h_approx + STATUS_PAD;
    }
}
