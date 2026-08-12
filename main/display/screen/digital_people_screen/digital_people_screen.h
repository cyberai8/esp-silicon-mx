#pragma once

#include "lvgl.h"
#include "screen_util.h"

// 改此项切换表情资源格式：".eaf" / ".jpg" / ".png" / ".sjpg"
// VoCat / 数字人动画：用 .eaf（可用 https://esp32-gif.espressif.com/ 把 GIF 转成 EAF）
#ifndef DIGITAL_PEOPLE_EMOTION_EXT
#define DIGITAL_PEOPLE_EMOTION_EXT ".eaf"
#endif

// ---------------------------------------------------------------------------
// DigitalPeopleScreen
//
// 数字人界面：纯黑背景 + 居中播放 SD 卡 system/emotion/ 下的表情动画。
// 当前默认 .eaf（Emote Animation Format）；也可用 .jpg/.png/.sjpg 静态图。
// 进入时会检查 6 个大类资源是否齐全；缺失时在屏幕中央提示用户将资源
// 包放入 SD 卡。
//
// 进入页面前检查设备是否已激活；未激活时弹出不可关闭的拦截弹窗（含返回
// 按钮），背后页面内容保持可见但不可操作，并打印日志。
//
// 对话字幕（底部单行气泡）：
//   - 用户 / 设备话术共用底部区域；显示最近一次说话方，直到对方再次说话或待机。
// ---------------------------------------------------------------------------
class DigitalPeopleScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    static bool IsActive();
    static void ShowUserMessage(const char* text);
    static void ShowSystemMessage(const char* text);
    static void ClearMessages();
    static void RefreshDeviceState();

    // 切换数字人屏正在播放的表情动画 / 静态图。
    // category 必须是 6 个大类之一：crying / happy / loving / neutral /
    // surprised / thinking。文件路径会被拼成
    //   S:/sdcard/system/emotion/{category}{ext}
    // ext 由 DIGITAL_PEOPLE_EMOTION_EXT 决定（默认 .eaf）。
    // 屏幕不在前台时也可以调用：本类会记住当前大类，下次 Create()
    // 时直接加载请求过的那一张，不会丢失。
    static void SetEmotion(const char* category);
};
