#include "toast.h"

#include "cimgui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// How long a toast is fully visible (seconds) before it starts fading.
#define TOAST_HOLD_SECS  3.5
// Duration of the fade-out tail (seconds).
#define TOAST_FADE_SECS  0.5
// Total lifespan = HOLD + FADE.
#define TOAST_TTL_SECS   (TOAST_HOLD_SECS + TOAST_FADE_SECS)

#define TOAST_MAX  8
#define TOAST_W    280.0f
#define TOAST_PAD  8.0f    // gap from the window edge and between cards

typedef struct {
    ap_toast_kind kind;
    char          msg[256];
    double        expire;  // igGetTime() value when this toast dies
} toast_entry;

static toast_entry g_toasts[TOAST_MAX];
static int         g_count = 0;   // live entries in g_toasts[0..g_count-1]

void ap_toast_push(ap_toast_kind kind, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // When the queue is full, drop the oldest (index 0) by shifting
    // the remaining entries down one slot.
    if (g_count == TOAST_MAX) {
        memmove(&g_toasts[0], &g_toasts[1],
                (size_t)(TOAST_MAX - 1) * sizeof(g_toasts[0]));
        g_count = TOAST_MAX - 1;
    }

    toast_entry *e = &g_toasts[g_count++];
    e->kind   = kind;
    e->expire = igGetTime() + TOAST_TTL_SECS;
    snprintf(e->msg, sizeof(e->msg), "%s", buf);
}

void ap_toast_draw(void)
{
    if (g_count == 0) return;

    double now = igGetTime();

    // Expire stale entries (compact in-place, preserving order).
    int live = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_toasts[i].expire > now) {
            if (live != i) g_toasts[live] = g_toasts[i];
            live++;
        }
    }
    g_count = live;
    if (g_count == 0) return;

    ImGuiViewport *vp = igGetMainViewport();
    float vp_x = vp->WorkPos.x;
    float vp_y = vp->WorkPos.y;
    float vp_w = vp->WorkSize.x;
    float vp_h = vp->WorkSize.y;

    // Cards stack upward from the bottom-right corner.  We approximate
    // a card height of one text line + vertical padding; the actual
    // rendered height varies with message wrapping, but ImGui
    // AlwaysAutoResize handles that.  The anchor is the bottom of each
    // card, recalculated as we walk the stack from oldest (bottom) to
    // newest (top).
    const float card_h_approx = 36.0f;   // used only for stacking offset
    float bottom_y = vp_y + vp_h - TOAST_PAD;

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

    for (int i = 0; i < g_count; i++) {
        toast_entry *e = &g_toasts[i];

        double remaining = e->expire - now;
        float alpha = 1.0f;
        if (remaining < TOAST_FADE_SECS) {
            alpha = (float)(remaining / TOAST_FADE_SECS);
            if (alpha < 0.0f) alpha = 0.0f;
        }

        // Position: anchored to bottom-right, stacked upward.
        // The pivot (1,1) pins the window's own bottom-right to the
        // supplied pos, so we give the bottom-right coordinate directly.
        ImVec2_c pos   = { vp_x + vp_w - TOAST_PAD, bottom_y };
        ImVec2_c pivot = { 1.0f, 1.0f };
        igSetNextWindowPos(pos, ImGuiCond_Always, pivot);
        igSetNextWindowSize((ImVec2_c){ TOAST_W, 0.0f }, ImGuiCond_Always);
        igSetNextWindowBgAlpha(alpha * 0.88f);

        // Choose accent colour by kind.
        ImVec4_c accent;
        if (e->kind == AP_TOAST_ERROR) {
            accent = (ImVec4_c){ 0.85f, 0.25f, 0.20f, alpha };
        } else {
            accent = (ImVec4_c){ 0.20f, 0.65f, 0.35f, alpha };
        }

        igPushStyleVar_Float(ImGuiStyleVar_WindowRounding,   6.0f);
        igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 1.5f);
        igPushStyleColor_Vec4(ImGuiCol_Border,    accent);
        igPushStyleColor_Vec4(ImGuiCol_Text,
                              (ImVec4_c){ 1.0f, 1.0f, 1.0f, alpha });

        // Unique window ID per slot so ImGui tracks them independently.
        char win_id[32];
        snprintf(win_id, sizeof(win_id), "##toast_%d", i);
        if (igBegin(win_id, NULL, flags)) {
            igTextWrapped("%s", e->msg);
        }
        igEnd();

        igPopStyleColor(2);
        igPopStyleVar(2);

        bottom_y -= card_h_approx + TOAST_PAD;
    }
}
