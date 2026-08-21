#pragma once

#include <cstdint>

namespace vocat {

/**
 * 喵伴标准脸（实机校准版）
 * 先钉死：标准开眼脸 + 开心闭眼脸；其余状态只改 Pose 参数。
 * 眼心固定 screen (100,172)/(260,172)，表情容器不随字幕移动。
 */
enum class ExpressionState : uint8_t {
    Neutral = 0,
    Winking,
    Happy,
    Sad,
    Crying,
    Angry,
    Shocked,
    Confused,
    Sleepy,
    LookLeft,
    LookRight,
    LookAround,
    Eat,
    Cool,
    Book,
    Question,
    Insert,
    Tried,
    Paishou,
    Listening,
    Speaking,
};

enum class EyeShape : uint8_t {
    Hidden = 0,
    OpenRing,   // 标准开眼：64×56 椭圆眼白 + 瞳孔（非大圆环）
    OpenOval,   // 实心椭圆（眨眼开眼侧）
    SmileArc,   // 闭眼弯弧 ∩，线宽 12～14
    SadArc,
    HalfLid,
    FlatArc,
    AngryArc,
    Focused,    // 聆听：略小开眼
};

enum class MouthShape : uint8_t {
    None = 0,
    TinySmile,       // 圆头 U 形微笑（arc）
    SmileArc,        // 更开的 U 微笑
    Flat,
    Frown,           // 下弯 U
    SmallO,          // 小圆嘴
    OpenO,
    Chew,
    SpeakingSmall,
    SpeakingMedium,
    SpeakingLarge,
};

enum class ExpressionAnim : uint8_t {
    None = 0,
    Blink,
    Breathe,
    LookScan,
    BlushPulse,
    TearDrop,
    Sparkle,
    SpeakMouth,
};

struct EyePose {
    EyeShape shape = EyeShape::OpenRing;
    int16_t offset_x = 0;
    int16_t offset_y = 0;
    int16_t width = 64;      // 开眼默认 64
    int16_t height = 56;     // 开眼默认 56（纵椭圆）
    int16_t rotation = 0;
    int16_t pupil_x = 0;
    int16_t pupil_y = 0;
    uint8_t pupil_scale = 100;  // 基准瞳孔 26px
    uint8_t line_width = 13;    // 闭眼 12～14
    uint8_t opacity = 255;
};

struct ExpressionPose {
    EyePose left_eye{};
    EyePose right_eye{};

    MouthShape mouth = MouthShape::None;
    int16_t mouth_y = 0;
    int16_t mouth_x = 0;
    int16_t mouth_width = 28;
    int16_t mouth_height = 12;

    int16_t eye_y = 0;
    int16_t eye_gap = 0;

    int16_t brow_left_rotation = 0;
    int16_t brow_right_rotation = 0;

    uint8_t blush_opacity = 0;
    bool show_tears = false;
    bool show_sparkle = false;
    bool show_emotion_mark = false;
    bool cool_shade = false;
    bool sleep_z = false;
    bool question_mark = false;

    uint8_t tear_level = 0;
    uint8_t sweat_level = 0;
    uint8_t sparkle_level = 0;

    bool blink_enabled = true;
    bool breathe_enabled = false;
    bool look_scan_enabled = false;
    bool pulse_enabled = false;

    ExpressionAnim animation = ExpressionAnim::None;

    uint32_t eye_ink = 0;
    uint32_t accent = 0;
    uint32_t blush_color = 0;
    uint32_t mouth_color = 0;
};

/**
 * 实机标准比例（360 画布，绝对屏幕坐标）
 * 表情 root 固定：y = kFaceY，内部眼心 y = kEyeCy - kFaceY
 */
struct ExpressionLayout {
    static constexpr int kCanvas = 360;

    // 表情容器（不随字幕移动）
    static constexpr int kFaceY = 96;
    static constexpr int kFaceH = 210;

    // 眼睛中心（屏幕绝对坐标）— 所有状态共用
    static constexpr int kLeftEyeCx = 100;
    static constexpr int kRightEyeCx = 260;
    static constexpr int kEyeCy = 172;

    // 标准开眼 / 闭眼
    static constexpr int kOpenEyeW = 64;
    static constexpr int kOpenEyeH = 56;
    static constexpr int kPupilD = 26;
    static constexpr int kGlintD = 6;
    static constexpr int kClosedEyeW = 68;
    static constexpr int kClosedLine = 13;

    // 嘴巴 / 腮红（屏幕）
    static constexpr int kMouthCx = 180;
    static constexpr int kMouthCy = 254;
    static constexpr int kBlushW = 28;
    static constexpr int kBlushH = 10;
    static constexpr int kBlushLCx = 62;
    static constexpr int kBlushRCx = 298;
    static constexpr int kBlushCy = 218;

    static constexpr int kLineMin = 12;
    static constexpr int kLineMax = 14;

    // root 内相对坐标
    static constexpr int EyeCyInRoot() { return kEyeCy - kFaceY; }
    static constexpr int MouthCyInRoot() { return kMouthCy - kFaceY; }
    static constexpr int BlushCyInRoot() { return kBlushCy - kFaceY; }
};

ExpressionPose PoseForState(uint8_t state_u8);
const ExpressionLayout& DefaultExpressionLayout();

}  // namespace vocat
