#pragma once

#include "vocat_styles.h"

#include <lvgl.h>

namespace vocat {

/**
 * Single-line chat / status hint under the character (home).
 * Future chat screens can grow this into a bubble list.
 */
inline lv_obj_t* CreateChatHintLabel(lv_obj_t* parent)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, "");
    ApplyTextFont(label);
    lv_obj_set_width(label, 200);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(label, lv_color_hex(kColTextSecondary), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 96);
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    return label;
}

/** Thin bottom-edge cue for “swipe up → menu”. */
inline lv_obj_t* CreateEdgeSwipeHint(lv_obj_t* parent)
{
    lv_obj_t* hint = lv_obj_create(parent);
    lv_obj_remove_style_all(hint);
    lv_obj_set_size(hint, 36, 3);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_radius(hint, 2, 0);
    lv_obj_set_style_bg_color(hint, lv_color_hex(kColTextDim), 0);
    lv_obj_set_style_bg_opa(hint, LV_OPA_60, 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    return hint;
}

/**
 * Placeholder for future multi-bubble chat.
 * Today: a rounded surface label block; screens can call this for assistant lines.
 */
inline lv_obj_t* CreateMessageBubble(lv_obj_t* parent, const char* text, bool from_assistant)
{
    lv_obj_t* bubble = lv_obj_create(parent);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_width(bubble, 240);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(from_assistant ? kColSurface : kColSurface2), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bubble, 12, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* label = lv_label_create(bubble);
    lv_label_set_text(label, text != nullptr ? text : "");
    ApplyTextFont(label);
    lv_obj_set_width(label, 216);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(kColTextPri), 0);
    return bubble;
}

}  // namespace vocat
