#include "panels.h"

#include "photo/keywords.h"
#include "photo/photo.h"

#include "cimgui.h"

#include <stdio.h>
#include <string.h>

// Keywords panel (photo mode): add and remove keywords on the open
// photo. Keywords are stored verbatim in the sidecar as a flat list;
// hierarchy is expressed by embedding the separator character ('|')
// in the keyword string, e.g. "lighting|softbox". On read both '|'
// and '/' are accepted and normalised to '|'.
//
// UX: a scrollable list of assigned keywords (click [x] to remove),
// followed by a text input + "Add" button. Typing a partial keyword
// and pressing Enter or clicking "Add" adds it. Duplicate and empty
// entries are silently ignored by the keywords model.

static char g_input[AP_KEYWORD_LEN] = {0};

static void photo_keywords_draw(ap_app *app)
{
    ap_photo *photo = ap_app_photo(app);
    if (!photo) {
        g_input[0] = '\0';
        return;
    }

    if (!igBegin("Keywords", NULL, 0)) {
        igEnd();
        return;
    }

    igTextDisabled("hierarchy: use | to separate levels  (e.g. lighting|softbox)");
    igSeparator();

    const ap_photo_keywords *kws = ap_photo_get_keywords(photo);
    int count = kws ? kws->count : 0;

    if (count == 0) {
        igTextDisabled("(no keywords)");
    } else {
        float frame_h = igGetFrameHeight();
        // Scrollable child so a long keyword list doesn't push the
        // input off screen.
        igBeginChild_Str("##kwlist",
                         (ImVec2_c){ 0.0f, frame_h * 8.0f + 4.0f },
                         ImGuiChildFlags_Borders, 0);
        for (int i = 0; i < count; i++) {
            igPushID_Int(i);
            // [x] removal button to the left of the label.
            if (igSmallButton("x")) {
                ap_photo_keyword_remove(photo, kws->kw[i]);
                igPopID();
                // kws->count changed; bail out of the loop safely.
                igEndChild();
                igEnd();
                return;
            }
            if (igIsItemHovered(0)) {
                igSetTooltip("Remove keyword");
            }
            igSameLine(0.0f, igGetStyle()->ItemSpacing.x);
            igTextUnformatted(kws->kw[i], NULL);
            igPopID();
        }
        igEndChild();
    }

    igSeparator();

    float btn_w  = 50.0f;
    float avail  = igGetContentRegionAvail().x - btn_w - igGetStyle()->ItemSpacing.x;
    if (avail < 40.0f) avail = 40.0f;
    igSetNextItemWidth(avail);

    bool enter = igInputText("##kwinput", g_input, sizeof(g_input),
                             ImGuiInputTextFlags_EnterReturnsTrue,
                             NULL, NULL);
    igSameLine(0.0f, igGetStyle()->ItemSpacing.x);
    bool click = igButton("Add", (ImVec2_c){ btn_w, 0.0f });

    if ((enter || click) && g_input[0]) {
        ap_photo_keyword_add(photo, g_input);
        g_input[0] = '\0';
        igSetKeyboardFocusHere(-1);
    }

    igEnd();
}

const ap_panel panel_photo_keywords = {
    .name = "photo_keywords",
    .mode = AP_MODE_PHOTO,
    .draw = photo_keywords_draw,
};
