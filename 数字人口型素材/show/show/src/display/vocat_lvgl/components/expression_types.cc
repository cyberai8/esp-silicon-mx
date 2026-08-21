#include "expression_types.h"

namespace vocat {
namespace {

constexpr uint32_t kColEyeInk = 0xFFFFFF;
constexpr uint32_t kColBlush = 0xFF8FA3;
constexpr uint32_t kColTear = 0x3DDCFF;
constexpr uint32_t kColSparkle = 0xFFE14A;
constexpr uint32_t kColAngry = 0xFF5A67;
constexpr uint32_t kColSleepy = 0x8FA6C9;
constexpr uint32_t kColAccentSoft = 0xA9D8FF;  // 仅低透明辅光，不作眼主体

constexpr ExpressionLayout kLayout{};

EyePose MakeOpen(int16_t w, int16_t h, int16_t px = 0, int16_t py = 0, uint8_t pupil = 100)
{
    EyePose e;
    e.shape = EyeShape::OpenRing;
    e.width = w;
    e.height = h;
    e.pupil_x = px;
    e.pupil_y = py;
    e.pupil_scale = pupil;
    e.line_width = ExpressionLayout::kClosedLine;
    return e;
}

EyePose MakeClosed(EyeShape shape, int16_t w, int16_t line, int16_t rot = 0)
{
    EyePose e;
    e.shape = shape;
    e.width = w;
    e.height = 42;
    e.rotation = rot;
    e.pupil_scale = 0;
    e.line_width = static_cast<uint8_t>(line);
    return e;
}

ExpressionPose BasePose()
{
    ExpressionPose p;
    p.mouth = MouthShape::None;
    p.blink_enabled = false;
    p.eye_ink = kColEyeInk;
    return p;
}

void SyncFx(ExpressionPose& p)
{
    if (p.show_tears && p.tear_level == 0) p.tear_level = 200;
    if (p.tear_level > 0) p.show_tears = true;
    if (p.show_sparkle && p.sparkle_level == 0) p.sparkle_level = 200;
    if (p.sparkle_level > 0) p.show_sparkle = true;
    if (p.question_mark || p.sleep_z) p.show_emotion_mark = true;
}

}  // namespace

const ExpressionLayout& DefaultExpressionLayout()
{
    return kLayout;
}

ExpressionPose PoseForState(uint8_t state_u8)
{
    const auto state = static_cast<ExpressionState>(state_u8);
    ExpressionPose p = BasePose();
    constexpr int kOW = ExpressionLayout::kOpenEyeW;
    constexpr int kOH = ExpressionLayout::kOpenEyeH;
    constexpr int kCW = ExpressionLayout::kClosedEyeW;
    constexpr int kCL = ExpressionLayout::kClosedLine;

    switch (state) {
        case ExpressionState::Neutral:
            // 标准开眼脸：64×56 椭圆 + U 微笑 + 轻腮红
            p.left_eye = MakeOpen(kOW, kOH, 0, 1, 100);
            p.right_eye = MakeOpen(kOW, kOH, 0, 1, 100);
            p.mouth = MouthShape::TinySmile;
            p.mouth_width = 30;
            p.mouth_height = 14;
            p.blush_opacity = 115;  // ~45%
            p.blush_color = kColBlush;
            p.blink_enabled = true;
            p.breathe_enabled = true;
            // No pupil scan on default idle — look_scan feels like eyes "flying".
            p.look_scan_enabled = false;
            break;

        case ExpressionState::Happy:
            // 标准闭眼脸：窄弯弧 + U 微笑 + 腮红略大
            p.left_eye = MakeClosed(EyeShape::SmileArc, kCW, kCL);
            p.right_eye = MakeClosed(EyeShape::SmileArc, kCW, kCL);
            p.mouth = MouthShape::SmileArc;
            p.mouth_width = 34;
            p.mouth_height = 16;
            p.mouth_y = 2;
            p.blush_opacity = 140;
            p.blush_color = kColBlush;
            p.blink_enabled = false;
            p.breathe_enabled = true;
            break;

        case ExpressionState::Winking:
            p.left_eye = MakeOpen(kOW - 4, kOH - 2, 0, 0, 100);
            p.right_eye = MakeClosed(EyeShape::SmileArc, kCW, kCL, -30);
            p.mouth = MouthShape::TinySmile;
            p.mouth_width = 28;
            p.mouth_height = 14;
            p.blush_opacity = 130;
            p.blush_color = kColBlush;
            break;

        case ExpressionState::Listening:
            // 中等开眼，瞳孔略向内，小圆嘴；无青色粗环
            p.left_eye = MakeOpen(kOW - 2, kOH - 2, 3, 0, 100);
            p.right_eye = MakeOpen(kOW - 2, kOH - 2, -3, 0, 100);
            p.mouth = MouthShape::SmallO;
            p.mouth_width = 18;
            p.mouth_height = 16;
            p.blush_opacity = 90;
            p.blush_color = kColBlush;
            p.blink_enabled = true;
            p.breathe_enabled = true;
            break;

        case ExpressionState::Speaking:
            p.left_eye = MakeOpen(kOW, kOH, 0, 0, 100);
            p.right_eye = MakeOpen(kOW, kOH, 0, 0, 100);
            p.mouth = MouthShape::SpeakingSmall;
            p.mouth_width = 26;
            p.mouth_height = 14;
            p.blush_opacity = 100;
            p.blush_color = kColBlush;
            break;

        case ExpressionState::Sad:
            p.left_eye = MakeClosed(EyeShape::SadArc, kCW, kCL);
            p.right_eye = MakeClosed(EyeShape::SadArc, kCW, kCL);
            p.left_eye.offset_y = 2;
            p.right_eye.offset_y = 2;
            p.mouth = MouthShape::Frown;
            p.mouth_width = 28;
            p.mouth_height = 14;
            break;

        case ExpressionState::Crying:
            p.left_eye = MakeClosed(EyeShape::SadArc, kCW - 2, kCL);
            p.right_eye = MakeClosed(EyeShape::SadArc, kCW - 2, kCL);
            p.mouth = MouthShape::Frown;
            p.mouth_width = 24;
            p.mouth_height = 12;
            p.show_tears = true;
            p.tear_level = 220;
            p.accent = kColTear;
            break;

        case ExpressionState::Angry:
            p.left_eye = MakeClosed(EyeShape::AngryArc, kCW, kCL + 1, 480);
            p.right_eye = MakeClosed(EyeShape::AngryArc, kCW, kCL + 1, -480);
            p.mouth = MouthShape::Flat;
            p.mouth_width = 30;
            p.mouth_height = 6;
            p.eye_ink = kColAngry;
            p.accent = kColAngry;
            break;

        case ExpressionState::Shocked:
            // 惊讶才用偏大圆眼，但仍比旧探照灯小约 20%
            p.left_eye = MakeOpen(70, 62, 0, 0, 70);
            p.right_eye = MakeOpen(70, 62, 0, 0, 70);
            p.mouth = MouthShape::OpenO;
            p.mouth_width = 28;
            p.mouth_height = 26;
            p.show_sparkle = true;
            p.sparkle_level = 180;
            p.accent = kColSparkle;
            break;

        case ExpressionState::Confused:
            p.left_eye = MakeOpen(kOW, kOH, -6, -3, 100);
            p.right_eye = MakeOpen(kOW, kOH, 6, 4, 100);
            p.left_eye.offset_y = -3;
            p.right_eye.offset_y = 3;
            p.mouth = MouthShape::SmallO;
            p.mouth_width = 16;
            p.mouth_height = 14;
            p.mouth_x = 3;
            p.question_mark = true;
            break;

        case ExpressionState::Sleepy:
            p.left_eye = MakeClosed(EyeShape::FlatArc, kCW, 12, -40);
            p.right_eye = MakeClosed(EyeShape::FlatArc, kCW, 12, 40);
            p.mouth = MouthShape::SmallO;
            p.mouth_width = 20;
            p.mouth_height = 14;
            p.eye_ink = kColSleepy;
            p.sleep_z = true;
            p.breathe_enabled = true;
            break;

        case ExpressionState::LookLeft:
            p.left_eye = MakeOpen(kOW, kOH, -8, 0, 100);
            p.right_eye = MakeOpen(kOW, kOH, -8, 0, 100);
            p.mouth = MouthShape::TinySmile;
            p.mouth_width = 24;
            p.mouth_height = 12;
            p.blink_enabled = true;
            break;

        case ExpressionState::LookRight:
            p.left_eye = MakeOpen(kOW, kOH, 8, 0, 100);
            p.right_eye = MakeOpen(kOW, kOH, 8, 0, 100);
            p.mouth = MouthShape::TinySmile;
            p.mouth_width = 24;
            p.mouth_height = 12;
            p.blink_enabled = true;
            break;

        case ExpressionState::LookAround:
            p.left_eye = MakeOpen(kOW, kOH, 0, 0, 100);
            p.right_eye = MakeOpen(kOW, kOH, 0, 0, 100);
            p.mouth = MouthShape::TinySmile;
            p.mouth_width = 24;
            p.mouth_height = 12;
            p.blink_enabled = true;
            p.look_scan_enabled = true;
            break;

        case ExpressionState::Eat:
            p.left_eye = MakeClosed(EyeShape::SmileArc, kCW - 4, kCL);
            p.right_eye = MakeClosed(EyeShape::SmileArc, kCW - 4, kCL);
            p.mouth = MouthShape::Chew;
            p.mouth_width = 30;
            p.mouth_height = 16;
            p.blush_opacity = 120;
            p.blush_color = kColBlush;
            break;

        case ExpressionState::Cool:
            p.left_eye = MakeClosed(EyeShape::FlatArc, kCW + 2, 12, 30);
            p.right_eye = MakeClosed(EyeShape::FlatArc, kCW + 2, 12, -30);
            p.mouth = MouthShape::TinySmile;
            p.mouth_width = 26;
            p.mouth_height = 12;
            p.cool_shade = true;
            p.accent = kColAccentSoft;
            break;

        case ExpressionState::Book:
            p.left_eye = MakeOpen(kOW - 6, kOH - 6, 0, 5, 110);
            p.right_eye = MakeOpen(kOW - 6, kOH - 6, 0, 5, 110);
            p.left_eye.shape = EyeShape::Focused;
            p.right_eye.shape = EyeShape::Focused;
            p.mouth = MouthShape::None;
            break;

        case ExpressionState::Question:
            p.left_eye = MakeOpen(kOW, kOH, 0, -4, 100);
            p.left_eye.offset_y = -4;
            p.right_eye = MakeClosed(EyeShape::HalfLid, kCW - 4, 12, -40);
            p.right_eye.offset_y = 4;
            p.mouth = MouthShape::SmallO;
            p.mouth_width = 16;
            p.mouth_height = 14;
            p.question_mark = true;
            break;

        case ExpressionState::Insert:
            p.left_eye = MakeOpen(kOW - 4, kOH - 4, 0, 0, 105);
            p.right_eye = MakeOpen(kOW - 4, kOH - 4, 0, 0, 105);
            p.left_eye.shape = EyeShape::Focused;
            p.right_eye.shape = EyeShape::Focused;
            p.mouth = MouthShape::TinySmile;
            p.mouth_width = 22;
            p.mouth_height = 12;
            p.look_scan_enabled = true;
            break;

        case ExpressionState::Tried:
            p.left_eye = MakeClosed(EyeShape::FlatArc, kCW, 12);
            p.right_eye = MakeClosed(EyeShape::FlatArc, kCW, 12);
            p.mouth = MouthShape::Frown;
            p.mouth_width = 26;
            p.mouth_height = 10;
            p.sweat_level = 160;
            p.eye_ink = kColSleepy;
            break;

        case ExpressionState::Paishou:
            p.left_eye = MakeClosed(EyeShape::SmileArc, kCW + 2, kCL);
            p.right_eye = MakeClosed(EyeShape::SmileArc, kCW + 2, kCL);
            p.mouth = MouthShape::SmileArc;
            p.mouth_width = 36;
            p.mouth_height = 16;
            p.blush_opacity = 150;
            p.blush_color = kColBlush;
            p.show_sparkle = true;
            p.sparkle_level = 200;
            p.accent = kColSparkle;
            break;
    }

    SyncFx(p);
    return p;
}

}  // namespace vocat
