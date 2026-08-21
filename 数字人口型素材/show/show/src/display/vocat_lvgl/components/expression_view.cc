#include "expression_view.h"
#include "expression_types.h"
#include "expression_debug.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>

#include <cstring>

#define TAG "ExprView"

namespace vocat {
namespace {

void LogHeapAtExpression(const char* why, ExpressionState state)
{
    ESP_LOGI(TAG,
             "%s -> %s | sram free=%u largest=%u min=%u | psram free=%u largest=%u",
             why, ExpressionStateName(state),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

constexpr uint32_t kEyeColor = 0xFFFFFF;          // 闭眼弧线纯白
constexpr uint32_t kScleraColor = 0xF7FBFF;        // 开眼眼白（非刺眼纯白盘）
constexpr uint32_t kPupilColor = 0x182238;         // 深蓝黑瞳孔
constexpr uint32_t kMouthColor = 0xFFFFFF;
constexpr uint32_t kInkColor = 0x0A0C14;
constexpr uint32_t kEyeGlow = 0xA9D8FF;            // 仅低透明辅光
constexpr uint32_t kBlushColor = 0xFF8FA3;
constexpr uint32_t kTearColor = 0x3DDCFF;
constexpr uint32_t kSpeakBlue = 0xFFFFFF;          // 说话嘴也保持白主体
constexpr uint32_t kSparkleColor = 0xFFE14A;
[[maybe_unused]] constexpr uint32_t kAngryRed = 0xFF5A67;
constexpr uint32_t kQuestionMark = 0x8FA6C9;
constexpr uint32_t kSleepBlueGrey = 0x8FA6C9;
constexpr uint32_t kCoolBlue = 0xA9D8FF;

/**
 * 实机标准脸几何：眼心固定 112/248 @ screen y=165
 * root 固定在 FaceY，字幕独立，不挤脸。
 */
constexpr int kRootW = ExpressionLayout::kCanvas;
constexpr int kRootH = ExpressionLayout::kFaceH;
constexpr int kFaceRestY = ExpressionLayout::kFaceY;
constexpr int kLeftEyeCx = ExpressionLayout::kLeftEyeCx;
constexpr int kRightEyeCx = ExpressionLayout::kRightEyeCx;
constexpr int kEyeCenterY = ExpressionLayout::EyeCyInRoot();
constexpr int kStdRingD = ExpressionLayout::kOpenEyeW;
constexpr int kStdPupilD = ExpressionLayout::kPupilD;
constexpr int kArcSize = 96;
constexpr int kArcWidthHappy = ExpressionLayout::kClosedLine;
constexpr int kBlushW = ExpressionLayout::kBlushW;
constexpr int kBlushH = ExpressionLayout::kBlushH;
constexpr int kTearW = 8;
constexpr int kTearH = 14;
constexpr int kSweatW = 10;
constexpr int kSweatH = 16;
constexpr int kSparkleS = 12;
constexpr int kGlintS = ExpressionLayout::kGlintD;
constexpr int kShadeW = 72;
constexpr int kShadeH = 12;
constexpr int kMaxPupilLook = 8;
constexpr int kMouthArcSize = 56;

constexpr int kSmileArcY = kEyeCenterY - 26;
constexpr int kSadArcY = kEyeCenterY - (kArcSize - 26);
constexpr int16_t kArcCapStart = 215;
constexpr int16_t kArcCapEnd = 325;
constexpr int16_t kArcCupStart = 45;
constexpr int16_t kArcCupEnd = 135;

int EyeSlotLeft() { return kLeftEyeCx - ExpressionLayout::kOpenEyeW / 2; }
int EyeSlotRight() { return kRightEyeCx - ExpressionLayout::kOpenEyeW / 2; }
int EyeSlotW() { return ExpressionLayout::kOpenEyeW; }

void StyleTransparent(lv_obj_t* obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
}

void StyleArc(lv_obj_t* arc)
{
    StyleTransparent(arc);
    lv_obj_set_size(arc, kArcSize, kArcSize);
    lv_arc_set_rotation(arc, 0);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
    lv_obj_set_style_arc_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, kArcWidthHappy, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(kEyeColor), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_transform_pivot_x(arc, kArcSize / 2, 0);
    lv_obj_set_style_transform_pivot_y(arc, kArcSize / 2, 0);
}

void StyleRing(lv_obj_t* ring)
{
    // 开眼眼白：椭圆填充，无粗色环
    StyleTransparent(ring);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 0, 0);
    lv_obj_set_style_bg_color(ring, lv_color_hex(kScleraColor), 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_pivot_x(ring, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(ring, LV_PCT(50), 0);
}

void StylePupil(lv_obj_t* pupil)
{
    StyleTransparent(pupil);
    lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pupil, lv_color_hex(kPupilColor), 0);
    lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, 0);
}

void StyleBlush(lv_obj_t* dot)
{
    StyleTransparent(dot);
    lv_obj_set_size(dot, kBlushW, kBlushH);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(kBlushColor), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_40, 0);  // ~45%
}

void StyleTear(lv_obj_t* tear)
{
    StyleTransparent(tear);
    lv_obj_set_size(tear, kTearW, kTearH);
    lv_obj_set_style_radius(tear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(tear, lv_color_hex(kTearColor), 0);
    lv_obj_set_style_bg_opa(tear, LV_OPA_COVER, 0);
}

void StyleSweat(lv_obj_t* sweat)
{
    StyleTransparent(sweat);
    lv_obj_set_size(sweat, kSweatW, kSweatH);
    lv_obj_set_style_radius(sweat, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sweat, lv_color_hex(kTearColor), 0);
    lv_obj_set_style_bg_opa(sweat, LV_OPA_80, 0);
}

void StyleSparkle(lv_obj_t* s)
{
    StyleTransparent(s);
    lv_obj_set_size(s, kSparkleS, kSparkleS);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(kSparkleColor), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_pivot_x(s, kSparkleS / 2, 0);
    lv_obj_set_style_transform_pivot_y(s, kSparkleS / 2, 0);
}

void StyleShade(lv_obj_t* bar)
{
    StyleTransparent(bar);
    lv_obj_set_size(bar, kShadeW, kShadeH);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kCoolBlue), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(kInkColor), 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_COVER, 0);
}

void StyleGlint(lv_obj_t* g)
{
    StyleTransparent(g);
    lv_obj_set_size(g, kGlintS, kGlintS);
    lv_obj_set_style_radius(g, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g, lv_color_hex(kEyeColor), 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
}

void StyleFxMark(lv_obj_t* lab)
{
    StyleTransparent(lab);
    lv_label_set_text(lab, "");
    lv_obj_set_style_text_color(lab, lv_color_hex(kQuestionMark), 0);
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_opa(lab, LV_OPA_COVER, 0);
}

void Hide(lv_obj_t* obj)
{
    if (obj != nullptr) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void Show(lv_obj_t* obj)
{
    if (obj != nullptr) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

int ArcInSlotX(int slot_x)
{
    return slot_x + (EyeSlotW() - kArcSize) / 2;
}

int ClampLook(int v)
{
    if (v > kMaxPupilLook) {
        return kMaxPupilLook;
    }
    if (v < -kMaxPupilLook) {
        return -kMaxPupilLook;
    }
    return v;
}

}  // namespace

const char* ExpressionStateName(ExpressionState state)
{
    switch (state) {
        case ExpressionState::Neutral: return "neutral";
        case ExpressionState::Winking: return "winking";
        case ExpressionState::Happy: return "happy";
        case ExpressionState::Sad: return "sad";
        case ExpressionState::Crying: return "crying";
        case ExpressionState::Angry: return "angry";
        case ExpressionState::Shocked: return "shocked";
        case ExpressionState::Confused: return "confused";
        case ExpressionState::Sleepy: return "sleepy";
        case ExpressionState::LookLeft: return "look_left";
        case ExpressionState::LookRight: return "look_right";
        case ExpressionState::LookAround: return "look_around";
        case ExpressionState::Eat: return "eat";
        case ExpressionState::Cool: return "cool";
        case ExpressionState::Book: return "book";
        case ExpressionState::Question: return "question";
        case ExpressionState::Insert: return "insert";
        case ExpressionState::Tried: return "tried";
        case ExpressionState::Paishou: return "paishou";
        case ExpressionState::Listening: return "listening";
        case ExpressionState::Speaking: return "speaking";
    }
    return "?";
}

ExpressionState ExpressionStateFromEmotion(const char* emotion)
{
    // Scheme §9: avoid collapsing most cloud emotions into Happy.
    const char* key = (emotion != nullptr && emotion[0] != '\0') ? emotion : "neutral";

    if (strcmp(key, "idle") == 0 || strcmp(key, "relaxed") == 0 || strcmp(key, "neutral") == 0 ||
        strcmp(key, "microchip_ai") == 0) {
        return ExpressionState::Neutral;
    }
    if (strcmp(key, "winking") == 0 || strcmp(key, "wink") == 0 ||
        strcmp(key, "kissy") == 0 || strcmp(key, "embarrassed") == 0) {
        return ExpressionState::Winking;
    }
    if (strcmp(key, "happy") == 0 || strcmp(key, "laughing") == 0 || strcmp(key, "funny") == 0 ||
        strcmp(key, "loving") == 0) {
        return ExpressionState::Happy;
    }
    if (strcmp(key, "confident") == 0) {
        return ExpressionState::Cool;
    }
    if (strcmp(key, "delicious") == 0 || strcmp(key, "eat") == 0) {
        return ExpressionState::Eat;
    }
    if (strcmp(key, "silly") == 0) {
        return ExpressionState::Confused;
    }
    if (strcmp(key, "sad") == 0 || strcmp(key, "triangle_exclamation") == 0 ||
        strcmp(key, "circle_xmark") == 0 || strcmp(key, "cloud_slash") == 0) {
        return ExpressionState::Sad;
    }
    if (strcmp(key, "crying") == 0) {
        return ExpressionState::Crying;
    }
    if (strcmp(key, "angry") == 0) {
        return ExpressionState::Angry;
    }
    if (strcmp(key, "shocked") == 0 || strcmp(key, "surprised") == 0) {
        return ExpressionState::Shocked;
    }
    if (strcmp(key, "thinking") == 0 || strcmp(key, "question") == 0) {
        return ExpressionState::Question;
    }
    if (strcmp(key, "confused") == 0 || strcmp(key, "dizzy") == 0 ||
        strcmp(key, "nauseated") == 0) {
        return ExpressionState::Confused;
    }
    if (strcmp(key, "sleepy") == 0) {
        return ExpressionState::Sleepy;
    }
    if (strcmp(key, "tired") == 0 || strcmp(key, "tried") == 0) {
        return ExpressionState::Tried;
    }
    if (strcmp(key, "look_left") == 0) {
        return ExpressionState::LookLeft;
    }
    if (strcmp(key, "look_right") == 0) {
        return ExpressionState::LookRight;
    }
    if (strcmp(key, "look_around") == 0) {
        return ExpressionState::LookAround;
    }
    if (strcmp(key, "cool") == 0) {
        return ExpressionState::Cool;
    }
    if (strcmp(key, "book") == 0) {
        return ExpressionState::Book;
    }
    if (strcmp(key, "insert") == 0) {
        return ExpressionState::Insert;
    }
    if (strcmp(key, "paishou") == 0) {
        return ExpressionState::Paishou;
    }
    if (strcmp(key, "listening") == 0) {
        return ExpressionState::Listening;
    }
    if (strcmp(key, "speaking") == 0) {
        return ExpressionState::Speaking;
    }
    return ExpressionState::Neutral;
}

void ExpressionView::Create(lv_obj_t* parent, int size)
{
    if (parent == nullptr || root_ != nullptr) {
        return;
    }
    // Emote mirrored face is ~340–360 wide; ignore undersized callers.
    size_ = (size >= kRootW) ? size : kRootW;
    MutableDefaultExpressionScale().Configure(static_cast<uint16_t>(size_),
                                              static_cast<uint16_t>(size_));

    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, kRootW, kRootH);
    lv_obj_set_pos(root_, 0, kFaceRestY);
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    rest_y_ = kFaceRestY;

    left_arc_ = lv_arc_create(root_);
    StyleArc(left_arc_);
    right_arc_ = lv_arc_create(root_);
    StyleArc(right_arc_);

    left_ring_ = lv_obj_create(root_);
    StyleRing(left_ring_);
    right_ring_ = lv_obj_create(root_);
    StyleRing(right_ring_);

    left_pupil_ = lv_obj_create(root_);
    StylePupil(left_pupil_);
    right_pupil_ = lv_obj_create(root_);
    StylePupil(right_pupil_);

    left_blush_ = lv_obj_create(root_);
    StyleBlush(left_blush_);
    right_blush_ = lv_obj_create(root_);
    StyleBlush(right_blush_);

    left_tear_ = lv_obj_create(root_);
    StyleTear(left_tear_);
    right_tear_ = lv_obj_create(root_);
    StyleTear(right_tear_);

    sweat_ = lv_obj_create(root_);
    StyleSweat(sweat_);
    lv_obj_add_flag(sweat_, LV_OBJ_FLAG_HIDDEN);

    sparkle_l_ = lv_obj_create(root_);
    StyleSparkle(sparkle_l_);
    sparkle_r_ = lv_obj_create(root_);
    StyleSparkle(sparkle_r_);
    lv_obj_add_flag(sparkle_l_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sparkle_r_, LV_OBJ_FLAG_HIDDEN);

    shade_l_ = lv_obj_create(root_);
    StyleShade(shade_l_);
    shade_r_ = lv_obj_create(root_);
    StyleShade(shade_r_);
    lv_obj_add_flag(shade_l_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(shade_r_, LV_OBJ_FLAG_HIDDEN);

    left_glint_ = lv_obj_create(root_);
    StyleGlint(left_glint_);
    right_glint_ = lv_obj_create(root_);
    StyleGlint(right_glint_);
    lv_obj_add_flag(left_glint_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_glint_, LV_OBJ_FLAG_HIDDEN);

    fx_mark_ = lv_label_create(root_);
    StyleFxMark(fx_mark_);
    lv_obj_add_flag(fx_mark_, LV_OBJ_FLAG_HIDDEN);

    // 圆嘴 / 说话口型
    mouth_ = lv_obj_create(root_);
    StyleTransparent(mouth_);
    lv_obj_set_size(mouth_, 28, 14);
    lv_obj_set_style_radius(mouth_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mouth_, lv_color_hex(kMouthColor), 0);
    lv_obj_set_style_bg_opa(mouth_, LV_OPA_COVER, 0);
    lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);

    // U 形微笑 / 下弯嘴
    mouth_arc_ = lv_arc_create(root_);
    StyleArc(mouth_arc_);
    lv_obj_set_size(mouth_arc_, kMouthArcSize, kMouthArcSize);
    lv_obj_set_style_arc_width(mouth_arc_, 8, LV_PART_INDICATOR);
    lv_obj_set_style_transform_pivot_x(mouth_arc_, kMouthArcSize / 2, 0);
    lv_obj_set_style_transform_pivot_y(mouth_arc_, kMouthArcSize / 2, 0);
    lv_obj_add_flag(mouth_arc_, LV_OBJ_FLAG_HIDDEN);

    LayoutEyes();
    ApplyVisual();
    StartIdleAnims();
    ESP_LOGI(TAG, "face layout: Lcx=%d Rcx=%d eyeCy=%d root=%dx%d@y%d (std face)",
             kLeftEyeCx, kRightEyeCx, kEyeCenterY + kFaceRestY, kRootW, kRootH, kFaceRestY);
}

void ExpressionView::LayoutEyes()
{
    if (root_ == nullptr) {
        return;
    }
    const int left_x = EyeSlotLeft();
    const int right_x = EyeSlotRight();
    const int eye_w = EyeSlotW();
    const int blush_y = ExpressionLayout::BlushCyInRoot() - kBlushH / 2;
    const int tear_y = kEyeCenterY + 36;
    const int sparkle_y = kEyeCenterY - 28;
    const int shade_y = kEyeCenterY - kShadeH / 2;

    auto place_arc_slot = [&](lv_obj_t* obj, int slot_x, int arc_y) {
        if (obj != nullptr) {
            lv_obj_set_size(obj, kArcSize, kArcSize);
            lv_obj_set_pos(obj, ArcInSlotX(slot_x), arc_y);
        }
    };
    place_arc_slot(left_arc_, left_x, kSmileArcY);
    place_arc_slot(right_arc_, right_x, kSmileArcY);

    if (left_blush_ != nullptr) {
        lv_obj_set_size(left_blush_, kBlushW, kBlushH);
        lv_obj_set_pos(left_blush_, ExpressionLayout::kBlushLCx - kBlushW / 2, blush_y);
    }
    if (right_blush_ != nullptr) {
        lv_obj_set_size(right_blush_, kBlushW, kBlushH);
        lv_obj_set_pos(right_blush_, ExpressionLayout::kBlushRCx - kBlushW / 2, blush_y);
    }
    if (left_tear_ != nullptr) {
        lv_obj_set_pos(left_tear_, left_x + eye_w / 2 - kTearW / 2, tear_y);
    }
    if (right_tear_ != nullptr) {
        lv_obj_set_pos(right_tear_, right_x + eye_w / 2 - kTearW / 2, tear_y);
    }
    if (sweat_ != nullptr) {
        lv_obj_set_pos(sweat_, right_x + eye_w - 20, kEyeCenterY - 40);
    }
    if (sparkle_l_ != nullptr) {
        lv_obj_set_pos(sparkle_l_, left_x + 10, sparkle_y);
    }
    if (sparkle_r_ != nullptr) {
        lv_obj_set_pos(sparkle_r_, right_x + eye_w - kSparkleS - 10, sparkle_y);
    }
    if (shade_l_ != nullptr) {
        lv_obj_set_pos(shade_l_, left_x + (eye_w - kShadeW) / 2, shade_y);
    }
    if (shade_r_ != nullptr) {
        lv_obj_set_pos(shade_r_, right_x + (eye_w - kShadeW) / 2, shade_y);
    }
}

void ExpressionView::Destroy()
{
    CancelAnims();
    root_ = nullptr;
    left_arc_ = right_arc_ = nullptr;
    left_ring_ = right_ring_ = nullptr;
    left_pupil_ = right_pupil_ = nullptr;
    left_blush_ = right_blush_ = nullptr;
    left_tear_ = right_tear_ = nullptr;
    sweat_ = sparkle_l_ = sparkle_r_ = nullptr;
    shade_l_ = shade_r_ = nullptr;
    left_glint_ = right_glint_ = nullptr;
    fx_mark_ = nullptr;
    mouth_ = nullptr;
    mouth_arc_ = nullptr;
}

void ExpressionView::SetState(ExpressionState state)
{
    if (state_ == state && !transitioning_) {
        return;
    }
    if (transitioning_) {
        pending_state_ = state;
        return;
    }
    // Scheme §六.1: brief close → swap pose → reopen (skip if already closed-eye pose).
    const ExpressionPose next = PoseForState(static_cast<uint8_t>(state));
    const EyeShape from_shape = pose_.left_eye.shape;
    const bool from_closed = (from_shape == EyeShape::SmileArc || from_shape == EyeShape::SadArc ||
                              from_shape == EyeShape::FlatArc || from_shape == EyeShape::AngryArc ||
                              from_shape == EyeShape::HalfLid);
    if (!from_closed && next.blink_enabled && root_ != nullptr &&
        !lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN)) {
        pending_state_ = state;
        BeginTransition(state);
        return;
    }
    state_ = state;
    LogHeapAtExpression("expression", state_);
    ApplyVisual();
    StartIdleAnims();
}

void ExpressionView::SetStateForced(ExpressionState state)
{
    // Gesture overlays must always redraw (idle is already Happy; temp Happy was invisible).
    // SyncHomeEmotionPresentation skips calling this when state is unchanged.
    transitioning_ = false;
    pending_state_ = state;
    state_ = state;
    LogHeapAtExpression("expression forced", state_);
    ApplyVisual();
    StartIdleAnims();
}

void ExpressionView::SetMouth(MouthShape mouth)
{
    if (mouth_ == nullptr && mouth_arc_ == nullptr) {
        mouth_override_ = mouth;
        mouth_override_active_ = (mouth != MouthShape::None);
        return;
    }
    if (mouth == MouthShape::None) {
        mouth_override_ = MouthShape::None;
        mouth_override_active_ = false;
        if (pose_.mouth != MouthShape::None) {
            SetMouth(pose_.mouth);
            mouth_override_active_ = false;
            return;
        }
        Hide(mouth_);
        Hide(mouth_arc_);
        return;
    }
    mouth_override_ = mouth;
    mouth_override_active_ = true;

    int w = pose_.mouth_width > 0 ? pose_.mouth_width : 28;
    int h = pose_.mouth_height > 0 ? pose_.mouth_height : 10;
    const int mouth_x = pose_.mouth_x;
    uint32_t mouth_col = pose_.mouth_color != 0 ? pose_.mouth_color : kMouthColor;

    switch (mouth) {
        case MouthShape::TinySmile:
            SetMouthArc(kArcCupStart, kArcCupEnd, w > 0 ? w : 30, h > 0 ? h : 14, 10);
            return;
        case MouthShape::SmileArc:
            SetMouthArc(kArcCupStart, kArcCupEnd, w > 0 ? w : 34, h > 0 ? h : 16, 11);
            return;
        case MouthShape::Frown:
            SetMouthArc(kArcCapStart, kArcCapEnd, w > 0 ? w : 28, h > 0 ? h : 14, 10);
            return;
        case MouthShape::Flat:
            w = w > 0 ? w : 28;
            h = 6;
            break;
        case MouthShape::SmallO:
            w = w > 0 ? w : 18;
            h = h > 0 ? h : 16;
            break;
        case MouthShape::OpenO:
            w = w > 0 ? w : 28;
            h = h > 0 ? h : 26;
            break;
        case MouthShape::Chew:
            w = w > 0 ? w : 30;
            h = h > 0 ? h : 16;
            break;
        case MouthShape::SpeakingSmall:
            w = 26;
            h = 14;
            break;
        case MouthShape::SpeakingMedium:
            w = 30;
            h = 18;
            break;
        case MouthShape::SpeakingLarge:
            w = 34;
            h = 24;
            break;
        default:
            break;
    }

    Hide(mouth_arc_);
    if (mouth_ == nullptr) {
        return;
    }
    const int mx = ExpressionLayout::kMouthCx - w / 2 + mouth_x;
    const int my = ExpressionLayout::MouthCyInRoot() - h / 2 + pose_.mouth_y;
    lv_obj_set_size(mouth_, w, h);
    lv_obj_set_style_radius(mouth_, mouth == MouthShape::Flat ? 3 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mouth_, lv_color_hex(mouth_col), 0);
    lv_obj_set_style_bg_opa(mouth_, LV_OPA_COVER, 0);
    lv_obj_set_pos(mouth_, mx, my);
    Show(mouth_);
}

void ExpressionView::SetMouthArc(int16_t start, int16_t end, int w, int h, int line_w)
{
    if (mouth_arc_ == nullptr) {
        return;
    }
    Hide(mouth_);
    if (w < 22) {
        w = 22;
    } else if (w > 40) {
        w = 40;
    }
    if (h < 10) {
        h = 10;
    } else if (h > 20) {
        h = 20;
    }
    if (line_w < 8) {
        line_w = 8;
    } else if (line_w > 12) {
        line_w = 12;
    }
    // 用略扁的 arc 框做出圆头 U；视觉宽度 ≈ w
    const int sz = w + 20;
    const uint32_t col = pose_.mouth_color != 0 ? pose_.mouth_color : kMouthColor;
    Show(mouth_arc_);
    lv_obj_set_size(mouth_arc_, sz, sz);
    lv_obj_set_style_transform_pivot_x(mouth_arc_, sz / 2, 0);
    lv_obj_set_style_transform_pivot_y(mouth_arc_, sz / 2, 0);
    lv_arc_set_bg_angles(mouth_arc_, 0, 360);
    lv_arc_set_angles(mouth_arc_, start, end);
    lv_obj_set_style_transform_rotation(mouth_arc_, 0, 0);
    lv_obj_set_style_arc_width(mouth_arc_, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(mouth_arc_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(mouth_arc_, line_w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(mouth_arc_, lv_color_hex(col), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(mouth_arc_, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(mouth_arc_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_opa(mouth_arc_, LV_OPA_COVER, 0);
    // 把弧段中心对齐到标准嘴位（略偏上，避免贴底）
    const int mx = ExpressionLayout::kMouthCx - sz / 2 + pose_.mouth_x;
    const int my = ExpressionLayout::MouthCyInRoot() - sz / 2 + pose_.mouth_y - (h / 4);
    lv_obj_set_pos(mouth_arc_, mx, my);
}

void ExpressionView::SetEmotion(const char* emotion)
{
    SetState(ExpressionStateFromEmotion(emotion));
}

void ExpressionView::SetEmotionForced(const char* emotion)
{
    SetStateForced(ExpressionStateFromEmotion(emotion));
}

void ExpressionView::SetVisible(bool visible)
{
    if (root_ == nullptr) {
        return;
    }
    if (visible) {
        const bool was_hidden = lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
        // Only restart when becoming visible. Every SetStatus used to call SetVisible(true)
        // → StartIdleAnims → CancelAnims mid-blink, freezing scale_y≈0 so eyes vanished
        // while the listen wave kept running.
        if (was_hidden) {
            StartIdleAnims();
        } else {
            // Heal any stuck flat-eye transform without restarting blink/float.
            ResetEyeTransforms();
        }
    } else {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        StopAnimation();
    }
}

void ExpressionView::StopAnimation()
{
    CancelAnims();
    if (root_ != nullptr) {
        lv_obj_set_pos(root_, 0, kFaceRestY);
        rest_y_ = kFaceRestY;
    }
}

void ExpressionView::FreezeCurrentPose()
{
    if (root_ == nullptr) {
        return;
    }
    SetBlinkEnabled(false);
    CancelAnims();
    lv_obj_set_pos(root_, 0, kFaceRestY);
    rest_y_ = kFaceRestY;
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void ExpressionView::ResetEyeTransforms()
{
    lv_obj_t* objs[6] = {left_arc_, right_arc_, left_ring_, right_ring_, left_pupil_, right_pupil_};
    for (int i = 0; i < 6; ++i) {
        if (objs[i] == nullptr) {
            continue;
        }
        lv_obj_set_style_transform_scale(objs[i], 256, 0);
        lv_obj_set_style_transform_scale_y(objs[i], 256, 0);
    }
}

void ExpressionView::SetBlinkEnabled(bool enabled)
{
    if (blink_enabled_ == enabled) {
        if (!enabled) {
            // Re-assert: kill any in-flight blink and restore lids.
            if (blink_timer_ != nullptr) {
                lv_timer_delete(blink_timer_);
                blink_timer_ = nullptr;
            }
            blinking_ = false;
            ResetEyeTransforms();
        }
        return;
    }
    blink_enabled_ = enabled;
    if (!blink_enabled_) {
        if (blink_timer_ != nullptr) {
            lv_timer_delete(blink_timer_);
            blink_timer_ = nullptr;
        }
        blinking_ = false;
        // Delete blink scale anims only — CancelAnims would also kill float/pulse.
        lv_obj_t* lids[6] = {left_arc_, right_arc_, left_ring_, right_ring_, left_pupil_, right_pupil_};
        for (int i = 0; i < 6; ++i) {
            if (lids[i] != nullptr) {
                lv_anim_delete(lids[i], OnBlinkAnim);
            }
        }
        ResetEyeTransforms();
    } else if (root_ != nullptr && !lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN)) {
        ScheduleBlink();
    }
}

void ExpressionView::LogDiagnostics(const char* why) const
{
    if (root_ == nullptr) {
        ESP_LOGI(TAG, "diag[%s]: root=null", why != nullptr ? why : "?");
        return;
    }
    auto dump = [](const char* name, lv_obj_t* o) {
        if (o == nullptr) {
            ESP_LOGI(TAG, "  %s=null", name);
            return;
        }
        const int32_t sc = lv_obj_get_style_transform_scale_x(o, LV_PART_MAIN);
        const int32_t scy = lv_obj_get_style_transform_scale_y(o, LV_PART_MAIN);
        ESP_LOGI(TAG,
                 "  %s hid=%d xy=%d,%d wh=%d,%d opa=%d sc=%d scy=%d", name,
                 lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) ? 1 : 0, lv_obj_get_x(o), lv_obj_get_y(o),
                 lv_obj_get_width(o), lv_obj_get_height(o),
                 static_cast<int>(lv_obj_get_style_opa(o, LV_PART_MAIN)), static_cast<int>(sc),
                 static_cast<int>(scy));
    };
    ESP_LOGI(TAG, "diag[%s]: state=%s blink_en=%d use_open=%d", why != nullptr ? why : "?",
             ExpressionStateName(state_), blink_enabled_ ? 1 : 0, use_open_eyes_ ? 1 : 0);
    dump("root", root_);
    dump("Lring", left_ring_);
    dump("Rring", right_ring_);
    dump("Larc", left_arc_);
    dump("Rarc", right_arc_);
    dump("Lpup", left_pupil_);
    dump("Rpup", right_pupil_);
}

void ExpressionView::SetArcEye(lv_obj_t* arc, int16_t start, int16_t end, int16_t rot_01deg, int width,
                               int span_w)
{
    if (arc == nullptr) {
        return;
    }
    if (width < ExpressionLayout::kLineMin) {
        width = ExpressionLayout::kLineMin;
    } else if (width > ExpressionLayout::kLineMax) {
        width = ExpressionLayout::kLineMax;
    }
    if (span_w < 64) {
        span_w = 64;
    } else if (span_w > 72) {
        span_w = 72;
    }
    // ~110° 弦宽 ≈ 0.82*直径 → 直径 ≈ span/0.82
    int sz = (span_w * 125) / 100;
    if (sz < 78) {
        sz = 78;
    } else if (sz > 92) {
        sz = 92;
    }
    const uint32_t ink = pose_.eye_ink != 0 ? pose_.eye_ink : kEyeColor;
    Show(arc);
    lv_obj_set_size(arc, sz, sz);
    lv_obj_set_style_transform_pivot_x(arc, sz / 2, 0);
    lv_obj_set_style_transform_pivot_y(arc, sz / 2, 0);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_angles(arc, start, end);
    lv_obj_set_style_transform_rotation(arc, rot_01deg, 0);
    lv_obj_set_style_arc_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(ink), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_opa(arc, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_scale(arc, 256, 0);
    lv_obj_set_style_transform_scale_y(arc, 256, 0);
}

void ExpressionView::ApplyVisual()
{
    pose_ = PoseForState(static_cast<uint8_t>(state_));
    ApplyPose(pose_);
    mouth_override_active_ = false;
    mouth_override_ = MouthShape::None;
    if (pose_.mouth == MouthShape::None) {
        Hide(mouth_);
        Hide(mouth_arc_);
    } else {
        SetMouth(pose_.mouth);
        mouth_override_active_ = false;
    }
}

void ExpressionView::ApplyPose(const ExpressionPose& pose)
{
    if (root_ == nullptr) {
        return;
    }
    // Lock face root to the same screen slot every swap (cancel float drift first).
    lv_anim_delete(root_, OnFloatAnim);
    lv_obj_set_pos(root_, 0, kFaceRestY);
    rest_y_ = kFaceRestY;

    LayoutEyes();

    Hide(left_arc_);
    Hide(right_arc_);
    Hide(left_ring_);
    Hide(right_ring_);
    Hide(left_pupil_);
    Hide(right_pupil_);
    Hide(left_blush_);
    Hide(right_blush_);
    Hide(left_tear_);
    Hide(right_tear_);
    Hide(sweat_);
    Hide(sparkle_l_);
    Hide(sparkle_r_);
    Hide(shade_l_);
    Hide(shade_r_);
    Hide(left_glint_);
    Hide(right_glint_);
    Hide(fx_mark_);
    Hide(mouth_);
    Hide(mouth_arc_);

    use_open_eyes_ = false;

    const uint32_t eye_ink = pose.eye_ink != 0 ? pose.eye_ink : kEyeColor;
    const uint32_t accent = pose.accent != 0 ? pose.accent : 0;
    const uint32_t blush_col = pose.blush_color != 0 ? pose.blush_color : kBlushColor;
    const uint32_t sparkle_col = accent != 0 ? accent : kSparkleColor;

    // 标准开眼：64×56 椭圆眼白 + 24～28 瞳孔 + 6px 高光；无青色粗环
    auto place_open = [&](lv_obj_t* ring, lv_obj_t* pupil, lv_obj_t* glint, int slot_x, int ew,
                          int eh, int pupil_d, int pupil_dx, int pupil_dy, int offset_y = 0) {
        lv_anim_delete(pupil, nullptr);
        if (ew < 56) {
            ew = 56;
        } else if (ew > 72) {
            ew = 72;
        }
        if (eh < 48) {
            eh = 48;
        } else if (eh > 64) {
            eh = 64;
        }
        if (pupil_d > 0) {
            if (pupil_d < 24) {
                pupil_d = 24;
            } else if (pupil_d > 28) {
                pupil_d = 28;
            }
        }
        pupil_dx = ClampLook(pupil_dx);
        pupil_dy = ClampLook(pupil_dy);

        Show(ring);
        if (pupil_d > 0) {
            Show(pupil);
        } else {
            Hide(pupil);
            Hide(glint);
        }
        lv_obj_set_size(ring, ew, eh);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_pos(ring, slot_x + (EyeSlotW() - ew) / 2, kEyeCenterY - eh / 2 + offset_y);
        lv_obj_set_style_border_width(ring, 0, 0);
        // 极淡辅光描边（可选），主体仍是眼白填充
        if (accent == kEyeGlow) {
            lv_obj_set_style_border_width(ring, 2, 0);
            lv_obj_set_style_border_color(ring, lv_color_hex(kEyeGlow), 0);
            lv_obj_set_style_border_opa(ring, LV_OPA_30, 0);
        }
        lv_obj_set_style_bg_color(ring, lv_color_hex(kScleraColor), 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
        lv_obj_set_style_opa(ring, LV_OPA_COVER, 0);
        lv_obj_set_style_transform_scale(ring, 256, 0);
        lv_obj_set_style_transform_scale_y(ring, 256, 0);
        if (pupil_d > 0) {
            lv_obj_set_size(pupil, pupil_d, pupil_d);
            const int pupil_x = slot_x + (EyeSlotW() - pupil_d) / 2 + pupil_dx;
            const int pupil_y = kEyeCenterY - pupil_d / 2 + pupil_dy + offset_y;
            lv_obj_set_pos(pupil, pupil_x, pupil_y);
            if (pupil == left_pupil_) {
                left_pupil_base_x_ = pupil_x;
            } else if (pupil == right_pupil_) {
                right_pupil_base_x_ = pupil_x;
            }
            lv_obj_set_style_bg_color(pupil, lv_color_hex(kPupilColor), 0);
            lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_transform_scale(pupil, 256, 0);
            lv_obj_set_style_transform_scale_y(pupil, 256, 0);
            if (glint != nullptr) {
                Show(glint);
                lv_obj_set_size(glint, kGlintS, kGlintS);
                lv_obj_set_pos(glint, pupil_x + pupil_d / 5, pupil_y + pupil_d / 6);
            }
        }
    };

    auto place_disc = [&](lv_obj_t* pupil, int slot_x, int dw, int dh, int offset_y = 0) {
        if (dw < 44) {
            dw = 44;
        } else if (dw > 58) {
            dw = 58;
        }
        if (dh < 40) {
            dh = 40;
        } else if (dh > 56) {
            dh = 56;
        }
        Show(pupil);
        lv_obj_set_size(pupil, dw, dh);
        lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_pos(pupil, slot_x + (EyeSlotW() - dw) / 2, kEyeCenterY - dh / 2 + offset_y);
        lv_obj_set_style_bg_color(pupil, lv_color_hex(eye_ink), 0);
        lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, 0);
        lv_obj_set_style_transform_scale(pupil, 256, 0);
        lv_obj_set_style_transform_scale_y(pupil, 256, 0);
    };

    auto place_arc_for_shape = [&](lv_obj_t* arc, int slot_x, EyeShape shape, int span_w,
                                   int offset_y = 0) {
        int sz = (span_w * 125) / 100;
        if (sz < 78) {
            sz = 78;
        } else if (sz > 92) {
            sz = 92;
        }
        // 闭眼弧中心对齐眼心；Smile 略偏上，Sad 略偏下
        int arc_y = kEyeCenterY - sz / 2 + offset_y;
        if (shape == EyeShape::SadArc || shape == EyeShape::HalfLid) {
            arc_y += 4;
        } else {
            arc_y -= 2;
        }
        lv_obj_set_size(arc, sz, sz);
        lv_obj_set_pos(arc, slot_x + (EyeSlotW() - sz) / 2, arc_y);
    };

    auto draw_eye = [&](bool left) {
        const EyePose& eye = left ? pose.left_eye : pose.right_eye;
        const EyeShape shape = eye.shape;
        const int slot_x = left ? EyeSlotLeft() : EyeSlotRight();
        lv_obj_t* arc = left ? left_arc_ : right_arc_;
        lv_obj_t* ring = left ? left_ring_ : right_ring_;
        lv_obj_t* pupil = left ? left_pupil_ : right_pupil_;
        lv_obj_t* glint = left ? left_glint_ : right_glint_;
        const int16_t rot = eye.rotation != 0 ? eye.rotation
                                             : (left ? pose.brow_left_rotation : pose.brow_right_rotation);
        const int16_t pdx = eye.pupil_x;
        const int16_t pdy = eye.pupil_y;
        const int ew = eye.width > 0 ? eye.width : ExpressionLayout::kOpenEyeW;
        const int eh = eye.height > 0 ? eye.height : ExpressionLayout::kOpenEyeH;
        const int pupil_d =
            (eye.pupil_scale == 0) ? 0 : (kStdPupilD * static_cast<int>(eye.pupil_scale)) / 100;
        const int span_w = eye.width > 0 ? eye.width : ExpressionLayout::kClosedEyeW;
        int arc_w = eye.line_width > 0 ? static_cast<int>(eye.line_width) : kArcWidthHappy;
        if (arc_w < ExpressionLayout::kLineMin) {
            arc_w = ExpressionLayout::kLineMin;
        } else if (arc_w > ExpressionLayout::kLineMax) {
            arc_w = ExpressionLayout::kLineMax;
        }

        switch (shape) {
            case EyeShape::SmileArc:
                place_arc_for_shape(arc, slot_x, shape, span_w, eye.offset_y);
                SetArcEye(arc, kArcCapStart, kArcCapEnd, rot, arc_w, span_w);
                break;
            case EyeShape::SadArc:
                place_arc_for_shape(arc, slot_x, shape, span_w, eye.offset_y);
                SetArcEye(arc, kArcCupStart, kArcCupEnd, rot, arc_w, span_w);
                break;
            case EyeShape::HalfLid:
                place_arc_for_shape(arc, slot_x, EyeShape::SadArc, span_w, eye.offset_y + 4);
                SetArcEye(arc, 55, 125, rot, 12, span_w);
                break;
            case EyeShape::FlatArc:
                place_arc_for_shape(arc, slot_x, EyeShape::SmileArc, span_w, eye.offset_y);
                SetArcEye(arc, 245, 295, rot, 12, span_w);
                break;
            case EyeShape::AngryArc:
                place_arc_for_shape(arc, slot_x, EyeShape::SmileArc, span_w, eye.offset_y);
                if (left) {
                    SetArcEye(arc, 185, 300, rot, arc_w, span_w);
                } else {
                    SetArcEye(arc, 240, 355, rot, arc_w, span_w);
                }
                break;
            case EyeShape::OpenRing:
            case EyeShape::Focused: {
                use_open_eyes_ = true;
                int ow = ew;
                int oh = eh;
                if (shape == EyeShape::Focused) {
                    if (ow > 60) {
                        ow = 58;
                    }
                    if (oh > 52) {
                        oh = 50;
                    }
                }
                place_open(ring, pupil, glint, slot_x, ow, oh, pupil_d, pdx, pdy, eye.offset_y);
                break;
            }
            case EyeShape::OpenOval:
                use_open_eyes_ = true;
                place_disc(pupil, slot_x, eye.width, eye.height, eye.offset_y);
                break;
            case EyeShape::Hidden:
            default:
                break;
        }
    };

    draw_eye(true);
    draw_eye(false);

    if (pose.blush_opacity > 0) {
        Show(left_blush_);
        Show(right_blush_);
        // Happy 略放大腮红；透明度按 pose（~45% = 115）
        const int bw = pose.blush_opacity >= 130 ? 32 : kBlushW;
        const int bh = pose.blush_opacity >= 130 ? 12 : kBlushH;
        const int blush_y = ExpressionLayout::BlushCyInRoot() - bh / 2;
        if (left_blush_ != nullptr) {
            lv_obj_set_size(left_blush_, bw, bh);
            lv_obj_set_pos(left_blush_, ExpressionLayout::kBlushLCx - bw / 2, blush_y);
            lv_obj_set_style_bg_color(left_blush_, lv_color_hex(blush_col), 0);
            lv_obj_set_style_bg_opa(left_blush_, pose.blush_opacity, 0);
        }
        if (right_blush_ != nullptr) {
            lv_obj_set_size(right_blush_, bw, bh);
            lv_obj_set_pos(right_blush_, ExpressionLayout::kBlushRCx - bw / 2, blush_y);
            lv_obj_set_style_bg_color(right_blush_, lv_color_hex(blush_col), 0);
            lv_obj_set_style_bg_opa(right_blush_, pose.blush_opacity, 0);
        }
    }
    if (pose.tear_level > 0) {
        Show(left_tear_);
        Show(right_tear_);
        if (left_tear_ != nullptr) {
            lv_obj_set_style_bg_color(left_tear_, lv_color_hex(kTearColor), 0);
        }
        if (right_tear_ != nullptr) {
            lv_obj_set_style_bg_color(right_tear_, lv_color_hex(kTearColor), 0);
        }
    }
    if (pose.sweat_level > 0) {
        Show(sweat_);
        if (sweat_ != nullptr) {
            const uint32_t sweat_col = accent != 0 ? accent : kTearColor;
            lv_obj_set_style_bg_color(sweat_, lv_color_hex(sweat_col), 0);
        }
    }
    if (pose.question_mark || pose.sleep_z) {
        if (fx_mark_ != nullptr) {
            Show(fx_mark_);
            lv_label_set_text(fx_mark_, pose.sleep_z ? "z" : "?");
            const uint32_t mark_col =
                pose.sleep_z ? kSleepBlueGrey : (accent != 0 ? accent : kQuestionMark);
            lv_obj_set_style_text_color(fx_mark_, lv_color_hex(mark_col), 0);
            lv_obj_set_pos(fx_mark_, EyeSlotRight() + EyeSlotW() - 8, kEyeCenterY - 70);
        }
    }
    if (pose.sparkle_level > 0) {
        Show(sparkle_l_);
        Show(sparkle_r_);
        if (sparkle_l_ != nullptr) {
            lv_obj_set_style_bg_color(sparkle_l_, lv_color_hex(sparkle_col), 0);
        }
        if (sparkle_r_ != nullptr) {
            lv_obj_set_style_bg_color(sparkle_r_, lv_color_hex(sparkle_col), 0);
        }
    }
    if (pose.cool_shade) {
        Show(shade_l_);
        Show(shade_r_);
        if (shade_l_ != nullptr) {
            lv_obj_set_style_bg_color(shade_l_, lv_color_hex(kCoolBlue), 0);
        }
        if (shade_r_ != nullptr) {
            lv_obj_set_style_bg_color(shade_r_, lv_color_hex(kCoolBlue), 0);
        }
    }
}

void ExpressionView::BeginTransition(ExpressionState target)
{
    transitioning_ = true;
    pending_state_ = target;
    CancelAnims();

    // Close lids (~80ms) then swap on complete.
    lv_obj_t* targets[4] = {left_arc_, right_arc_, left_pupil_, right_pupil_};
    int n = 0;
    lv_obj_t* active[4] = {};
    for (int i = 0; i < 4; ++i) {
        if (targets[i] != nullptr && !lv_obj_has_flag(targets[i], LV_OBJ_FLAG_HIDDEN)) {
            active[n++] = targets[i];
        }
    }
    if (n == 0) {
        transitioning_ = false;
        state_ = target;
        ESP_LOGI(TAG, "expression -> %s", ExpressionStateName(state_));
        ApplyVisual();
        StartIdleAnims();
        return;
    }
    for (int i = 0; i < n; ++i) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, active[i]);
        lv_anim_set_values(&a, 256, 40);
        lv_anim_set_duration(&a, 85);  // 方案 §七：表情切换闭眼 ~65–85ms
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
        lv_anim_set_exec_cb(&a, OnTransitionAnim);
        if (i == n - 1) {
            lv_anim_set_completed_cb(&a, OnTransitionReady);
            lv_anim_set_user_data(&a, this);
        }
        lv_anim_start(&a);
    }
}

void ExpressionView::OnTransitionAnim(void* var, int32_t v)
{
    lv_obj_set_style_transform_scale_y(static_cast<lv_obj_t*>(var), v, 0);
}

void ExpressionView::OnTransitionReady(lv_anim_t* a)
{
    auto* self = static_cast<ExpressionView*>(lv_anim_get_user_data(a));
    if (self == nullptr) {
        return;
    }
    self->state_ = self->pending_state_;
    self->transitioning_ = false;
    ESP_LOGI(TAG, "expression -> %s", ExpressionStateName(self->state_));
    self->ApplyVisual();
    // Only reopen widgets that this pose actually shows (don't squash hidden layers).
    lv_obj_t* objs[6] = {self->left_arc_, self->right_arc_, self->left_ring_,
                         self->right_ring_, self->left_pupil_, self->right_pupil_};
    for (int i = 0; i < 6; ++i) {
        if (objs[i] == nullptr || lv_obj_has_flag(objs[i], LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_obj_set_style_transform_scale_y(objs[i], 40, 0);
        lv_anim_t open;
        lv_anim_init(&open);
        lv_anim_set_var(&open, objs[i]);
        lv_anim_set_values(&open, 40, 256);
        lv_anim_set_duration(&open, 100);
        lv_anim_set_path_cb(&open, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&open, OnTransitionAnim);
        lv_anim_start(&open);
    }
    self->StartIdleAnims();
}

void ExpressionView::StartIdleAnims()
{
    if (root_ == nullptr || lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    CancelAnims();
    lv_obj_set_pos(root_, 0, kFaceRestY);
    rest_y_ = kFaceRestY;

    // 呼吸与瞳孔扫描可并存；眼脉冲与呼吸互斥，避免叠太多动画
    const bool run_eye_pulse = pose_.pulse_enabled;
    if (run_eye_pulse) {
        lv_obj_t* eyes[4] = {left_arc_, right_arc_, left_ring_, right_ring_};
        const bool energetic =
            (state_ == ExpressionState::Speaking || state_ == ExpressionState::Paishou);
        const int lo = energetic ? 248 : 252;
        const int hi = energetic ? 268 : 260;
        const int dur = energetic ? 320 : 900;
        for (int i = 0; i < 4; ++i) {
            lv_obj_t* eye = eyes[i];
            if (eye == nullptr || lv_obj_has_flag(eye, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            lv_anim_t p;
            lv_anim_init(&p);
            lv_anim_set_var(&p, eye);
            lv_anim_set_values(&p, lo, hi);
            lv_anim_set_duration(&p, dur);
            lv_anim_set_playback_duration(&p, dur);
            lv_anim_set_repeat_count(&p, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&p, lv_anim_path_ease_in_out);
            lv_anim_set_exec_cb(&p, OnSpeakPulse);
            lv_anim_start(&p);
        }
    }

    if (pose_.breathe_enabled) {
        lv_anim_t f;
        lv_anim_init(&f);
        lv_anim_set_var(&f, root_);
        lv_anim_set_values(&f, rest_y_ - 1, rest_y_ + 2);
        const int32_t breathe_ms = 1800 + static_cast<int32_t>(esp_random() % 600);
        lv_anim_set_duration(&f, breathe_ms);
        lv_anim_set_playback_duration(&f, breathe_ms);
        lv_anim_set_repeat_count(&f, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&f, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&f, OnFloatAnim);
        lv_anim_start(&f);
    }

    if (pose_.look_scan_enabled) {
        // Animate a bounded offset from the pose's stored pupil center.
        const int amp = 8;
        const int scan_dur = 2000 + static_cast<int>(esp_random() % 2000);  // 2～4s
        lv_obj_t* pupils[2] = {left_pupil_, right_pupil_};
        for (int i = 0; i < 2; ++i) {
            lv_obj_t* p = pupils[i];
            if (p == nullptr || lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, p);
            lv_anim_set_user_data(&a, this);
            lv_anim_set_values(&a, -amp, amp);
            lv_anim_set_duration(&a, scan_dur);
            lv_anim_set_playback_duration(&a, scan_dur);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            lv_anim_set_custom_exec_cb(&a, OnLookAnim);
            lv_anim_start(&a);
        }
    }

    if (pose_.tear_level > 0) {
        lv_obj_t* tears[2] = {left_tear_, right_tear_};
        for (int i = 0; i < 2; ++i) {
            lv_obj_t* tear = tears[i];
            if (tear == nullptr || lv_obj_has_flag(tear, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            const int base_y = lv_obj_get_y(tear);
            lv_anim_t t;
            lv_anim_init(&t);
            lv_anim_set_var(&t, tear);
            lv_anim_set_values(&t, base_y, base_y + 16);
            lv_anim_set_duration(&t, 600 + i * 150);  // 600～900ms
            lv_anim_set_playback_duration(&t, 0);
            lv_anim_set_repeat_count(&t, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&t, lv_anim_path_ease_in);
            lv_anim_set_exec_cb(&t, OnTearAnim);
            lv_anim_start(&t);
        }
    }

    if (pose_.sweat_level > 0 && sweat_ != nullptr && !lv_obj_has_flag(sweat_, LV_OBJ_FLAG_HIDDEN)) {
        const int base_y = lv_obj_get_y(sweat_);
        lv_anim_t s;
        lv_anim_init(&s);
        lv_anim_set_var(&s, sweat_);
        lv_anim_set_values(&s, base_y, base_y + 10);
        lv_anim_set_duration(&s, 900);
        lv_anim_set_playback_duration(&s, 900);
        lv_anim_set_repeat_count(&s, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&s, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&s, OnTearAnim);
        lv_anim_start(&s);
    }

    if (pose_.sparkle_level > 0) {
        lv_obj_t* sparks[2] = {sparkle_l_, sparkle_r_};
        for (int i = 0; i < 2; ++i) {
            lv_obj_t* sp = sparks[i];
            if (sp == nullptr || lv_obj_has_flag(sp, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, sp);
            lv_anim_set_values(&a, 140, 256);
            lv_anim_set_duration(&a, 500 + i * 120);  // 500～800ms
            lv_anim_set_playback_duration(&a, 500 + i * 120);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            lv_anim_set_exec_cb(&a, OnSparkleAnim);
            lv_anim_start(&a);
        }
    }

    // Mouth envelope is owned by ExpressionController (playback pressure).
    ScheduleBlink();
}

void ExpressionView::ScheduleBlink()
{
    if (blink_timer_ != nullptr) {
        lv_timer_delete(blink_timer_);
        blink_timer_ = nullptr;
    }
    if (!blink_enabled_ || !pose_.blink_enabled || transitioning_) {
        return;
    }
    const uint32_t delay_ms = 3000 + static_cast<uint32_t>(esp_random() % 3000);  // 3～6s
    blink_timer_ = lv_timer_create(OnBlinkTimer, delay_ms, this);
    lv_timer_set_repeat_count(blink_timer_, 1);
}

void ExpressionView::CancelAnims()
{
    if (root_ != nullptr) {
        lv_anim_delete(root_, OnFloatAnim);
    }
    lv_obj_t* objs[14] = {left_arc_,  right_arc_,  left_ring_, right_ring_,
                          left_pupil_, right_pupil_, left_tear_, right_tear_, mouth_,
                          sweat_,     sparkle_l_,  sparkle_r_, shade_l_,   shade_r_};
    for (int i = 0; i < 14; ++i) {
        if (objs[i] != nullptr) {
            lv_anim_delete(objs[i], nullptr);
        }
    }
    // Blink/transition scale anims leave the last exec value — restore full lids.
    ResetEyeTransforms();
    blinking_ = false;
    if (blink_timer_ != nullptr) {
        lv_timer_delete(blink_timer_);
        blink_timer_ = nullptr;
    }
}

void ExpressionView::OnFloatAnim(void* var, int32_t v)
{
    lv_obj_set_y(static_cast<lv_obj_t*>(var), v);
}

void ExpressionView::OnBlinkAnim(void* var, int32_t v)
{
    lv_obj_set_style_transform_scale_y(static_cast<lv_obj_t*>(var), v, 0);
}

void ExpressionView::OnSpeakPulse(void* var, int32_t v)
{
    lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(var), v, 0);
}

void ExpressionView::OnTearAnim(void* var, int32_t v)
{
    lv_obj_set_y(static_cast<lv_obj_t*>(var), v);
}

void ExpressionView::OnLookAnim(lv_anim_t* anim, int32_t offset)
{
    if (anim == nullptr) {
        return;
    }
    auto* self = static_cast<ExpressionView*>(lv_anim_get_user_data(anim));
    auto* pupil = static_cast<lv_obj_t*>(anim->var);
    if (self == nullptr || pupil == nullptr) {
        return;
    }
    offset = ClampLook(offset);
    if (pupil == self->left_pupil_) {
        lv_obj_set_x(pupil, self->left_pupil_base_x_ + offset);
    } else if (pupil == self->right_pupil_) {
        lv_obj_set_x(pupil, self->right_pupil_base_x_ + offset);
    }
}

void ExpressionView::OnSparkleAnim(void* var, int32_t v)
{
    lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(var), v, 0);
}

void ExpressionView::OnBlinkAnimReady(lv_anim_t* a)
{
    auto* self = static_cast<ExpressionView*>(lv_anim_get_user_data(a));
    if (self == nullptr) {
        return;
    }
    self->blinking_ = false;
    lv_obj_t* objs[6] = {self->left_arc_, self->right_arc_, self->left_ring_, self->right_ring_,
                         self->left_pupil_, self->right_pupil_};
    for (int i = 0; i < 6; ++i) {
        if (objs[i] != nullptr) {
            lv_obj_set_style_transform_scale_y(objs[i], 256, 0);
        }
    }
    self->ScheduleBlink();
}

void ExpressionView::OnBlinkTimer(lv_timer_t* t)
{
    auto* self = static_cast<ExpressionView*>(lv_timer_get_user_data(t));
    if (self == nullptr || self->root_ == nullptr ||
        lv_obj_has_flag(self->root_, LV_OBJ_FLAG_HIDDEN) || !self->blink_enabled_) {
        return;
    }
    self->blinking_ = true;
    self->blink_timer_ = nullptr;

    lv_obj_t* targets[4] = {};
    int n = 0;
    auto add = [&](lv_obj_t* o) {
        if (o != nullptr && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) && n < 4) {
            targets[n++] = o;
        }
    };
    if (self->use_open_eyes_) {
        add(self->left_ring_);
        add(self->right_ring_);
        add(self->left_pupil_);
        add(self->right_pupil_);
    } else {
        add(self->left_arc_);
        add(self->right_arc_);
    }
    if (n == 0) {
        self->ScheduleBlink();
        return;
    }
    for (int i = 0; i < n; ++i) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, targets[i]);
        lv_anim_set_values(&a, 256, 40);
        lv_anim_set_duration(&a, 65);                 // 闭眼 65ms
        lv_anim_set_playback_delay(&a, 35);           // 保持 35ms
        lv_anim_set_playback_duration(&a, 85);        // 睁眼 85ms
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, OnBlinkAnim);
        if (i == n - 1) {
            lv_anim_set_completed_cb(&a, OnBlinkAnimReady);
            lv_anim_set_user_data(&a, self);
        }
        lv_anim_start(&a);
    }
}

}  // namespace vocat
