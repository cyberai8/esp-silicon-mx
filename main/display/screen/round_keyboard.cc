#include "round_keyboard.h"

#include "screen_util.h"

#include <cmath>
#include <new>

LV_FONT_DECLARE(lv_font_montserrat_14);

namespace {

constexpr int kRowCount = 4;
constexpr int kRowH = 32;
constexpr int kRowGap = 4;
constexpr int kCircleInset = 8;

constexpr uint32_t kColorKeyBg = 0x2A2F3A;
constexpr uint32_t kColorKeyText = 0xFFFFFF;
constexpr uint32_t kColorKeyFn = 0x3B82F6;

#define RK_KEY(w)                                                                 \
    (static_cast<lv_buttonmatrix_ctrl_t>(                                        \
        LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | (w)))
#define RK_FN(w)                                                                  \
    (static_cast<lv_buttonmatrix_ctrl_t>(                                         \
        (LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |       \
         LV_BUTTONMATRIX_CTRL_CHECKED) |                                         \
        (w)))

enum class KbMode { Lower, Upper, Number };

struct RoundKbState {
    lv_obj_t* ta = nullptr;
    lv_obj_t* rows[kRowCount] = {};
    KbMode mode = KbMode::Lower;
};

// 字母 11/10/9/6，退格在第一行右上角；数字同样把退格放在首行最右。
static const char* const kLower0[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, ""};
static const char* const kLower1[] = {
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "z", ""};
static const char* const kLower2[] = {
    "Aa", "x", "c", "v", "b", "n", "m", ".", "-", ""};
static const char* const kLower3[] = {
    "123", "@", "_", ",", " ", LV_SYMBOL_OK, ""};

static const char* const kUpper0[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, ""};
static const char* const kUpper1[] = {
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "Z", ""};
static const char* const kUpper2[] = {
    "abc", "X", "C", "V", "B", "N", "M", ".", "-", ""};
static const char* const kUpper3[] = {
    "123", "@", "_", ",", " ", LV_SYMBOL_OK, ""};

static const char* const kNum0[] = {
    "1", "2", "3", "4", "5", "6", LV_SYMBOL_BACKSPACE, ""};
static const char* const kNum1[] = {"7", "8", "9", "0", "@", "#", ""};
static const char* const kNum2[] = {"-", "_", ".", "+", ""};
static const char* const kNum3[] = {"ABC", "=", " ", LV_SYMBOL_OK, ""};

static const lv_buttonmatrix_ctrl_t kCtrl11Bksp[] = {
    RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1),
    RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_FN(1)};
static const lv_buttonmatrix_ctrl_t kCtrl10[] = {
    RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1),
    RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1)};
static const lv_buttonmatrix_ctrl_t kCtrlShift9[] = {
    RK_FN(1), RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1),
    RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1)};
static const lv_buttonmatrix_ctrl_t kCtrlLetterBottom[] = {
    RK_FN(1), RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_FN(1)};
static const lv_buttonmatrix_ctrl_t kCtrl7Bksp[] = {
    RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_FN(1)};
static const lv_buttonmatrix_ctrl_t kCtrl6[] = {
    RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1)};
static const lv_buttonmatrix_ctrl_t kCtrl4Keys[] = {
    RK_KEY(1), RK_KEY(1), RK_KEY(1), RK_KEY(1)};
static const lv_buttonmatrix_ctrl_t kCtrl4[] = {
    RK_FN(1), RK_KEY(1), RK_KEY(1), RK_FN(1)};

struct RowSpec {
    const char* const* map;
    const lv_buttonmatrix_ctrl_t* ctrl;
};

void FillMode(KbMode mode, RowSpec out[kRowCount]) {
    switch (mode) {
        case KbMode::Upper:
            out[0] = {kUpper0, kCtrl11Bksp};
            out[1] = {kUpper1, kCtrl10};
            out[2] = {kUpper2, kCtrlShift9};
            out[3] = {kUpper3, kCtrlLetterBottom};
            break;
        case KbMode::Number:
            out[0] = {kNum0, kCtrl7Bksp};
            out[1] = {kNum1, kCtrl6};
            out[2] = {kNum2, kCtrl4Keys};
            out[3] = {kNum3, kCtrl4};
            break;
        case KbMode::Lower:
        default:
            out[0] = {kLower0, kCtrl11Bksp};
            out[1] = {kLower1, kCtrl10};
            out[2] = {kLower2, kCtrlShift9};
            out[3] = {kLower3, kCtrlLetterBottom};
            break;
    }
}

int ChordWidth(int y, int size, int inset) {
    const int c = size / 2;
    const int r = c - inset;
    const int dy = y - c;
    const int inner = r * r - dy * dy;
    if (inner <= 0) {
        return 48;
    }
    return 2 * static_cast<int>(std::sqrt(static_cast<float>(inner)) + 0.5f);
}

