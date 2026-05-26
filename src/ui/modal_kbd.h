#ifndef APERTURE_UI_MODAL_KBD_H
#define APERTURE_UI_MODAL_KBD_H

#include "cimgui.h"

#include <stdbool.h>

// Shared keyboard-navigation predicates for modal popups.
//
// ap_modal_enter_pressed - Enter or Keypad Enter pressed this frame.
//   Gate the call site on "primary action is currently enabled".
// ap_modal_esc_pressed   - Escape pressed this frame. ImGui usually
//   closes popups on Esc on its own, but text-input focus can swallow
//   the event; calling this explicitly inside the modal makes Esc
//   reliable.

static inline bool ap_modal_enter_pressed(void)
{
    return igIsKeyPressed_Bool(ImGuiKey_Enter, false)
        || igIsKeyPressed_Bool(ImGuiKey_KeypadEnter, false);
}

static inline bool ap_modal_esc_pressed(void)
{
    return igIsKeyPressed_Bool(ImGuiKey_Escape, false);
}

#endif
