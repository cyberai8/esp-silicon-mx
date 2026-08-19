#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// DigitalPeopleScreen
//
// 数字人界面：纯黑背景 + 固件内 show11 分层脸（base / upper / mouth / overlay）。
// 资源在 mmap 资源分区（A:*.spng），不读 SD 卡 system/emotion/。
//
// 聊天应用仍独立读取 SD 卡 system/chat/{emotion}.eaf，互不影响。
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

    // 云端 llm.emotion 原字符串（如 laughing / loving / thinking）。
    // 屏幕不在前台时也会记住，下次 Create() 时恢复。
    static void SetEmotion(const char* emotion);

    static void ArmUtterance(int index);
    static void LoadVisemeTimeline(int index,
                                   const struct DigitalPeopleVisemeEvent* events,
                                   size_t count);
    static void ResetLipSync();
};

struct DigitalPeopleVisemeEvent {
    uint16_t time_ms;
    uint16_t duration_ms;
    uint8_t id;
};