int RowWidth(int y_top, int row_h, int size) {
    const int c = size / 2;
    const int y0 = y_top;
    const int y1 = y_top + row_h;
    const int y_tight = std::abs(y1 - c) >= std::abs(y0 - c) ? y1 : y0;
    int w = ChordWidth(y_tight, size, kCircleInset);
    if (w > size - 8) {
        w = size - 8;
    }
    if (w < 72) {
        w = 72;
    }
    return w;
}

void ApplyRowStyle(lv_obj_t* row) {
    lv_obj_remove_style_all(row);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 2, LV_PART_ITEMS);
    lv_obj_set_style_radius(row, 8, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(row, lv_color_hex(kColorKeyBg), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(row, lv_color_hex(kColorKeyText), LV_PART_ITEMS);
    lv_obj_set_style_text_font(row, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(row, lv_color_hex(kColorKeyFn),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(row, lv_color_hex(kColorKeyText),
                                LV_PART_ITEMS | LV_STATE_CHECKED);
}

void ApplyMode(RoundKbState* st) {
    RowSpec specs[kRowCount];
    FillMode(st->mode, specs);
    for (int i = 0; i < kRowCount; ++i) {
        lv_obj_t* row = st->rows[i];
        if (row == nullptr) {
            continue;
        }
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_buttonmatrix_set_map(row, specs[i].map);
        if (specs[i].ctrl != nullptr) {
            lv_buttonmatrix_set_ctrl_map(row, specs[i].ctrl);
        }
    }
}

void OnRowKey(lv_event_t* e) {
    auto* st = static_cast<RoundKbState*>(lv_event_get_user_data(e));
    lv_obj_t* row = lv_event_get_target_obj(e);
    if (st == nullptr || row == nullptr) {
        return;
    }
    const uint32_t btn_id = lv_buttonmatrix_get_selected_button(row);
    if (btn_id == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }
    const char* txt = lv_buttonmatrix_get_button_text(row, btn_id);
    if (txt == nullptr || txt[0] == '\0') {
        return;
    }

    lv_obj_t* root = lv_obj_get_parent(row);

    if (lv_strcmp(txt, "123") == 0) {
        st->mode = KbMode::Number;
        ApplyMode(st);
        return;
    }
    if (lv_strcmp(txt, "ABC") == 0 || lv_strcmp(txt, "abc") == 0) {
        st->mode = KbMode::Lower;
        ApplyMode(st);
        return;
    }
    if (lv_strcmp(txt, "Aa") == 0) {
        st->mode = KbMode::Upper;
        ApplyMode(st);
        return;
    }
    if (lv_strcmp(txt, LV_SYMBOL_OK) == 0) {
        lv_obj_send_event(root, LV_EVENT_READY, nullptr);
        if (st->ta != nullptr) {
            lv_obj_send_event(st->ta, LV_EVENT_READY, nullptr);
        }
        return;
    }

    if (st->ta == nullptr) {
        return;
    }
    if (lv_strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_delete_char(st->ta);
    } else {
        lv_textarea_add_text(st->ta, txt);
    }
}

void OnRootDeleted(lv_event_t* e) {
    auto* st = static_cast<RoundKbState*>(lv_event_get_user_data(e));
    delete st;
}

}  // namespace

lv_obj_t* RoundKeyboard_Create(lv_obj_t* parent, lv_obj_t* textarea, int /*height*/) {
    const int size = static_cast<int>(lv_obj_get_width(parent));
    auto* st = new (std::nothrow) RoundKbState();
    if (st == nullptr) {
        return nullptr;
    }
    st->ta = textarea;

    lv_obj_t* root = lv_obj_create(parent);
    screen_strip_obj_chrome(root);
    lv_obj_set_size(root, size, size);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_user_data(root, st);
    lv_obj_add_event_cb(root, OnRootDeleted, LV_EVENT_DELETE, st);

    const int kb_start_y = size / 2;
    for (int i = 0; i < kRowCount; ++i) {
        const int y = kb_start_y + i * (kRowH + kRowGap);
        const int w = RowWidth(y, kRowH, size);
        lv_obj_t* row = lv_buttonmatrix_create(root);
        st->rows[i] = row;
        ApplyRowStyle(row);
        lv_obj_set_size(row, w, kRowH);
        lv_obj_set_pos(row, (size - w) / 2, y);
        lv_obj_add_event_cb(row, OnRowKey, LV_EVENT_VALUE_CHANGED, st);
        screen_swipe_back_ignore(row, true);
    }

    ApplyMode(st);
    screen_swipe_back_ignore(root, true);
    return root;
}

void RoundKeyboard_SetTextarea(lv_obj_t* kb, lv_obj_t* textarea) {
    if (kb == nullptr) {
        return;
    }
    auto* st = static_cast<RoundKbState*>(lv_obj_get_user_data(kb));
    if (st != nullptr) {
        st->ta = textarea;
    }
}

lv_obj_t* RoundKeyboard_GetTextarea(lv_obj_t* kb) {
    if (kb == nullptr) {
        return nullptr;
    }
    auto* st = static_cast<RoundKbState*>(lv_obj_get_user_data(kb));
    return st != nullptr ? st->ta : nullptr;
}
